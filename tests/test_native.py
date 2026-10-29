"""Offline native transport, provider onboarding, and usage display checks."""
import http.client
import json
import os
import re
from pathlib import Path
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from test_router import BINARY, port, vault

calls = []
binary = b'RIFF\x00\xff\x80\r\nWAVE'
large = b'x' * (17 * 1024 * 1024)

class Upstream(BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_GET(self): self.respond()
    def do_POST(self): self.respond()
    def respond(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        calls.append((self.command, self.path, dict(self.headers), body))
        status, content_type, data = 200, 'application/json', b'{"ok":true}'
        if self.path.endswith('/audio'): content_type, data = 'audio/wav', binary
        if self.path.endswith('/large'): content_type, data = 'application/octet-stream', large
        if self.path.endswith('/events'): content_type, data = 'text/event-stream', b'data: {"x":1}\n\ndata: [DONE]\n\n'
        if self.path.endswith('/error'): status, data = 429, b'{"error":"quota"}'
        if self.path.endswith('/created'): status = 201
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

def main():
    checked = []
    def check(name, condition):
        assert condition, name
        checked.append(name)
    upstream = ThreadingHTTPServer(('127.0.0.1', 0), Upstream)
    threading.Thread(target=upstream.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='brokie-native-') as tmp:
        folder, gateway = Path(tmp), port()
        names = ['exa', 'parallel', 'keenable', 'brave', 'google', 'speechify', 'mock', 'aihorde', 'pinecone', 'compat']
        password = vault(folder, {str(i + 1): 'fixture-native-secret' for i in range(len(names) + 1)})
        env = dict(os.environ, BROKIE_PASSWORD=password)
        def cli(*args, input=None, metadata_only=False):
            child_env = dict(env)
            if metadata_only: child_env.pop('BROKIE_PASSWORD', None)
            return subprocess.run([str(BINARY), '--dir', tmp, *args], input=input, text=True,
                                  capture_output=True, check=True, env=child_env, timeout=10)
        directory = cli('providers', metadata_only=True).stdout
        check('provider directory before init', 'https://console.groq.com/keys' in directory)
        documented = set(re.findall(r'^\| `([^`]+)`', (Path(__file__).resolve().parents[1] / 'PROVIDERS.md').read_text(encoding='utf-8'), re.M))
        listed_ids = set(re.findall(r'^(\w+)\s+(?:added|needed|optional)\s', directory, re.M))
        check('all provider presets match documented sources', documented == listed_ids and len(documented) == 78)
        cfg = {'port': gateway, 'keys': [
            {'id': i + 1, 'provider': name, 'url': f'http://127.0.0.1:{upstream.server_port}/api' + ('/chat/completions' if name == 'compat' else ''),
             'native': name != 'compat', 'anonymous': name == 'aihorde'} for i, name in enumerate(names)],
            'models': [{'id': 1, 'provider': 'exa', 'model': 'not-a-chat-key'}],
            'custom': {'nested': [True, 42, {'escaped': 'preserve me'}]}}
        cfg['keys'].append({'id': len(names) + 1, 'provider': 'mock', 'url': f'http://127.0.0.1:{upstream.server_port}/other', 'native': True})
        (folder / 'config.json').write_text(json.dumps(cfg))
        (folder / 'catalog.json').write_text(json.dumps({'platforms': [{'id': 'exa'}, {'id': 'brand-new', 'name': 'New Provider'}]}))
        listed = cli('providers', metadata_only=True).stdout
        check('synced provider and configured key shown', 'added' in next(x for x in listed.splitlines() if x.startswith('exa ')) and 'synced' in next(x for x in listed.splitlines() if x.startswith('exa ')))
        check('new catalog-only provider remains visible', 'brand-new' in listed)
        cli('mode', 'reliable')
        check('config edits preserve unknown nested fields', json.loads((folder / 'config.json').read_text())['custom'] == cfg['custom'])
        proc = subprocess.Popen([str(BINARY), '--dir', tmp, 'serve'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        def request(path, body=None, content_type='application/json', auth=True):
            c = http.client.HTTPConnection('127.0.0.1', gateway, timeout=10)
            h = {'Content-Type': content_type}
            if auth: h['Authorization'] = 'Bearer test_gateway'
            c.request('GET' if body is None else 'POST', path, body, h)
            r = c.getresponse()
            out = r.status, r.getheader('Content-Type'), r.read()
            c.close()
            return out
        try:
            for _ in range(100):
                try:
                    if request('/health', auth=False)[0] == 200: break
                except OSError: time.sleep(.05)
            else: raise RuntimeError('server did not start')
            before = len(calls)
            check('native route requires gateway authentication', request('/native/exa/search', b'{}', auth=False)[0] == 401 and len(calls) == before)
            for name, header in [('exa', 'x-api-key'), ('parallel', 'x-api-key'), ('keenable', 'x-api-key'), ('brave', 'X-Subscription-Token'), ('google', 'x-goog-api-key'), ('pinecone', 'Api-Key')]:
                check(name + ' native authentication', request(f'/native/{name}/search?q=hello%20world&n=2')[0] == 200 and calls[-1][2].get(header) == 'fixture-native-secret')
            check('GET query preserved', calls[-1][1] == '/api/search?q=hello%20world&n=2')
            check('Pinecone version header', calls[-1][2].get('X-Pinecone-Api-Version') == '2026-07')
            request('/native/compat/events')
            check('existing chat keys support native paths', calls[-1][1] == '/api/events')
            request('/native/keenable/search/public', b'{"query":"test"}')
            check('Keenable application header', calls[-1][2].get('X-Keenable-Title') == 'Brokie')
            request('/native/speechify/audio', b'{"input":"test"}')
            check('Speechify version header', calls[-1][2].get('Speechify-Version') == '2026-09-08')
            request('/native/aihorde/models')
            check('AI Horde anonymous sentinel', calls[-1][2].get('Authorization') == 'Bearer 0000000000')
            upload = b'--fixture\r\nContent-Disposition: form-data; name="file"\r\n\r\n\x00\xff\r\n--fixture--\r\n'
            request('/native/mock/transcriptions', upload, 'multipart/form-data; boundary=fixture')
            check('multipart binary upload is lossless', calls[-1][3] == upload and calls[-1][2]['Content-Type'] == 'multipart/form-data; boundary=fixture')
            code, content_type, data = request('/native/mock/audio', b'{}')
            check('binary response and content type preserved', code == 200 and content_type.strip() == 'audio/wav' and data == binary)
            check('responses larger than buffer limit stream', request('/native/mock/large')[2] == large)
            check('native SSE preserved', request('/native/mock/events')[2] == b'data: {"x":1}\n\ndata: [DONE]\n\n')
            check('upstream error status and body preserved', request('/native/mock/error')[::2] == (429, b'{"error":"quota"}'))
            check('non-200 success preserved', request('/native/mock/created', b'{}')[0] == 201)
            check('unknown provider rejected', request('/native/missing/search')[0] == 404)
            request(f'/native/mock@{len(names) + 1}/search')
            check('explicit credential selects its API base', calls[-1][1] == '/other/search')
            before = len(calls)
            code = request('/v1/chat/completions', b'{"model":"auto","messages":[{"role":"user","content":"test"}]}')[0]
            check('native credentials excluded from chat routing', code >= 400 and len(calls) == before)
        finally:
            proc.terminate()
            proc.wait(timeout=5)
        cli('key', 'add', 'groq', input='fixture-added-key\n')
        current = json.loads((folder / 'config.json').read_text())
        check('provider base URL default', current['keys'][-1]['url'] == 'https://api.groq.com/openai/v1/chat/completions')
        cli('key', 'add', 'tavily', input='fixture-search-key\n')
        current = json.loads((folder / 'config.json').read_text())
        check('search preset uses native API base', current['keys'][-1]['url'] == 'https://api.tavily.com' and current['keys'][-1]['native'])
        account = '0123456789abcdef0123456789abcdef'
        cli('key', 'add', 'cloudflare', input=account + '\nfixture-cloud-token\n')
        current = json.loads((folder / 'config.json').read_text())
        check('Cloudflare account ID expands endpoint', current['keys'][-1]['url'] == f'https://api.cloudflare.com/client/v4/accounts/{account}/ai/v1/chat/completions')
        now = time.time()
        utc = time.gmtime(now)
        (folder / 'stats.json').write_text(json.dumps([{'model': 1, 'key': 1, 'updated': now,
            'minute': int(now) // 60, 'day': int(now) // 86400, 'month': utc.tm_year * 12 + utc.tm_mon - 1,
            'min_tokens': 11, 'day_tokens': 22, 'month_tokens': 33}]))
        check('token usage display needs no vault unlock', '11 / 22 / 33' in cli('stats', metadata_only=True).stdout)
    upstream.shutdown()
    print(json.dumps({'passed': len(checked), 'checks': checked}, indent=2))

if __name__ == '__main__': main()
