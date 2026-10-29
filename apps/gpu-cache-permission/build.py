#!/usr/bin/env python3
"""Freeze a CPU-only read-only output cache diagnostic; never accesses a board."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from returning_build import digest, copy_closure
BASE_HASH = 'e786516e077e7375dfaee16fa570b09b707c31ef5975fc0525214e439ff3c9ec'

def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Shared source checkpoint changed: ' + old)
    return text.replace(old, new)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--baseline', type=Path, default=Path.home() / '.cache/sunstoneos/evidence/sol-gpu-exact-sum-prep-20261028/bundle')
    a = p.parse_args(); out = a.output.resolve(); base = a.baseline.resolve()
    out.mkdir(parents=True, exist_ok=False)
    record = json.loads((base / 'build.json').read_text())
    assert digest(base / 'image.elf') == record['image_sha256'] == BASE_HASH
    oldroot = str(Path(record['compile'][0][record['compile'][0].index('-c') + 1]).parents[2])
    flags = [x.replace(oldroot, str(ROOT)) for x in record['compile'][0][:record['compile'][0].index('-MD')]]
    # Selected base was physically accepted with the current monitor cleanup.
    monitor = next(x for x in record['new_objects'] if x['source'] == 'platform/a733/monitor.c')
    assert digest(ROOT / monitor['source']) == monitor['source_sha256'], 'Monitor changed since accepted return'
    b = out / 'build'; b.mkdir()
    flags += ['-ffile-prefix-map=' + str(b) + '=gpu-cache-permission']
    monitor_s = replace_once((ROOT / 'platform/a733/monitor.S').read_text(),
                             '    bl monitor_init\n', '    bl monitor_init\n    bl gpu_cache_permission_initialize\n')
    platform = replace_once((ROOT / 'platform/a733/platform.c').read_text(),
        '    configASSERT(a733_memory_boot_uncached(level2));',
        '    level2[12] = UINT64_C(0x41800000) | UINT64_C(0x00600000000007c5);\n'
        '    level2[13] = UINT64_C(0x41a00000) | UINT64_C(0x00600000000007c5);\n'
        '    configASSERT(a733_memory_boot_uncached(level2));')
    platform += '\nuint64_t gpu_cache_permission_descriptor(unsigned i) { return i < 2 ? level2[12+i] : 0; }\n'
    (b / 'monitor.S').write_text(monitor_s); (b / 'platform.c').write_text(platform)
    linker = replace_once((ROOT / 'platform/a733/link.ld').read_text(),
        '    /DISCARD/ :',
        '    ASSERT(__uncached_pool_end <= 0x41800000, \"image/stack/heap/pools overlap RO output\")\n'
        '    .readonly_output 0x41800000 (NOLOAD) : { . += 0x400000; } > RAM\n'
        '    ASSERT(ADDR(.readonly_output) + SIZEOF(.readonly_output) <= 0x42200000, \"RO output exceeds owned RAM\")\n'
        '    /DISCARD/ :')
    (b / 'link.ld').write_text(linker)
    units = [('main', ROOT / 'apps/gpu-cache-permission/main.c'),
             ('init', ROOT / 'apps/gpu-cache-permission/init.c'),
             ('memory', ROOT / 'arch/aarch64/device_memory.c'),
             ('monitor', b / 'monitor.S'), ('platform', b / 'platform.c')]
    commands = []; new = []
    for name, source in units:
        obj = b / (name + '.obj')
        cmd = flags + ['-I' + str(ROOT / 'apps/gpu-cache-permission'), '-Werror', '-MD', '-MF', str(obj) + '.d', '-c', str(source), '-o', str(obj)]
        commands.append(cmd)
        subprocess.run(cmd, cwd=ROOT, check=True)
        new.append({'source': str(source), 'object': str(obj), 'sha256': digest(obj)})
    # Bind inherited object bytes to the physically accepted base; identify replacements
    # by their actual defining symbols rather than by repeated local symbol names.
    accepted = record['new_objects'] + record['added_objects'] + record['reused_objects']
    replacements = {'kernel_main': b / 'main.obj', 'monitor_entry': b / 'monitor.obj',
                    'platform_init': b / 'platform.obj', 'aarch64_device_cache': b / 'memory.obj'}
    objects = {}; reused = []; seen = set()
    for item in accepted:
        source = Path(item['object']); assert digest(source) == item['sha256']
        names = {line.split()[-1] for line in subprocess.check_output(['/usr/lib/llvm-21/bin/llvm-nm', '--defined-only', str(source)], text=True).splitlines() if line.split()}
        matches = names & replacements.keys()
        assert len(matches) <= 1
        if matches:
            name = matches.pop(); assert name not in seen; seen.add(name)
            objects[str(source)] = str(replacements[name])
        else:
            dest = b / ('reused-' + item['sha256'] + '.obj')
            shutil.copy2(source, dest); objects[str(source)] = str(dest)
            reused.append({'object': str(dest), 'sha256': digest(dest), 'accepted': item})
    assert seen == replacements.keys()
    link = []
    for value in record['link']:
        if value in objects: value = objects[value]
        elif value.startswith('-Wl,-T,'): value = '-Wl,-T,' + str(b / 'link.ld')
        elif value == '-o': pass
        elif value.endswith('/image.elf'): value = str(out / 'image.elf')
        elif value.startswith('-Wl,-Map,'): value = '-Wl,-Map,' + str(out / 'image.map')
        link.append(value.replace(oldroot, str(ROOT)))
    link.insert(link.index('-o'), str(b / 'init.obj'))
    # No uncached pool is needed by this CPU-only client.
    link = [x for x in link if x != '-Wl,--defsym=__uncached_pool_size=2097152']
    commands.append(link); subprocess.run(link, cwd=ROOT, check=True)
    binary = ['/usr/lib/llvm-21/bin/llvm-objcopy', '-O', 'binary', str(out / 'image.elf'), str(out / 'probe.bin')]
    commands.append(binary); subprocess.run(binary, check=True)
    closure = copy_closure(out, b.glob('*.obj.d'), extra=[__file__, ROOT / 'apps/gpu-cache-permission/probe.h', ROOT / 'apps/gpu-cache-permission/scratch.py', ROOT / 'apps/gpu-cache-permission/host.py', ROOT / 'platform/a733/platform.c', ROOT / 'platform/a733/monitor.S', ROOT / 'platform/a733/link.ld'])
    names = subprocess.check_output(['/usr/lib/llvm-21/bin/llvm-nm', '--defined-only', str(out / 'image.elf')], text=True)
    assert 'a733_gpu_platform_init' not in names and 'pvr_firmware_begin' not in names
    shutil.copytree(base / 'host', out / 'host')
    (out / 'host/tools').chmod(0o755)
    (out / 'host/tools/check-a733-kernel.py').unlink()
    shutil.copy2(ROOT / 'apps/gpu-cache-permission/host.py', out / 'host/tools/check-a733-kernel.py')
    runner_path = out / 'host/tools/returning-kernel.py'
    runner = replace_once(runner_path.read_text(),
        'def main(argv=None, observe=None, validate_result=None):',
        (ROOT / 'apps/gpu-cache-permission/scratch.py').read_text() + '\n\ndef main(argv=None, observe=None, validate_result=None):')
    runner = replace_once(runner,
        '                words = transaction("readl", "0x20", "readl", "0x24", "readl", "0x28",\n'
        '                                    "readl", "0x3c", "readl", "0x40")',
        '                words = permission_rom_words(transaction, args.evidence, record, save)')
    runner = replace_once(runner,
        '    if record.get("launch_attempted") or record.get("transport_failed"):',
        '    if record.get("launch_attempted") or record.get("transport_failed") or record.get("scratch_dirty"):')
    runner_path.unlink(); runner_path.write_text(runner)
    shutil.copy2(base / 'build.json', out / 'baseline-build.json')
    result = {'schema': 'gpu-cache-permission-v1', 'physical_access': False, 'execution_admitted': False,
              'baseline_build_sha256': digest(base / 'build.json'), 'baseline_image_sha256': BASE_HASH,
              'compile': commands[:-2], 'link': link, 'commands': commands, 'new_objects': new, 'reused_objects': reused,
              'compiled_inputs': closure, 'host_files': {str(p.relative_to(out)): digest(p) for p in (out / 'host').rglob('*.py')},
              'tools': {str(p): digest(p) for p in {flags[0], '/usr/lib/llvm-21/bin/llvm-nm', binary[0], next(x.split('=', 1)[1] for x in link if x.startswith('--ld-path='))}}, 'image_sha256': digest(out / 'image.elf'),
              'binary_sha256': digest(out / 'probe.bin'), 'binary_bytes': (out / 'probe.bin').stat().st_size,
              'extra_owned_ram': [], 'logical_output': {'address': 0x41800000, 'bytes': 0x400000},
              'owned_ram': {'address': 0x41000000, 'bytes': 0x1200000},
              'expect': 'COMPLETE GPU output cache permission: EL1 RO, zero tags, CIVAC 4MiB, bytes preserved'}
    (out / 'build.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: result[k] for k in ('image_sha256', 'binary_bytes', 'logical_output')}))
if __name__ == '__main__': main()
