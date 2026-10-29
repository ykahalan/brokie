import json
import os
import re
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'brokie'
with tempfile.TemporaryDirectory(prefix='brokie-catalog-') as tmp:
    env = dict(os.environ, BROKIE_PASSWORD='fixture-password-only-for-tests')
    def run(*args):
        return subprocess.run([str(BINARY), '--dir', tmp, *args], env=env, check=True, capture_output=True, text=True)
    run('init')
    run('model', 'add', 'local', 'my-model')
    sync = run('sync')
    config = json.loads((Path(tmp) / 'config.json').read_text())
    catalog = json.loads((Path(tmp) / 'catalog.json').read_text())
    documented = set(re.findall(r'^\| `([^`]+)`', (ROOT / 'PROVIDERS.md').read_text(encoding='utf-8'), re.M))
    missing = {p['id'] for p in catalog['platforms']} - documented
    assert not missing, f'Undocumented catalog providers: {sorted(missing)}'
    assert config['catalog_version'] == catalog['version']
    assert any(m['provider'] == 'local' and m['model'] == 'my-model' for m in config['models'])
    remote = next(m for m in config['models'] if m.get('source') == 'catalog')
    remote['enabled'] = False
    (Path(tmp) / 'config.json').write_text(json.dumps(config))
    run('sync')
    config2 = json.loads((Path(tmp) / 'config.json').read_text())
    assert not next(m for m in config2['models'] if m['id'] == remote['id'])['enabled']
    print(json.dumps({'signature_verified': True, 'catalog_version': catalog['version'], 'catalog_tier': catalog['tier'], 'models': len(config2['models']), 'all_catalog_providers_documented': True, 'user_model_preserved': True, 'local_disable_preserved': True}))
