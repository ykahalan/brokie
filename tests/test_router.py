"""Offline protocol tests. Uses generated fixture keys, never real provider credentials."""
import contextlib
import hashlib
import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get('BROKIE_BINARY', ROOT / 'brokie'))
calls = []

class Upstream(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *args):
        pass
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        calls.append((self.path, body))
        model = body['model']
        if model == 'bad':
            self.send_response(429)
            self.send_header('Retry-After', '1')
            self.send_header('Content-Length', '2')
            self.end_headers()
            self.wfile.write(b'{}')
            return
        tools = bool(body.get('tools'))
        if body.get('stream'):
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Connection', 'close')
            self.end_headers()
            if tools:
                chunks = [
                    {'choices': [{'delta': {'tool_calls': [{'index': 0, 'id': 'call_1', 'type': 'function', 'function': {'name': 'echo', 'arguments': '{"x":'}}]}}]},
                    {'choices': [{'delta': {'tool_calls': [{'index': 0, 'function': {'arguments': '1}'}}]}}]},
                    {'choices': [{'delta': {}, 'finish_reason': 'tool_calls'}], 'usage': {'prompt_tokens': 12, 'completion_tokens': 4}},
                ]
            else:
                chunks = [{'choices': [{'delta': {'role': 'assistant'}}]}, {'choices': [{'delta': {'content': 'hello '}}]}, {'choices': [{'delta': {'content': 'world'}, 'finish_reason': 'stop'}], 'usage': {'prompt_tokens': 8, 'completion_tokens': 2}}]
            for obj in chunks:
                data = ('data: ' + json.dumps(obj) + '\n\n').encode()
                # Deliberately split JSON tokens and multibyte event boundaries.
                for offset in range(0, len(data), 7):
                    self.wfile.write(data[offset:offset + 7])
                    self.wfile.flush()
            self.wfile.write(b'data: [DONE]\n\n')
            self.wfile.flush()
            self.close_connection = True
        else:
            message = {'role': 'assistant', 'content': 'hello world'}
            if tools:
                message = {'role': 'assistant', 'content': None, 'tool_calls': [{'id': 'call_1', 'type': 'function', 'function': {'name': 'echo', 'arguments': '{"x":1}'}}]}
            response = {'id': 'chat_fixture', 'object': 'chat.completion', 'model': model, 'choices': [{'message': message, 'finish_reason': 'tool_calls' if tools else 'stop'}], 'usage': {'prompt_tokens': 12, 'completion_tokens': 4}}
            data = json.dumps(response).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]

def request(p, path, body=None, auth=True):
    c = http.client.HTTPConnection('127.0.0.1', p, timeout=10)
    h = {'Content-Type': 'application/json'}
    if auth:
        h['Authorization'] = 'Bearer test_gateway'
    c.request('GET' if body is None else 'POST', path, body=None if body is None else json.dumps(body), headers=h)
    r = c.getresponse()
    data = r.read().decode()
    result = (r.status, dict(r.getheaders()), data)
    c.close()
    return result

def vault(folder, secrets=None):
    password = 'generated-fixture-password-123'
    salt, iv = os.urandom(16), os.urandom(12)
    key = hashlib.pbkdf2_hmac('sha256', password.encode(), salt, 600000, 32)
    plain = json.dumps({'token': 'test_gateway', 'keys': secrets or {'1': 'test_provider'}}).encode()
    cipher = AESGCM(key).encrypt(iv, plain, None)
    (folder / 'vault.bin').write_bytes(b'MRP1' + salt + iv + cipher[-16:] + cipher[:-16])
    return password

def main():
    upstream = ThreadingHTTPServer(('127.0.0.1', 0), Upstream)
    threading.Thread(target=upstream.serve_forever, daemon=True).start()
    checked = []
    def check(name, condition):
        assert condition, name
        checked.append(name)
    with tempfile.TemporaryDirectory(prefix='brokie-test-') as tmp:
        folder = Path(tmp)
        password = vault(folder)
        p = port()
        cfg = {'port': p, 'mode': 'smartest', 'sticky_seconds': 0, 'keys': [{'id': 1, 'provider': 'mock', 'url': f'http://127.0.0.1:{upstream.server_port}/chat/completions'}], 'models': [
            {'id': 1, 'provider': 'mock', 'model': 'bad', 'tier': 4, 'rank': 1, 'tools': True},
            {'id': 2, 'provider': 'mock', 'model': 'good', 'tier': 3, 'rank': 2, 'tools': True},
            {'id': 3, 'provider': 'mock', 'model': 'vision', 'tier': 2, 'rank': 3, 'vision': True},
        ]}
        (folder / 'config.json').write_text(json.dumps(cfg))
        env = dict(os.environ, BROKIE_PASSWORD=password, BROKIE_OFFLINE='1')
        with (folder / 'log').open('w+') as log:
            proc = subprocess.Popen([str(BINARY), '--dir', str(folder), 'serve'], env=env, stdout=log, stderr=log)
            try:
                for _ in range(100):
                    try:
                        if request(p, '/health', auth=False)[0] == 200:
                            break
                    except OSError:
                        time.sleep(.05)
                else:
                    raise RuntimeError('server did not start')
                check('authentication', request(p, '/v1/models', auth=False)[0] == 401)
                status, _, text = request(p, '/v1/models')
                check('virtual model aliases', status == 200 and 'auto:reliable' in text)
                base = {'model': 'mock/good', 'messages': [{'role': 'user', 'content': 'hello'}]}
                status, headers, text = request(p, '/v1/chat/completions', base)
                check('OpenAI nonstream', status == 200 and json.loads(text)['choices'][0]['message']['content'] == 'hello world')
                check('served model header', headers['X-Routed-Via'] == 'mock/good')
                status, _, text = request(p, '/v1/chat/completions', dict(base, stream=True))
                check('OpenAI fragmented streaming', status == 200 and 'hello ' in text and '[DONE]' in text)
                a = {'model': 'mock/good', 'max_tokens': 100, 'system': [{'type': 'text', 'text': 'system text'}], 'messages': [{'role': 'user', 'content': [{'type': 'text', 'text': 'hello'}]}]}
                status, _, text = request(p, '/v1/messages?beta=true', a)
                check('Anthropic nonstream', status == 200 and json.loads(text)['content'][0]['text'] == 'hello world')
                check('system prompt translated', calls[-1][1]['messages'][0]['role'] == 'system')
                request(p, '/v1/messages', dict(a, messages=a['messages'] + [{'role': 'system', 'content': [{'type': 'text', 'text': 'late system reminder'}]}]))
                check('Claude mid-conversation system messages', calls[-1][1]['messages'][-1]['role'] == 'system')
                status, _, text = request(p, '/v1/messages', dict(a, stream=True))
                check('Anthropic text streaming', status == 200 and 'message_start' in text and 'text_delta' in text and 'message_stop' in text)
                tool = dict(a, tools=[{'name': 'echo', 'description': 'echo', 'input_schema': {'type': 'object', 'properties': {'x': {'type': 'integer'}}}}])
                status, _, text = request(p, '/v1/messages', tool)
                out = json.loads(text)
                check('Anthropic tool call', status == 200 and out['stop_reason'] == 'tool_use' and out['content'][0]['input'] == {'x': 1})
                status, _, text = request(p, '/v1/messages', dict(tool, stream=True))
                check('Anthropic streamed tool arguments', status == 200 and 'input_json_delta' in text and 'tool_use' in text and 'message_stop' in text)
                replay = dict(tool, messages=[{'role': 'assistant', 'content': [{'type': 'tool_use', 'id': 'call_1', 'name': 'echo', 'input': {'x': 1}}]}, {'role': 'user', 'content': [{'type': 'tool_result', 'tool_use_id': 'call_1', 'content': 'result'}]}])
                request(p, '/v1/messages', replay)
                msgs = calls[-1][1]['messages']
                check('tool result round trip', msgs[-1]['role'] == 'tool' and msgs[-1]['tool_call_id'] == 'call_1' and msgs[-2]['tool_calls'][0]['function']['arguments'] == '{"x": 1}')
                status, _, _ = request(p, '/v1/chat/completions', dict(base, model='no-such-model'))
                check('unknown explicit model does not substitute', status >= 400)
                status, _, _ = request(p, '/v1/messages', dict(a, messages=[{'role': 'user', 'content': [{'type': 'document', 'source': {}}]}]))
                check('unsupported block fails explicitly', status == 400)
                # A genuine 429 on the top model should fail over, then remain on cooldown.
                status, _, text = request(p, '/v1/chat/completions', dict(base, model='auto:smartest'))
                check('automatic route answers', status == 200)
                status, _, _ = request(p, '/v1/chat/completions', dict(base, model='mock/bad'))
                check('429 exhausts pinned route', status >= 400)
                status, _, text = request(p, '/v1/chat/completions', dict(base, model='auto:reliable'))
                check('reliable mode avoids cooldown', status == 200 and json.loads(text)['model'] != 'bad')
                vision = dict(base, model='auto:fastest', messages=[{'role': 'user', 'content': [{'type': 'image_url', 'image_url': {'url': 'data:image/png;base64,AA=='}}]}])
                status, _, text = request(p, '/v1/chat/completions', vision)
                check('vision capability gate', status == 200 and json.loads(text)['model'] == 'vision')
                # Concurrent requests exercise reservations and state writes.
                errors = []
                def concurrent():
                    try:
                        code = request(p, '/v1/chat/completions', base)[0]
                        if code != 200:
                            errors.append(code)
                    except Exception as exc:
                        errors.append(str(exc))
                threads = [threading.Thread(target=concurrent) for _ in range(2)]
                for t in threads: t.start()
                for t in threads: t.join()
                check('concurrent requests', not errors)
                proc.terminate()
                proc.wait(timeout=5)
                records = json.loads((folder / 'stats.json').read_text())
                check('statistics persisted', bool(records) and sum(r['s'] for r in records) >= 5)
                log.seek(0)
                check('credentials absent from logs', 'test_gateway' not in log.read())
            finally:
                if proc.poll() is None:
                    proc.terminate()
                    proc.wait(timeout=5)
    upstream.shutdown()
    # A fresh setup uses only the shipped C executable: no migration helper.
    with tempfile.TemporaryDirectory(prefix='brokie-fresh-') as tmp:
        env = dict(os.environ, BROKIE_PASSWORD='fresh-fixture-password-123', BROKIE_OFFLINE='1')
        def cli(*args, input=None, check_code=True):
            return subprocess.run([str(BINARY), '--dir', tmp, *args], env=env, input=input, capture_output=True, text=True, check=check_code)
        cli('init')
        cli('key', 'add', 'local', 'http://127.0.0.1:9999/v1', input='fixture-provider-secret\n')
        cli('model', 'add', 'local', 'fixture-model')
        cfg = json.loads((Path(tmp) / 'config.json').read_text())
        check('from-scratch setup', len(cfg['keys']) == 1 and len(cfg['models']) == 1)
        blob = (Path(tmp) / 'vault.bin').read_bytes()
        check('credentials encrypted on disk', b'fixture-provider-secret' not in blob and 'fixture-provider-secret' not in json.dumps(cfg))
        child = cli('run', 'python3', 'fastest', '-c', 'import os,json;print(json.dumps({"model":os.environ["OPENAI_MODEL"],"base":os.environ["OPENAI_BASE_URL"],"has_key":bool(os.environ["OPENAI_API_KEY"])}))')
        info = json.loads(child.stdout)
        check('OpenAI client launch and argument forwarding', info['model'] == 'auto:fastest' and info['has_key'] and info['base'].endswith('/v1'))
        original = (Path(tmp) / 'vault.bin').read_bytes()
        damaged = bytearray(original)
        damaged[-1] ^= 1
        (Path(tmp) / 'vault.bin').write_bytes(damaged)
        result = cli('list', check_code=False)
        check('tampered vault rejected', result.returncode != 0 and 'damaged vault' in result.stderr)
    print(json.dumps({'passed': len(checked), 'checks': checked}, indent=2))

if __name__ == '__main__':
    main()
