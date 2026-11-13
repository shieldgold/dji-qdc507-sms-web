"""Download pinned public build inputs to the computer, no device access."""
import concurrent.futures
import hashlib
import json
from pathlib import Path
import sys
import urllib.request

dest = Path(sys.argv[1]).resolve()
dest.mkdir(parents=True, exist_ok=True)
inputs = json.loads(Path(__file__).with_name('inputs.json').read_text())
def get(item):
    relative = Path(item['file'])
    if relative.is_absolute() or '..' in relative.parts: raise ValueError('unsafe input path')
    path = dest/relative
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        partial = path.with_name(path.name+'.part')
        with urllib.request.urlopen(item['url'], timeout=40) as src, partial.open('xb') as out:
            while True:
                data = src.read(1024*1024)
                if not data: break
                out.write(data)
        data = partial.read_bytes()
        if len(data) != item['bytes'] or hashlib.sha256(data).hexdigest() != item['sha256']:
            raise ValueError('download size/hash mismatch: '+str(relative))
        partial.rename(path)
    data = path.read_bytes()
    if len(data) != item['bytes'] or hashlib.sha256(data).hexdigest() != item['sha256']:
        raise ValueError('existing input mismatch: '+str(relative))
    return str(relative)

with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    for name in pool.map(get, inputs): print('VERIFIED', name)
deps = [i for i in inputs if i['file'].startswith('compiler-debs/')]
(dest/'compiler-debs/SHA256SUMS').write_text(''.join(i['sha256']+'  '+Path(i['file']).name+'\n' for i in deps))
