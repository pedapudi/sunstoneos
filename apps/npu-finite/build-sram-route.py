#!/usr/bin/env python3
"""Build the SRAM-routing variant of the finite numerical NPU image.

The variant is the default image of apps/npu-finite/build.py with two changes:
sram-route.c routes the shared SRAM to the NPU before the startup sequencer
releases the NPU resets and back to the CPU after local stop, and refuses NPU
register access unless the SRAM is routed and startup is READY;
sram-route-el3.c restores the CPU routing before the ROM return. The builder first runs build.py unchanged into
OUTPUT/default, then compiles the two files with the flags of the objects they
sit beside, gives the variant its own build identity and relinks the default
link command with the two objects and these linker redirections:

  --wrap=a733_npu_startup_init --wrap=a733_npu_platform_native_write
  --wrap=npu_service_activate --wrap=npu_convolution_stop_poll
  --wrap=a733_npu_native_operations --wrap=npu_finite_rom_return

No other object changes, so the default image and every other image are
unchanged. The execution profile stays npu-finite-return-v1, so the finite
host prepares and validates the variant as it does the default image.
"""
import argparse, hashlib, json, shlex, shutil, subprocess, sys
from pathlib import Path

WRAPS = ('a733_npu_startup_init', 'a733_npu_platform_native_write', 'npu_service_activate',
         'npu_convolution_stop_poll', 'a733_npu_native_operations', 'npu_finite_rom_return')

p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('--evidence', type=Path, required=True, help='new output directory')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
e = a.evidence.resolve()
e.mkdir(parents=True, exist_ok=False)
default = e / 'default'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save(name, value):
    (e / name).write_text(json.dumps(value, indent=2) + '\n')


def run(name, command, cwd=None):
    with (e / (name + '.log')).open('w') as log:
        subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)


run('default-build', [sys.executable, str(root / 'apps/npu-finite/build.py'), '--evidence', str(default)])
base = json.loads((default / 'input-identities.json').read_text())
entries = [x for x in json.loads((default / 'native/compile_commands.json').read_text())
           if 'a733-npu-convolution-single-core.dir/' in x['command']]
commands = []
objects = e / 'objects'
objects.mkdir()


def compile_like(suffix, source, name):
    entry = next(x for x in entries if x['file'].endswith(suffix))
    command = shlex.split(entry['command'])
    dest = objects / (name + '.obj')
    command[command.index('-o') + 1] = str(dest)
    command[command.index('-c') + 1] = str(source)
    command += ['-MD', '-MF', str(dest) + '.d']
    commands.append(command)
    run(name, command, cwd=entry['directory'])
    return dest


route = compile_like('/apps/npu-convolution/native.c', root / 'apps/npu-finite/sram-route.c', 'sram-route')
route_el3 = compile_like('/apps/storage-read-single-core/monitor.c', root / 'apps/npu-finite/sram-route-el3.c',
                         'sram-route-el3')
identity = dict(base['identity_inputs'])
identity['sram_route_sources'] = {str(x.relative_to(root)): sha(x) for x in
                                  (root / 'apps/npu-finite' / n for n in
                                   ('sram-route.c', 'sram-route-el3.c', 'build-sram-route.py'))}
identity['sram_route_wraps'] = list(WRAPS)
build_id = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
source = e / 'build-id.c'
source.write_text('const unsigned char a733_smp_build_id[32]={' +
                  ','.join('0x' + build_id[i:i + 2] for i in range(0, 64, 2)) + '};\n')
build_obj = compile_like('/npu-single-core/build-id.c', source, 'build-id')

base_commands = json.loads((default / 'commands.json').read_text())
link = next(c for c in base_commands if c and c[-1] == str(default / 'image.elf'))
old_build = str(default / 'objects/build-id.obj')
assert link.count(old_build) == 1, 'The default link command names no single build-id object.'
newlink = []
for value in link:
    if value == old_build:
        value = str(build_obj)
    elif value.startswith('-Wl,-Map,'):
        value = '-Wl,-Map,' + str(e / 'image.map')
    newlink.append(value)
newlink[-1] = str(e / 'image.elf')
newlink[-2:-2] = [str(route), str(route_el3)] + ['-Wl,--wrap=' + n for n in WRAPS]
commands.append(newlink)
run('link', newlink, cwd=default / 'native')
copy = ['/usr/lib/llvm-21/bin/llvm-objcopy', '-O', 'binary', str(e / 'image.elf'), str(e / 'image.bin')]
commands.append(copy)
run('binary', copy)
save('commands.json', commands)

inputs = {Path(x) for x in base['compiled_inputs']} | {Path(__file__).resolve(), source}
for dep in objects.glob('*.obj.d'):
    text = dep.read_text().replace('\\\n', ' ')
    inputs.update(Path(x).resolve() for x in shlex.split(text.split(':', 1)[1]))
closure = {}
for i, path in enumerate(sorted(inputs)):
    dest = e / 'compiled-inputs' / str(i) / path.name
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(path, dest)
    closure[str(path)] = {'sha256': sha(path), 'snapshot': str(dest.relative_to(e))}
save('input-identities.json', {'compiled_inputs': closure, 'identity_inputs': identity, 'build_id': build_id,
                               'default_image_sha256': sha(default / 'image.elf'),
                               'elf_sha256': sha(e / 'image.elf'), 'binary_sha256': sha(e / 'image.bin'),
                               'physical_access': False, 'physical_admission': False,
                               'note': 'Default finite image plus the two SRAM-routing objects and six linker '
                                       'redirections. Root review and real session and board evidence remain '
                                       'mandatory.'})
save('manifest.json', {'execution_profile': identity['profile'], 'build_id': build_id,
                       'final_elf': sha(e / 'image.elf'), 'binary_sha256': sha(e / 'image.bin'),
                       'physical_admission': False})
print('Built NPU finite SRAM-routing image:', sha(e / 'image.elf'))
