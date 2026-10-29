"""Local-only signed catalog refresh, hot replacement, metadata and credential health checks."""
import base64
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from test_router import ROOT, port, request, vault

signer = Ed25519PrivateKey.generate()
calls = []
slow_started, slow_release = threading.Event(), threading.Event()
state = {'version': '2026.01.01', 'model': 'first', 'failure': '', 'recovered': False}
providers = {'schema': 1, 'providers': [dict(id='newfixture', url='https://fixture.invalid/v1',
             signup='https://fixture.invalid/signup', native=False, anonymous=False, health_path='/models')]}


class Upstream(BaseHTTPRequestHandler):
    def log_message(self, *args): pass

    def respond(self, status, body, signature=None):
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        if signature: self.send_header('X-Catalog-Signature', signature)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        calls.append(('GET', self.path, self.headers.get('Authorization')))
        if self.path == '/providers':
            self.respond(200, json.dumps(providers).encode())
        elif self.path == '/catalog':
            snapshot = dict(state)
            rows = [dict(platform='valid', modelId=snapshot['model'], modality='text', enabled=True,
                         supportsTools=True, intelligenceRank=1, sizeLabel='Frontier'),
                    dict(platform='valid', modelId='disabled', modality='text', enabled=True)]
            body = json.dumps({'version': snapshot['version'], 'models': rows}).encode()
            if snapshot['failure'] == 'malformed': body = b'{"models":'
            if snapshot['failure'] == 'overflow':
                rows[0]['contextWindow'] = 1e100
                body = json.dumps({'version': snapshot['version'], 'models': rows}).encode()
            signature = base64.b64encode(signer.sign(body)).decode()
            if snapshot['failure'] == 'signature': body += b' '
            self.respond(503 if snapshot['failure'] == 'network' else 200, body, signature)
        else:
            source = self.path.split('/')[1]
            status = {'public': 200, 'limited': 429, 'down': 503}.get(source)
            if status is None:
                status = 200 if self.headers.get('Authorization') == 'Bearer fixture-key' else 401
                if source == 'invalid' and not state['recovered']: status = 401
            self.respond(status, b'{}')

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        calls.append(('POST', self.path, body['model']))
        if body['messages'][0]['content'] == 'slow':
            slow_started.set()
            slow_release.wait(10)
        response = {'id': 'fixture', 'object': 'chat.completion', 'model': body['model'],
                    'choices': [{'message': {'role': 'assistant', 'content': 'ok'}, 'finish_reason': 'stop'}]}
        self.respond(200, json.dumps(response).encode())


def wait_for(predicate, label, seconds=15):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            if predicate(): return
        except (OSError, ValueError): pass
        time.sleep(.05)
    raise AssertionError('Timed out: ' + label)


def compile_fixture(binary, base):
    env = dict(os.environ)
    if os.uname().sysname == 'Darwin':
        prefixes = [subprocess.check_output(['brew', '--prefix', p], text=True).strip()
                    for p in ('openssl@3', 'curl')]
        env['PKG_CONFIG_PATH'] = ':'.join(p + '/lib/pkgconfig' for p in prefixes) + ':' + env.get('PKG_CONFIG_PATH', '')
    cflags = shlex.split(env['BROKIE_TEST_CFLAGS']) if 'BROKIE_TEST_CFLAGS' in env else shlex.split(
        subprocess.check_output(['pkg-config', '--cflags', 'libcurl', 'openssl'], env=env, text=True))
    libs = shlex.split(env['BROKIE_TEST_LIBS']) if 'BROKIE_TEST_LIBS' in env else shlex.split(
        subprocess.check_output(['pkg-config', '--libs', 'libcurl', 'openssl'], env=env, text=True))
    pem = signer.public_key().public_bytes(serialization.Encoding.PEM,
                                           serialization.PublicFormat.SubjectPublicKeyInfo).decode()
    definitions = {'BROKIE_CATALOG_KEY': pem, 'BROKIE_CATALOG_URL': base + '/catalog',
                   'BROKIE_PROVIDERS_URL': base + '/providers'}
    subprocess.run(['cc', '-std=c11', '-Os', '-Wall', '-Wextra', '-Wpedantic',
                    *shlex.split(env.get('BROKIE_TEST_SANITIZERS', '')), *cflags,
                    *['-D' + k + '=' + json.dumps(v) for k, v in definitions.items()],
                    str(ROOT / 'brokie.c'), '-o', str(binary), *libs, '-lm', '-pthread'], check=True)


def main():
    checked = []
    def check(label, condition):
        assert condition, label
        checked.append(label)
    upstream = ThreadingHTTPServer(('127.0.0.1', 0), Upstream)
    threading.Thread(target=upstream.serve_forever, daemon=True).start()
    base = f'http://127.0.0.1:{upstream.server_port}'
    with tempfile.TemporaryDirectory(prefix='brokie-auto-') as tmp:
        root = Path(tmp)
        binary = root / 'brokie-fixture'
        compile_fixture(binary, base)
        folder = root / 'state'
        folder.mkdir()
        names = ['valid', 'invalid', 'public', 'limited', 'down', 'native', 'anon']
        password = vault(folder, {str(i + 1): 'fixture-key' for i in range(len(names))})
        cfg = {'port': port(), 'refresh_seconds': 1, 'health_seconds': 2, 'sticky_seconds': 1800,
               'custom_option': 'keep', 'models': [
                   {'id': 10, 'provider': 'valid', 'model': 'old', 'source': 'catalog'},
                   {'id': 11, 'provider': 'valid', 'model': 'disabled', 'enabled': False,
                    'context': 1234, 'overrides': ['context'], 'source': 'catalog'},
                   {'id': 20, 'provider': 'valid', 'model': 'local', 'source': 'user'},
                   {'id': 21, 'provider': 'invalid', 'model': 'recovery', 'source': 'user'}],
               'keys': [dict(id=i + 1, provider=n, url=base + '/' + n + ('/chat/completions' if n not in ('native', 'anon') else ''),
                             native=n == 'native', anonymous=n == 'anon') for i, n in enumerate(names)]}
        (folder / 'config.json').write_text(json.dumps(cfg))
        env = dict(os.environ, BROKIE_PASSWORD=password)
        env.pop('BROKIE_OFFLINE', None)
        def cli(*args, unlock=True, offline=False):
            child_env = dict(env)
            if not unlock: child_env.pop('BROKIE_PASSWORD', None)
            if offline: child_env['BROKIE_OFFLINE'] = '1'
            return subprocess.run([str(binary), '--dir', str(folder), *args], env=child_env,
                                  capture_output=True, text=True, check=True, timeout=20)
        def config(): return json.loads((folder / 'config.json').read_text())
        def health(): return {r['id']: r for r in json.loads((folder / 'health.json').read_text())}
        def chat(model, message='hello'):
            return request(cfg['port'], '/v1/chat/completions',
                           {'model': model, 'messages': [{'role': 'user', 'content': message}]})
        with (root / 'log').open('w+') as log:
            proc = subprocess.Popen([str(binary), '--dir', str(folder), 'serve'], env=env, stdout=log, stderr=log)
            try:
                wait_for(lambda: config().get('catalog_version') == state['version'], 'automatic signed sync')
                wait_for(lambda: all(r['checked'] for r in health().values()), 'automatic key probes')
                check('live catalog without restart', proc.poll() is None and chat('valid/first')[0] == 200)
                check('catalog removes obsolete model', all(m['model'] != 'old' for m in config()['models']))
                disabled = next(m for m in config()['models'] if m['id'] == 11)
                check('disabled catalog model and override preserved', not disabled['enabled'] and disabled['context'] == 1234)
                check('user models and custom settings preserved', config()['custom_option'] == 'keep' and
                      any(m['id'] == 20 and m['source'] == 'user' for m in config()['models']))
                expected = {1: 1, 2: 2, 3: 6, 4: 4, 5: 5, 6: 8, 7: 7}
                check('safe probe health classifications', all(health()[k]['health'] == v for k, v in expected.items()))
                check('invalid key excluded from routing', chat('invalid/recovery')[0] == 503)
                check('no generation for key probes', sum(c[0] == 'POST' for c in calls) == 1)
                check('no probes for native-only and anonymous keys', not any(c[1].startswith(('/native/', '/anon/')) for c in calls))
                output = cli('health', unlock=False).stdout
                check('cached health without vault unlock or secrets', 'unverified' in output and 'invalid' in output and 'fixture-key' not in output)
                listing = cli('providers', unlock=False).stdout
                check('new provider metadata loaded automatically', 'newfixture' in listing and 'https://fixture.invalid/signup' in listing)
                check('metadata never changes configured credential URLs', config()['keys'] == cfg['keys'])
                wait_for(lambda: (folder / 'stats.json').exists(), 'saved usage')
                stats = json.loads((folder / 'stats.json').read_text())
                first_id = next(m['id'] for m in config()['models'] if m['model'] == 'first')
                first_tokens = next(s['month_tokens'] for s in stats if s['model'] == first_id)
                check('usage fallback when upstream omits counts', first_tokens > 0)
                # A metadata download can finish during a request. Applying it waits for that request.
                with ThreadPoolExecutor(max_workers=1) as pool:
                    pending = pool.submit(chat, 'valid/first', 'slow')
                    check('active request reaches upstream', slow_started.wait(5))
                    state.update(version='2026.01.02', model='second')
                    wait_for(lambda: sum(c[1] == '/catalog' for c in calls) >= 2, 'next catalog fetch')
                    time.sleep(.3)
                    check('catalog activation waits for active request', config()['catalog_version'] == '2026.01.01')
                    slow_release.set()
                    check('active request completes across refresh', pending.result()[0] == 200)
                wait_for(lambda: config()['catalog_version'] == state['version'], 'hot swap')
                check('new live model routes and removed pin fails', chat('valid/second')[0] == 200 and chat('valid/first')[0] == 503)
                check('retained local model routes after index remapping', chat('valid/local')[0] == 200)
                check('stats remain attached to stable model IDs', any(s['model'] == 20 and s['month_tokens'] > 0
                      for s in json.loads((folder / 'stats.json').read_text())))
                for failure in ('signature', 'network', 'malformed', 'overflow'):
                    state.update(version='2026.01.03', model='broken', failure=failure)
                    before = sum(c[1] == '/catalog' for c in calls)
                    wait_for(lambda: sum(c[1] == '/catalog' for c in calls) > before, failure + ' refresh')
                    time.sleep(.15)
                    check(failure + ' retains working catalog', config()['catalog_version'] == '2026.01.02' and chat('valid/second')[0] == 200)
                state.update(version='2025.01.01', failure='')
                before = sum(c[1] == '/catalog' for c in calls)
                wait_for(lambda: sum(c[1] == '/catalog' for c in calls) > before, 'rollback attempt')
                time.sleep(.15)
                check('older signed catalog rejected', config()['catalog_version'] == '2026.01.02')
                state.update(version='2026.01.04', model='recovered', recovered=True)
                wait_for(lambda: config()['catalog_version'] == state['version'], 'catalog recovery')
                wait_for(lambda: health()[2]['health'] == 1, 'key recovery')
                check('automatic recovery without restart', proc.poll() is None and chat('valid/recovered')[0] == 200 and chat('invalid/recovery')[0] == 200)
                check('metadata fetches never receive provider secrets', all(c[2] is None for c in calls if c[1] in ('/catalog', '/providers')))
                check('manual health check uses only GET', 'healthy' in cli('check').stdout)
                proc.terminate()
                proc.wait(timeout=5)
                before = len(calls)
                cli('check', offline=True)
                cli('providers', unlock=False, offline=True)
                check('offline mode disables all probes and metadata downloads', len(calls) == before)
                # A malformed metadata update must retain the cached directory.
                original = (folder / 'providers.json').read_bytes()
                providers['providers'][0]['url'] = 'http://insecure.invalid'
                cli('sync')
                check('invalid provider metadata retains cache', (folder / 'providers.json').read_bytes() == original)
            except BaseException:
                log.seek(0)
                print(log.read())
                raise
            finally:
                slow_release.set()
                if proc.poll() is None: proc.terminate()
                proc.wait(timeout=5)
    upstream.shutdown()
    print(json.dumps({'checks_passed': len(checked), 'checks': checked}))


if __name__ == '__main__': main()
