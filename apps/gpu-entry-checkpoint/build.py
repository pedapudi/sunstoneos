#!/usr/bin/env python3
"""Build a distinct pre-provider checkpoint from the frozen finite GPU image."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
BASE_HASH = '2ab2cb1b3bac150d5eb695793ab0eb39bb453afc7fbd7ecb2f595ebfca3fd08d'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline', type=Path, default=Path.home() / '.cache/sunstoneos/evidence/gpu-returning-repeat-fix-20261019')
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
base = args.baseline.resolve(); out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
build = out / 'build'; objects = build / 'objects'; objects.mkdir(parents=True)
overlay = out / 'inputs'; overlay.mkdir()

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def require(condition, message):
    if not condition:
        raise SystemExit(message)

def save(name, value):
    (out / name).write_text(json.dumps(value, indent=2) + '\n')

def relative(name):
    for marker in ('/sunstoneos-worktrees/', '/provider-inputs/'):
        if marker in name:
            tail = name.split(marker, 1)[1]
            return tail.split('/', 1)[1] if marker == '/sunstoneos-worktrees/' else tail
    return None

require(sha(base / 'build/return.elf') == BASE_HASH, 'Wrong frozen GPU image')
for name, expected in {'object-source-closure.json': 'bbea6583e0c7b13fb800f0eb7be4a1de8755f8ccd0b07bf9801716ce355d1c07', 'commands.json': '60c2e793656322b9cc5e47409e3355815a07b3bf3d8bd2f3b2b99b55bceecea8', 'build/link.ld': 'cdc6a0b23181f9be99024091ca8032a24cebc0d409b1a148a3c27578fa268ca8'}.items():
    require(sha(base / name) == expected, 'Changed frozen build metadata: ' + name)
closure = json.loads((base / 'object-source-closure.json').read_text())
bindings = []
for value in closure['unique_inputs'].values():
    source = Path(value['frozen'])
    require(sha(source) == value['sha256'], 'Changed frozen source: ' + str(source))
    name = relative(value['original'])
    if name:
        dest = overlay / name
        if dest.exists():
            require(sha(dest) == value['sha256'], 'Ambiguous frozen header: ' + name)
        else:
            dest.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(source, dest)
    else:
        # External headers keep their accepted include paths and exact identity.
        original = Path(value['original'])
        if original.suffix == '.h':
            require(original.exists() and sha(original) == value['sha256'], 'Changed external header: ' + str(original))
        dest = source
    bindings.append(dict(original=value['original'], frozen=str(dest), sha256=value['sha256']))

commands = []
def run(command, label):
    commands.append([str(x) for x in command]); save('commands.json', commands)
    with (out / (label + '.log')).open('w') as log:
        subprocess.run(command, cwd=overlay, stdout=log, stderr=subprocess.STDOUT, check=True)

for name, target in [('runtime.c', 'apps/storage-read-single-core/runtime.c'),
                     ('return.h', 'apps/gpu-finite/return.h'), ('exit.c', 'apps/gpu-finite/exit.c'),
                     ('return.c', 'apps/gpu-finite/return.c')]:
    path = overlay / target
    run(['patch', '--batch', '--fuzz=0', str(path), str(HERE / (name + '.patch'))], 'patch-' + name)
for name in ('checkpoint.c', 'checkpoint.h'):
    shutil.copy2(HERE / name, overlay / 'apps/gpu-finite' / name)

old = json.loads((base / 'commands.json').read_text())
flags = old[1][:old[1].index('-MMD')]
oldroot = '/home/sunil/git/sunstoneos-worktrees/gpu-finite-adapter'
flags = [x.replace(oldroot, str(overlay)) for x in flags]
flags += ['-include', str(overlay / 'apps/gpu-render-single-core/boot.h')]
inputs = {str(p.relative_to(ROOT)): sha(p) for p in HERE.iterdir()
          if p.name in ('build.py', 'checkpoint.c', 'checkpoint.h') or p.suffix == '.patch'}
tools = {flags[0]: sha(flags[0]), old[-1][2].split('=', 1)[1]: sha(old[-1][2].split('=', 1)[1])}
identity = dict(baseline=BASE_HASH, sources=inputs, flags=flags, tools=tools,
                checkpoint='CPU entry and empty pools; no provider construction')
build_id = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
(build / 'build-id.c').write_text('const unsigned char a733_smp_build_id[32]={' +
    ','.join('0x' + build_id[i:i+2] for i in range(0, 64, 2)) + '};\n')
replacements = {}
for name, source in [('runtime.c', overlay / 'apps/storage-read-single-core/runtime.c'),
                     ('exit.c', overlay / 'apps/gpu-finite/exit.c'),
                     ('return.c', overlay / 'apps/gpu-finite/return.c'),
                     ('checkpoint.c', overlay / 'apps/gpu-finite/checkpoint.c'),
                     ('build-id.c', build / 'build-id.c')]:
    obj = objects / (name + '.o')
    run([*flags, '-MMD', '-MF', str(obj) + '.d', '-o', str(obj), '-c', str(source)], 'compile-' + name)
    if name != 'checkpoint.c':
        matched = [v for v in closure['linked_objects'] if v['source'].endswith('/' + name)]
        require(len(matched) == 1, 'Ambiguous replaced unit: ' + name)
        replacements[matched[0]['object']] = str(obj)

lookup = {v['object']: v for v in closure['linked_objects']}
linked = []; link = []
for value in old[-1]:
    if value.endswith(('.obj', '.o')):
        entry = lookup[value]
        require(sha(value) == entry['sha256'], 'Changed retained object: ' + value)
        if value in replacements:
            dest = Path(replacements[value])
        else:
            dest = objects / Path(value).name; shutil.copy2(value, dest)
        link.append(str(dest)); linked.append(dict(original=value, object=str(dest), sha256=sha(dest), reused=value not in replacements))
    elif value.startswith('-Wl,-T,'):
        shutil.copy2(base / 'build/link.ld', build / 'link.ld'); link.append('-Wl,-T,' + str(build / 'link.ld'))
    elif value.startswith('-Wl,-Map,'):
        link.append('-Wl,-Map,' + str(build / 'checkpoint.map'))
    else:
        link.append(value)
link[link.index('-o') + 1] = str(build / 'checkpoint.elf')
link.insert(link.index('-o'), str(objects / 'checkpoint.c.o'))
linked.append(dict(original=None, object=str(objects / 'checkpoint.c.o'),
                   sha256=sha(objects / 'checkpoint.c.o'), reused=False))
run(link, 'link')
dependencies = {}
for dep in objects.glob('*.d'):
    for name in shlex.split(dep.read_text().replace('\\\n', ' ').split(':', 1)[1]):
        dependencies[name] = sha(name)
save('compiled-inputs.json', dependencies); save('frozen-bindings.json', bindings)
save('image-objects.json', linked); save('identity.json', dict(identity=identity, build_id=build_id))
save('manifest.json', dict(target='sunstone-a733-gpu-entry-checkpoint', execution_profile='gpu-entry-memory-checkpoint-v1',
    workload='entry-memory-ready-no-providers', build_id=build_id, final_elf=sha(build / 'checkpoint.elf'),
    baseline_elf=BASE_HASH, physical_access=False, physical_eligible=False,
    root_architecture_and_admission_review_required=True))
print('Built checkpoint:', sha(build / 'checkpoint.elf'))
