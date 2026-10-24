#!/usr/bin/env python3
"""Build and check the two returning images of the single-core board bundles.

The fault-capture image links checks/board-bundles-fault.c in place of
checks/kernel.c. The counter-option image compiles platform/a733/monitor.c
with SUNSTONE_MONITOR_COUNTERS from a source tree that carries that option.
Every other object is the configured central build's own a733-kernel-check
object. The check:

- recompiles checks/kernel.c and platform/a733/monitor.c of this tree and
  requires both objects to equal the central build's, so the compile method
  adds nothing of its own; it also requires the option tree's monitor.c,
  compiled without the option, to disassemble to the central object's
  instructions (its line table differs by the option's lines);
- links each image and its secure QEMU model (a733-kernel-model objects) with
  the central link commands, and writes the flat binary;
- records, for every linked object, whether it is byte-identical to the
  object of the kernel check that passed ranks 4 and 5 on both boards, to the
  platform object of rank-9-usb-v4, which passed on port 3-3, or new, and
  refuses a new object other than the replaced one;
- runs each model once in secure QEMU: the fault model must exit with status
  1 and print exactly one FAIL exception line that the kernel runner's
  controlled_match() accepts against the model ELF's function range; the
  counter-option and unchanged models must print the kernel check's PASS
  line and exit 0;
- writes build.json for each physical image, which
  tools/prepare-board-bundles.py reads.

No step contacts a device.

  board-bundles-images.py --central BUILD --option-tree TREE --rank4 BUILD \\
      --v4 EVIDENCE --output NEW_DIRECTORY
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shlex
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
QEMU = Path.home() / '.cache/sunstoneos/host/usr/bin/qemu-system-aarch64'
LLVM = Path('/usr/lib/llvm-21/bin')
PASSED_KERNEL_CHECK = '574245e291d8ec9a2a52a405ab45ce48fe5550f323a46158b007123dbf96bc54'
FAULT_ADDRESS = 0x400000000
# ESR_EL1: EC 0x25 (data abort, same EL), IL 1, WnR 0, DFSC 0b000101
# (translation fault, level 1); ISV and the syndrome fields stay unconstrained.
ESR_MASK, ESR_VALUE = 0xfe00007f, 0x96000005
CHECK_TEXT = 'CHECK fault capture: load 0x400000000'
KERNEL_PASS = 'PASS kernel: timer preemption, queue ordering, blocking wakeup, FP context; rounds=100'
KERNEL_OBJ = 'checks/kernel.c.obj'
MONITOR_OBJ = 'platform/a733/monitor.c.obj'
TARGETS = (('a733-kernel-check', 'image'), ('a733-kernel-model', 'model'))


def require(condition, message):
    if not condition:
        raise SystemExit(message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def flags(central, target):
    """C defines, includes and flags of one central target, from its flags.make."""
    values = {}
    for line in (central / f'CMakeFiles/{target}.dir/flags.make').read_text().splitlines():
        if ' = ' in line and not line.startswith('#'):
            key, value = line.split(' = ', 1)
            values[key] = shlex.split(value)
    return values['C_DEFINES'], values['C_INCLUDES'], values['C_FLAGS']


def compile_object(central, target, source, output, defines=(), source_root=None, general_regs=False):
    c_defines, includes, c_flags = flags(central, target)
    # A source outside the central tree is renamed to the tree's own prefix.
    # Clang applies the last matching map, so this one follows the central
    # maps, one of which also matches it.
    last = [f'-ffile-prefix-map={source_root}=src'] if source_root is not None else []
    command = ['/usr/bin/clang', '--target=aarch64-none-elf', *c_defines, *defines, *includes, *c_flags, *last]
    if general_regs:
        command.append('-mgeneral-regs-only')
    command += ['-MD', '-MF', str(output) + '.d', '-o', str(output), '-c', str(source)]
    # The central build compiles in its own directory, which its prefix maps
    # rename; the same working directory gives the same debug information.
    subprocess.run(command, check=True, cwd=central)
    return command


def instructions(obj):
    """Disassembly of an object without its file-name header."""
    text = subprocess.run([str(LLVM / 'llvm-objdump'), '-d', '--no-show-raw-insn', str(obj)], check=True,
                          capture_output=True, text=True).stdout
    return text.split('\n', 3)[3]


def link(central, target, replace, elf):
    """The central link command of target with objects replaced; returns the linked objects."""
    words = shlex.split((central / f'CMakeFiles/{target}.dir/link.txt').read_text())
    prefix = f'CMakeFiles/{target}.dir/'
    objects, command = {}, []
    for word in words:
        if word.startswith('-Wl,-Map,'):
            word = f'-Wl,-Map,{elf.with_suffix(".map")}'
        elif word.startswith(prefix) and word.endswith('.obj'):
            rel = word[len(prefix):]
            path = Path(replace[rel]) if rel in replace else central / word
            objects[rel] = path
            word = str(path)
        elif word == f'{target}.elf':
            word = str(elf)
        command.append(word)
    require(set(replace) <= set(objects), f'{target} links no object named {set(replace) - set(objects)}')
    subprocess.run(command, check=True, cwd=central)
    return objects


def fault_outcome(elf):
    """The fault image's one controlled outcome, with ELR_EL1 inside its load function."""
    out = subprocess.run([str(LLVM / 'llvm-nm'), '--defined-only', '-S', str(elf)], check=True,
                         capture_output=True, text=True).stdout
    found = [line.split() for line in out.splitlines() if line.endswith(' board_bundles_fault_load')]
    require(len(found) == 1 and len(found[0]) == 4, f'{elf} defines no sized board_bundles_fault_load')
    low = int(found[0][0], 16)
    return dict(name='level-1-translation-fault', status=1, interrupt=None, fail_lines=1,
                fault=dict(esr_mask=ESR_MASK, esr_value=ESR_VALUE, far=FAULT_ADDRESS, elr_low=low,
                           elr_high=low + int(found[0][1], 16), task='fault'),
                log_contains=[CHECK_TEXT])


def origins(objects, rank4, v4):
    """Each linked object against the passing kernel check and rank-9-usb-v4."""
    passed = {}
    for item in json.loads((v4 / 'reused-objects.json').read_text()):
        # The list also names TinyUSB objects from another build.
        if 'a733-power-probe.dir/' in item['original']:
            passed[item['original'].split('a733-power-probe.dir/', 1)[1]] = item['sha256']
    record = {}
    for rel, path in sorted(objects.items()):
        digest = sha(path)
        kernel = rank4 / 'CMakeFiles/a733-kernel-check.dir' / rel
        if kernel.is_file() and sha(kernel) == digest:
            origin = 'rank-4-kernel-check'
        elif passed.get(rel) == digest:
            origin = 'rank-9-usb-v4'
        else:
            origin = 'new'
        record[rel] = {'sha256': digest, 'origin': origin}
    return record


def qemu(elf, log):
    command = [str(QEMU), '-machine', 'virt,secure=on,virtualization=on,gic-version=3', '-cpu', 'cortex-a55',
               '-smp', '1', '-m', '128M', '-nographic', '-monitor', 'none', '-serial', 'none',
               '-semihosting-config', 'enable=on,target=native', '-device', f'loader,file={elf},cpu-num=0']
    result = subprocess.run(command, capture_output=True, text=True, timeout=10, stdin=subprocess.DEVNULL)
    # Semihosting writes the image's console to standard error.
    Path(log).write_text(result.stdout + result.stderr)
    return result.returncode, result.stdout + result.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--central', type=Path, required=True, help='Built central tree of this revision')
    parser.add_argument('--option-tree', type=Path, required=True,
                        help='Source tree whose platform/a733/monitor.c and tools/check-a733-kernel.py carry the '
                             'counter option and controlled outcomes')
    parser.add_argument('--rank4', type=Path, required=True, help='Central build of the passing kernel check')
    parser.add_argument('--v4', type=Path, required=True, help='rank-9-usb-v4 bundle directory')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    central, tree, out = args.central.resolve(), args.option_tree.resolve(), args.output.resolve()
    rank4, v4 = args.rank4.resolve(), args.v4.resolve()
    require(sha(rank4 / 'a733-kernel-check.elf') == PASSED_KERNEL_CHECK,
            'The rank 4 build is not the kernel check that passed on both boards.')
    out.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location('option_runner', tree / 'tools/check-a733-kernel.py')
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    objects_dir = out / 'objects'
    objects_dir.mkdir()
    result = {'physical_access': False, 'central': str(central), 'option_tree': str(tree),
              'qemu_sha256': sha(QEMU), 'reproduced': {}, 'images': {}}

    # The compile method reproduces the central objects.
    for target, _ in TARGETS:
        central_dir = central / f'CMakeFiles/{target}.dir'
        for name, rel, general in (('checks/kernel.c', KERNEL_OBJ, False),
                                   ('platform/a733/monitor.c', MONITOR_OBJ, True)):
            obj = objects_dir / f'{target}-{Path(name).name}.obj'
            compile_object(central, target, ROOT / name, obj, general_regs=general)
            require(sha(obj) == sha(central_dir / rel), f'Recompiled {name} for {target} differs from the central '
                                                        'object.')
            result['reproduced'][f'{target}:{name}'] = 'byte-identical'
        plain = objects_dir / f'{target}-option-tree-monitor.c.obj'
        compile_object(central, target, tree / 'platform/a733/monitor.c', plain, source_root=tree,
                       general_regs=True)
        require(instructions(plain) == instructions(central_dir / MONITOR_OBJ),
                f'The option tree monitor.c without the option changes {target} instructions.')
        result['reproduced'][f'{target}:option-tree monitor.c without the option'] = 'same instructions'

    for image, rel, source, defines, kwargs in (
            ('fault', KERNEL_OBJ, ROOT / 'checks/board-bundles-fault.c', (), {}),
            ('counters', MONITOR_OBJ, tree / 'platform/a733/monitor.c', ('-DSUNSTONE_MONITOR_COUNTERS=1',),
             {'source_root': tree, 'general_regs': True})):
        directory = out / image
        directory.mkdir()
        entry = {}
        for target, suffix in TARGETS:
            obj = objects_dir / f'{image}-{suffix}.obj'
            entry[f'{suffix}_compile'] = compile_object(central, target, source, obj, defines, **kwargs)
            elf = directory / f'{suffix}.elf'
            objects = link(central, target, {rel: obj}, elf)
            if suffix == 'image':
                binary = directory / 'probe.bin'
                subprocess.run([str(LLVM / 'llvm-objcopy'), '-O', 'binary', str(elf), str(binary)], check=True)
                entry['objects'] = origins(objects, rank4, v4)
                entry.update(elf=str(elf), elf_sha256=sha(elf), bin=str(binary), bin_sha256=sha(binary),
                             bin_bytes=binary.stat().st_size)
                continue
            status, stdout = qemu(elf, directory / 'qemu.log')
            entry['qemu'] = {'exit_status': status, 'log': str(directory / 'qemu.log'),
                             'log_sha256': sha(directory / 'qemu.log')}
            if image == 'fault':
                outcome = fault_outcome(elf)
                faults = runner.fault_lines(stdout)
                terminal = dict(magic=0x534b4552, stage=5, status=status, interrupt=0)
                found = [text for text in outcome['log_contains'] if text in stdout]
                require(status == 1 and runner.controlled_match(outcome, terminal, faults, found),
                        f'The fault model did not end in its controlled outcome: {status} {faults}')
                entry['qemu'].update(faults=faults, matched=outcome['name'])
            else:
                require(status == 0 and KERNEL_PASS in stdout,
                        f'The counter-option model did not pass the kernel check: {status}')
        unexpected = sorted(r for r, o in entry['objects'].items() if o['origin'] == 'new' and r != rel)
        require(not unexpected, f'{image}: new objects outside the overlay: {unexpected}')
        if image == 'fault':
            entry['controlled'] = {'schema': 'kernel-controlled-outcomes-v1',
                                   'outcomes': [fault_outcome(Path(entry['elf']))]}
            runner.controlled_outcomes(json.dumps(entry['controlled']))
            entry['expect'] = CHECK_TEXT
        else:
            entry['controlled'] = None
            entry['expect'] = KERNEL_PASS
        entry['overlay'] = [rel]
        entry['host_tools'] = {'check-a733-kernel.py': str(tree / 'tools/check-a733-kernel.py'),
                               'check-a733-smp.py': str(ROOT / 'tools/check-a733-smp.py'),
                               'usb-access.py': str(ROOT / 'tools/usb-access.py')}
        (directory / 'build.json').write_text(json.dumps(entry, indent=2) + '\n')
        result['images'][image] = {'build': str(directory / 'build.json'), 'elf_sha256': entry['elf_sha256'],
                                   'qemu_exit_status': entry['qemu']['exit_status'],
                                   'new_objects': [r for r, o in entry['objects'].items() if o['origin'] == 'new']}

    # The unchanged kernel model passes under the same QEMU command.
    status, stdout = qemu(central / 'a733-kernel-model.elf', out / 'kernel-model-qemu.log')
    require(status == 0 and KERNEL_PASS in stdout, 'The unchanged kernel model did not pass in QEMU.')
    result['unchanged_model_exit_status'] = status
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result['images'], indent=1))


if __name__ == '__main__':
    main()
