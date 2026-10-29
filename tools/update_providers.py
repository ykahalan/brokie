"""Maintain public provider metadata from FreeLLMAPI. Development/CI only."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import json
from pathlib import Path
import re
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = 'https://raw.githubusercontent.com/tashfeenahmed/freellmapi/main/'
NATIVE = {'aclide', 'sail', 'aihorde', 'speechify', 'freeai'}
BASES = {
    'google': 'https://generativelanguage.googleapis.com/v1beta/openai',
    'cloudflare': 'https://api.cloudflare.com/client/v4/accounts/YOUR_ACCOUNT_ID/ai/v1',
    'zhipu': 'https://api.z.ai/api/paas/v4', 'aihorde': 'https://oai.aihorde.net/v1',
}
PROBES = {
    'electronhub': '/user/me', 'xkiro': '/usage', 'simplellm': '/rate-limit',
    'blockrun': '/account', 'kenari': '/account/balance',
    'inferbase': '//api/v1/inference/credits', 'tavily': '/usage',
    'firecrawl': '/team/credit-usage', 'linkup': '/credits/balance',
    'speechify': '/workspaces/current/entitlements',
    'llmtr': '-', 'iteracompute': '-', 'kiosapi': '-', 'freeai': '-', 'modelscope': '-',
}
SEARCH = [
    ('tavily', 'Tavily', 'https://app.tavily.com', 'https://api.tavily.com'),
    ('exa', 'Exa', 'https://dashboard.exa.ai/api-keys', 'https://api.exa.ai'),
    ('firecrawl', 'Firecrawl', 'https://www.firecrawl.dev/app/api-keys', 'https://api.firecrawl.dev/v2'),
    ('linkup', 'Linkup', 'https://app.linkup.so', 'https://api.linkup.so/v1'),
    ('parallel', 'Parallel', 'https://platform.parallel.ai', 'https://api.parallel.ai/v1'),
    ('serpsearch', 'SerpSearch', 'https://serpsearch.com', 'https://api.serpsearch.com/api/v1'),
    ('keenable', 'Keenable', 'https://app.keenable.ai', 'https://api.keenable.ai/v1'),
    ('brave', 'Brave Search', 'https://api-dashboard.search.brave.com', 'https://api.search.brave.com/res/v1'),
]

def fetch(path):
    with urlopen(Request(UPSTREAM + path, headers={'User-Agent': 'Brokie-metadata/1'}), timeout=30) as r:
        return r.read(2_000_000).decode('utf-8')

def validate(rows):
    assert 0 < len(rows) <= 256
    assert len({p['id'] for p in rows}) == len(rows), 'Duplicate provider IDs'
    for p in rows:
        assert re.fullmatch(r'[a-z0-9_-]{1,95}', p['id'])
        for key in ('url', 'signup'):
            assert p[key].startswith('https://') and len(p[key]) < 2048
            assert not any(c in p[key] for c in '\r\n ')
        assert isinstance(p['native'], bool) and isinstance(p['anonymous'], bool)
        probe = p['health_path']
        assert len(probe) < 256 and (probe.startswith('/') or probe == '-')
        assert '://' not in probe and not any(c in probe for c in '\r\n')

def build():
    ui = fetch('client/src/components/keys/shared.tsx')
    index = fetch('server/src/providers/index.ts')
    rows = []
    bases = dict(re.findall(r"platform:\s*'([^']+)'[\s\S]*?baseUrl:\s*'([^']*)'", index))
    platforms = re.findall(r"\{\s*value:\s*'([^']+)',\s*label:\s*'([^']+)',\s*url:\s*'([^']*)'([^}]*)\}", ui)
    platforms = [p for p in platforms if p[0] != 'custom']
    assert len(platforms) >= 50, 'Provider parser returned an unexpectedly small roster; review upstream changes'
    wanted = [p[0] for p in platforms if p[0] not in bases]
    with ThreadPoolExecutor(max_workers=6) as pool:
        adapters = dict(zip(wanted, pool.map(lambda p: fetch('server/src/providers/' + p + '.ts'), wanted)))
    for pid, label, signup, flags in platforms:
        source = adapters.get(pid, '')
        match = re.search(r"(?:const\s+(?:\w*BASE_URL|API_BASE)|baseUrl:)\s*(?:=\s*)?'(https:[^']+)'", source)
        base = BASES.get(pid) or bases.get(pid) or (match[1] if match else None)
        if not base: raise ValueError('API base needs review: ' + pid)
        native = pid in NATIVE or (pid not in BASES and pid not in bases and '/chat/completions' not in source and 'extends OpenAICompatProvider' not in source)
        probe = PROBES.get(pid, '-' if native else '/models')
        if re.search(r'supportsKey(?:Validation|Polling)[\s\S]{0,150}?return false', source): probe = '-'
        rows.append(dict(id=pid, name=label.split(' (')[0], url=base, signup=signup,
                         native=native, anonymous='keyless: true' in flags, health_path=probe,
                         note=label.partition('(')[2].rstrip(')') or 'Check current free-plan eligibility.'))
    search_ids = re.search(r'SEARCH_PLATFORMS[^=]*=\s*\[([^]]+)\]', fetch('server/src/lib/search-platforms.ts'))
    assert search_ids and set(re.findall(r"'([^']+)'", search_ids[1])) == {p[0] for p in SEARCH}, 'New search provider needs endpoint review'
    rows += [dict(id=pid, name=name, url=base, signup=signup, native=True, anonymous=pid == 'keenable',
                  health_path=PROBES.get(pid, '-'), note='Check current free credits and billing requirements.')
             for pid, name, signup, base in SEARCH]
    rows.append(dict(id='pinecone', name='Pinecone', url='https://api.pinecone.io', signup='https://app.pinecone.io',
                     native=True, anonymous=False, health_path='-', note='Cataloged embeddings; confirm Starter allowance.'))
    rows.sort(key=lambda p: p['id'])
    validate(rows)
    return rows

def embed_defaults(rows, verify=False):
    source = ROOT / 'brokie.c'
    text = source.read_text(encoding='utf-8')
    table = 'static const Provider providers[] = {\n' + ',\n'.join(
        '    {' + ', '.join([json.dumps(p[k]) for k in ('id', 'url', 'signup', 'health_path')] +
                           [str(int(p['native'])), str(int(p['anonymous']))]) + '}' for p in rows) + '\n};'
    pattern = r'static const Provider providers\[\] = \{\n.*?\n\};'
    if verify:
        assert re.search(pattern, text, re.S)[0] == table, 'Bundled provider defaults differ from providers.json'
    else:
        source.write_text(re.sub(pattern, lambda _: table, text, count=1, flags=re.S), encoding='utf-8')

def main():
    args = argparse.ArgumentParser()
    args.add_argument('--verify', action='store_true')
    options = args.parse_args()
    target = ROOT / 'providers.json'
    old = json.loads(target.read_text(encoding='utf-8')) if target.exists() else {}
    if options.verify:
        validate(old['providers'])
        embed_defaults(old['providers'], verify=True)
        documented = set(re.findall(r'^\| `([^`]+)`', (ROOT / 'PROVIDERS.md').read_text(encoding='utf-8'), re.M))
        assert {p['id'] for p in old['providers']} <= documented
        print(f"Validated {len(old['providers'])} provider records")
        return
    rows = build()
    embed_defaults(rows)
    if rows == old.get('providers'): print('Provider metadata unchanged'); return
    payload = {'schema': 1, 'updated': datetime.now(timezone.utc).date().isoformat(), 'providers': rows}
    target.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    doc = ROOT / 'PROVIDERS.md'
    text = doc.read_text(encoding='utf-8')
    start = text.index('| ID |')
    end = text.index('\n\nCustom OpenAI-compatible', start)
    table = ['| ID | Register / get a key | API base | Access | Notes |', '|---|---|---|---|---|']
    table += [f"| `{p['id']}` | [{p['name']}]({p['signup']}) | `{p['url']}` | {'native' if p['native'] else 'chat'} | {p['note'].replace('|', '/')} |" for p in rows]
    text = text[:start] + '\n'.join(table) + text[end:]
    text = re.sub(r'^\d+ providers from[^\n]*', f"{len(rows)} providers from FreeLLMAPI's key manager, search registry, and catalog extras; updated {payload['updated']}.", text, flags=re.M)
    doc.write_text(text, encoding='utf-8')
    print(f'Updated {len(rows)} providers')

if __name__ == '__main__': main()
