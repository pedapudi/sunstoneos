#!/usr/bin/env python3
"""Check data-selected NPU catalogues against the decoder and the real SDK bind.

The native image's includes and definitions supply its OS/FreeRTOS types.
Device calls abort if metadata or malformed request admission reaches them.
No device is opened and no image is linked.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import shlex
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'checks'))
from harness import SANITIZE, environment
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--artifacts', type=Path, required=True)
p.add_argument('--native-flags', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--cc', default='/usr/bin/clang-21')
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
spec = importlib.util.spec_from_file_location('os_npu_catalogue', ROOT / 'tools/npu-test-models.py')
embed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(embed)
flags = a.native_flags.read_text()
def value(name):
    return shlex.split(next(line.split('=', 1)[1] for line in flags.splitlines() if line.startswith(name + ' =')))
args = value('C_DEFINES') + value('C_INCLUDES')
old_root = next(arg[2:-len('/apps/os')] for arg in args if arg.startswith('-I') and arg.endswith('/apps/os'))
args = [arg.replace(old_root, str(ROOT)) for arg in args]
# Host mocks retain note packing; ARM SVC assembly is checked by the native ELF.
args += ['-USUNSTONE_CRUMBS', '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-ffunction-sections', '-fdata-sections', *SANITIZE]
commands, results = [], []
def run(command):
    commands.append(command)
    result = subprocess.run(command, capture_output=True, text=True, env=environment())
    with (a.output / 'commands.log').open('a') as log:
        log.write(shlex.join(command) + '\n' + result.stdout + result.stderr)
    if result.returncode:
        raise SystemExit(result.stdout + result.stderr)

sources = ['apps/os/npu-client.c', 'runtime/npu_app.c', 'drivers/npu/nbg_model.c',
           'drivers/npu/nbg_assembly.c', 'drivers/npu/nbg_relocation.c', 'drivers/npu/nbg_checksum.c',
           'drivers/storage/storage_digest.c']
objects = []
for i, source in enumerate(sources):
    output = a.output / f'source-{i}.o'
    run([a.cc, *args, '-c', str(ROOT / source), '-o', str(output)])
    objects.append(str(output))

variants = [('one', ['shufflenet-v2-published']),
            ('two', ['dual-u8', 'shufflenet-v2-published']),
            ('reordered-three', ['shufflenet-v2-published', 'dual-i8', 'conv8'])]
for label, names in variants:
    directory = a.output / label
    directory.mkdir()
    model_list = directory / 'models.json'
    model_list.write_text(json.dumps({'npu_models': names}) + '\n')
    generated = directory / 'models.c'
    # Exercise the same CLI that CMake uses: the only changing input is data.
    run([sys.executable, str(ROOT / 'tools/npu-test-models.py'), '--artifacts', str(a.artifacts),
         '--models', str(model_list), '--model-name', 'shufflenet-v2-published', '--all', '--output', str(generated)])
    executable = directory / 'check'
    run([a.cc, *args, str(ROOT / 'apps/npu-sdk/catalogue-check.c'), str(generated),
         *objects, '-Wl,--gc-sections', '-o', str(executable)])
    for fault in (0, 1, 2):
        run([str(executable), str(len(names)), str(fault)])
    results.append(dict(catalogue=names, selected_index=names.index('shufflenet-v2-published'),
                        decoded_metadata=True, wrong_count_refused=True, wrong_size_refused=True))
    # The selected-only profile retains a local index of zero even when the
    # selected model occurs later in the caller's source list.
    selected = directory / 'selected.c'
    embed.generate(a.artifacts, selected, model_list=model_list, model_name='shufflenet-v2-published')
    executable = directory / 'selected-check'
    run([a.cc, *args, str(ROOT / 'apps/npu-sdk/catalogue-check.c'), str(selected),
         *objects, '-Wl,--gc-sections', '-o', str(executable)])
    run([str(executable), '1', '0'])

(a.output / 'result.json').write_text(json.dumps(dict(result='pass', cases=results, commands=commands), indent=1) + '\n')
print('PASS NPU SDK: data catalogues with one, two and reordered three models; decoded metadata and request geometry checks')
