#!/usr/bin/env python3
"""Build and check the base image and the returning images of bundles 4, 6, 7 and 8.

docs/designs/board-bundles-next.md specifies the bundles. Every image links
objects of an image that passed on the board, objects of the set's own base
image, which runs first, and its declared new units:

- the base: the central kernel check with
  platform/a733/monitor.c compiled with the monitor's counter option, as
  bundle 2 was built. Objects that differ from every image that passed
  (platform/a733/platform.c since bundle 2) are its declared new units, so
  its board pass makes it the equivalence base of the images below;
- bundle 4 (apps/board-bundles-cpu/) on the base: its checks/kernel.c object
  is replaced by the bundle's main.c, platform/a733/cpu_pmu.c and
  arch/aarch64/device_memory.c, and every other object equals the base's;
- bundles 6 and 7 (apps/board-bundles-power/) on rank-9-usb-v4 and the
  base: the 13 returning platform objects and 14 provider objects, without
  the USB units, plus the bundle's main file, power.c, trace.c, the
  power-domain owner, its native adapter and device_memory.c. The PMIC
  provider, its native adapter and its consumer changed with the PMIC access
  path (docs/designs/pmic-access.md); they and every other provider object
  that recompiles differently are declared new units of both images;
- bundle 8 (apps/board-bundles-dram/) on the same platform objects and the
  provider's tasks.c and queue.c, plus main.c, sequence.c,
  drivers/dram/a733_dram.c in table mode and device_memory.c.

The check:

- requires the rank 4 build to be the kernel check that passed on both
  boards, bundle 2's frozen image to be the one that bb2-run passed on port
  3-3, and every reused rank-9-usb-v4 object to equal its recorded digest;
- recompiles checks/kernel.c and platform/a733/monitor.c and requires each to
  equal the central object, so the compile method adds nothing of its own;
- takes each returning platform object of rank-9-usb-v4 when this tree's
  a733-power-probe object equals it (monitor.c: equal instructions, its line
  table moved with the counter option's lines), and otherwise this tree's
  object, which must equal the base's object of the same unit;
- recompiles every provider unit and takes rank-9-usb-v4's object when they
  are byte-identical; a differing unit must be a declared new provider unit;
- checks that no translation-table write follows platform_init()'s final
  flush of both table pages: in the source, every table write and every call
  that receives a table precedes the two final flush calls, and in the linked
  platform object only platform_init()'s code refers to the tables;
- links each image and records each object's origin: an image that passed
  (rank-4-kernel-check, kernel-check-counter-option, rank-9-usb-v4), the
  set's base (set-base) or new, and refuses a new object outside the image's
  overlay;
- runs the base's secure QEMU model, which must print the kernel check's PASS
  line and exit 0, and builds bundle 4's secure QEMU model twice: the normal
  model must print the three stage markers and the COMPLETE line and exit 0;
  the model whose tick stops before WFI must print the WFI marker and return
  through the secure timer with status 2; each log must fit the 16,127 log
  bytes, and since every bundle 4 value prints at a fixed width, the board's
  log has the same length;
- runs checks/board-bundles-apps-model.c, the host model of bundle 8's
  sequence, of the bundle 6 and 7 records and bundle 6's provisional bound,
  which also bounds the log bytes that bundles 6 and 7 print before their
  detail lines, and of bundle 4's walk-block acceptance on the walk records
  that port 3-3 returned;
- with --usb, records the origin of every object of a native USB image built
  by apps/usb-returning/build.py --platform-build from this tree against
  rank-9-usb-v4, the base and bundle 6's new provider objects;
- writes each image's controlled outcomes, which the runner of --tree must
  accept, and build.json for tools/prepare-board-bundles.py.

Run it from the source tree, which must lie outside ~/.cache/sunstoneos: the
provider objects record the compile directory. No step contacts a device.

  board-bundles-apps.py --central BUILD --rank4 BUILD --bundle2 BUNDLE --bundle2-run SESSION_DIR \\
      --v4 EVIDENCE --tree TREE [--usb EVIDENCE] --output NEW_DIRECTORY
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
LLVM = Path('/usr/lib/llvm-21/bin')
QEMU = Path.home() / '.cache/sunstoneos/host/usr/bin/qemu-system-aarch64'
# Log bytes after the 256-byte result header, less the terminating NUL.
LOG_ROOM = 16384 - 256 - 1
PASSED_KERNEL_CHECK = '574245e291d8ec9a2a52a405ab45ce48fe5550f323a46158b007123dbf96bc54'
BUNDLE2_ELF = 'a8e6ecacd346e79112a49689de0c764c123d6325d5353f500fd86e8bc0350dee'
KERNEL_OBJ = 'checks/kernel.c.obj'
MONITOR_OBJ = 'platform/a733/monitor.c.obj'
TARGETS = (('a733-kernel-check', 'image'), ('a733-kernel-model', 'model'))
# Two ticks of the 24 MHz counter: a core that still wakes on every tick
# has woken less than one tick before any forced return.
PROGRESS = {'offset': 128, 'magic': 0x3130535342344242, 'stage': 3, 'count_below': 1000, 'age_above': 48000}
BUNDLE4_MARKERS = ['STAGE 1 counters complete', 'STAGE 2 walk complete', 'STAGE 3 wfi begin']
KERNEL_PASS = 'PASS kernel: timer preemption, queue ordering, blocking wakeup, FP context; rounds=100'
# Units of the base that differ from every image that passed: platform.c
# since bundle 2 (the IRQ mask after pool preparation), and monitor.c, which
# equals bundle 2's object with the counter option.
BASE_OVERLAY = ['platform/a733/monitor.c.obj', 'platform/a733/platform.c.obj']
# Provider units that the PMIC access path changed (docs/designs/pmic-access.md):
# the provider, its native adapter and its consumer, and the GPU resource
# adapter, whose instructions are unchanged but whose debug information
# records the changed layout of struct a733_pmic. Any other provider unit must
# recompile byte-identical to rank-9-usb-v4's.
PROVIDER_NEW = ('drivers/power/a733_pmic.c', 'drivers/power/a733_pmic_native.c', 'drivers/power/a733_pmic_consumer.c',
                'drivers/gpu/a733_gpu_resources_native.c')
EXPECT = {
    'base': KERNEL_PASS,
    'cpu': 'COMPLETE board bundle 4:',
    'pmic': 'COMPLETE board bundle 6: PMIC service and domain adoption reads',
    'domain': 'COMPLETE board bundle 7:',
    'dram': 'COMPLETE board bundle 8:',
}
POWER_UNITS = ['apps/board-bundles-power/power.c', 'apps/board-bundles-power/trace.c',
               'drivers/power/a733_power_domains.c', 'drivers/power/a733_power_domains_native.c',
               'arch/aarch64/device_memory.c']
DRAM_UNITS = ['apps/board-bundles-dram/main.c', 'apps/board-bundles-dram/sequence.c', 'drivers/dram/a733_dram.c',
              'arch/aarch64/device_memory.c']
# Synchronous external abort on a data access: EC 0x25, DFSC 0b010000.
SEA_MASK, SEA_VALUE = 0xfc00003f, 0x94000010


def require(condition, message):
    if not condition:
        raise SystemExit(message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def flags(central, target):
    """C defines, includes and flags of one central target, from its flags.make."""
    values = {}
    for line in (central / f'CMakeFiles/{target}.dir/flags.make').read_text().splitlines():
        if ' = ' in line and not line.startswith('#'):
            key, value = line.split(' = ', 1)
            values[key] = shlex.split(value)
    return values['C_DEFINES'], values['C_INCLUDES'], values['C_FLAGS']


def compile_object(central, target, source, output, defines=(), general_regs=False):
    """One unit with a central target's flags, in the central build directory, which its prefix maps rename,
    so that the same source gives the central build's object."""
    c_defines, includes, c_flags = flags(central, target)
    command = ['/usr/bin/clang', '--target=aarch64-none-elf', *c_defines, *defines, *includes, *c_flags]
    if general_regs:
        command.append('-mgeneral-regs-only')
    command += ['-MD', '-MF', str(output) + '.d', '-o', str(output), '-c', str(source)]
    subprocess.run(command, check=True, cwd=central)
    return command


def instructions(obj):
    """Disassembly of an object without its file-name header."""
    text = subprocess.run([str(LLVM / 'llvm-objdump'), '-d', '--no-show-raw-insn', str(obj)], check=True,
                          capture_output=True, text=True).stdout
    return text.split('\n', 3)[3]


def qemu(elf, log):
    """One run of a secure model on QEMU virt, one Cortex-A55; returns the exit status and the console."""
    command = [str(QEMU), '-machine', 'virt,secure=on,virtualization=on,gic-version=3', '-cpu', 'cortex-a55',
               '-smp', '1', '-m', '128M', '-nographic', '-monitor', 'none', '-serial', 'none',
               '-semihosting-config', 'enable=on,target=native', '-device', f'loader,file={elf},cpu-num=0']
    result = subprocess.run(command, capture_output=True, text=True, timeout=10, stdin=subprocess.DEVNULL)
    # Semihosting writes the image's console to standard error.
    Path(log).write_text(result.stdout + result.stderr)
    return result.returncode, result.stdout + result.stderr


sys.path.insert(0, str(ROOT / 'tools'))
import returning_build  # noqa: E402


def symbol_range(elf, name):
    out = subprocess.run([str(LLVM / 'llvm-nm'), '--defined-only', '-S', str(elf)], check=True, capture_output=True,
                         text=True).stdout
    found = [line.split() for line in out.splitlines() if line.endswith(' ' + name)]
    require(len(found) == 1 and len(found[0]) == 4, f'{elf} defines no sized {name}')
    low = int(found[0][0], 16)
    return low, low + int(found[0][1], 16)


def table_flush_check(platform_obj):
    """No table write follows platform_init()'s final flush of both table pages."""
    lines = (ROOT / 'platform/a733/platform.c').read_text().splitlines()
    start = next(i for i, line in enumerate(lines) if line.startswith('void platform_init(void)'))
    end = next(i for i in range(start, len(lines)) if lines[i] == '}')
    flushes = [i for i in range(start, end) if re.search(r'a733_cpu_coherence_flush\(\(uintptr_t\)level[12],', lines[i])]
    require(len(flushes) == 4, f'platform_init() flushes the tables {len(flushes)} times, not 4')
    final = flushes[-2]
    named = [i for i, line in enumerate(lines) if re.search(r'\blevel[12]\b', line)]
    outside = [i + 1 for i in named if not start < i < end and 'static uint64_t level' not in lines[i]]
    require(not outside, f'platform.c names a table outside platform_init(): lines {outside}')
    writes = [i for i in named if re.search(r'\blevel[12]\s*\[[^]]*\]\s*=[^=]', lines[i])
              or re.search(r'\w\(\s*level[12]\s*\)', lines[i])]
    late = [i + 1 for i in writes if i > final]
    require(writes and not late, f'table writes follow the final flush: lines {late}')
    after = [i + 1 for i in named if i > flushes[-1]]
    require(all('ttbr0_el1' in lines[i - 1] or '"r"((uintptr_t)level1)' in lines[i - 1] for i in after),
            f'platform_init() names a table after the final flush other than the TTBR0_EL1 write: {after}')
    relocations = subprocess.run([str(LLVM / 'llvm-objdump'), '-r', str(platform_obj)], check=True,
                                 capture_output=True, text=True).stdout
    section, users = None, set()
    for line in relocations.splitlines():
        m = re.match(r'RELOCATION RECORDS FOR \[(.*)\]:', line)
        if m:
            section = m.group(1)
        elif re.search(r'\.bss\.level[12]\b', line) and not section.startswith('.debug'):
            users.add(section)
    require(users == {'.text.platform_init'}, f'table relocations outside platform_init(): {sorted(users)}')
    return {'final_flush_lines': [final + 1, flushes[-1] + 1], 'last_table_write_line': max(writes) + 1,
            'relocation_sections': sorted(users)}


def link_target(central, target, replace, elf):
    """The central link command of target with each named object replaced by a list of objects."""
    words = shlex.split((central / f'CMakeFiles/{target}.dir/link.txt').read_text())
    prefix = f'CMakeFiles/{target}.dir/'
    objects, command = {}, []
    for word in words:
        if word.startswith('-Wl,-Map,'):
            command.append(f'-Wl,-Map,{elf.with_suffix(".map")}')
        elif word.startswith(prefix) and word.endswith('.obj'):
            rel = word[len(prefix):]
            for name, path in replace.get(rel, [(rel, central / word)]):
                objects[name] = Path(path)
                command.append(str(path))
        elif word == f'{target}.elf':
            command.append(str(elf))
        else:
            command.append(word)
    require(set(replace) <= {w[len(prefix):] for w in words if w.startswith(prefix)},
            f'{target} links no object named {sorted(replace)}')
    subprocess.run(command, check=True, cwd=central)
    return objects


def objcopy(elf, binary):
    subprocess.run([str(LLVM / 'llvm-objcopy'), '-O', 'binary', str(elf), str(binary)], check=True)


class Origins:
    """Object digests of the images that passed on the board, then of the set's base."""

    def __init__(self, rank4, bundle2, v4_objects):
        self.known = {}
        for path in sorted((rank4 / 'CMakeFiles/a733-kernel-check.dir').rglob('*.obj')):
            self.known.setdefault(sha(path), 'rank-4-kernel-check')
        for o in json.loads((bundle2 / 'equivalence.json').read_text())['objects'].values():
            self.known.setdefault(o['sha256'], 'kernel-check-counter-option')
        for path in v4_objects:
            self.known.setdefault(sha(path), 'rank-9-usb-v4')

    def add_base(self, objects):
        """The base's objects; those of no passed image take the origin set-base."""
        for o in objects.values():
            self.known.setdefault(o['sha256'], 'set-base')

    def record(self, objects, overlay, image):
        record = {rel: {'sha256': sha(path), 'origin': self.known.get(sha(path), 'new')}
                  for rel, path in sorted(objects.items())}
        new = sorted(rel for rel, o in record.items() if o['origin'] == 'new')
        require(set(new) <= set(overlay), f'{image}: new objects outside the overlay: {sorted(set(new) - set(overlay))}')
        return record


def rel_object(source):
    return str(Path(source).relative_to(ROOT)) + '.obj'


def v4_parts(v4):
    """rank-9-usb-v4's platform objects in link order and its provider objects by unit, each with its digest."""
    platform = []
    for item in json.loads((v4 / 'reused-objects.json').read_text()):
        if 'a733-power-probe.dir/' in item['original']:
            path = v4 / 'build/objects' / Path(item['path']).name
            require(sha(path) == item['sha256'], f'{path} differs from its record')
            platform.append((item['original'].split('a733-power-probe.dir/', 1)[1], path))
    names = tuple(returning_build.CENTRAL_PLATFORM)
    kernel = 'home/sunil/.cache/sunstoneos/sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2/'
    require(tuple('FREERTOS/' + r[len(kernel):-4] if r.startswith(kernel) else r[:-4] for r, _ in platform) == names,
            'rank-9-usb-v4 platform objects are not the returning platform')
    provider = {}
    units = {v: v for v in returning_build.PROVIDER_UNITS.values()}
    for command in json.loads((v4 / 'commands.json').read_text())[:-2]:
        out = Path(command[command.index('-o') + 1])
        if not out.name.startswith('provider-'):
            continue
        source = command[command.index('-c') + 1]
        unit = next(u for u in units if source.endswith('/' + (u[len('FREERTOS/'):] if u.startswith('FREERTOS/') else u)))
        provider[unit] = v4 / 'build/objects' / out.name
    require(set(provider) == set(units), 'rank-9-usb-v4 provider objects are incomplete')
    linked = [Path(x) for x in json.loads((v4 / 'commands.json').read_text())[-2] if x.endswith('.obj')]
    return platform, provider, linked


def platform_and_provider(platform, provider, rp, central, base_objects, work):
    """The returning platform and provider objects of the v4-based images, each with how it was chosen.

    A platform object is rank-9-usb-v4's when this tree's a733-power-probe
    object equals it (monitor.c: equal instructions), and otherwise this
    tree's, which must equal the base's object of the same unit. A provider
    object is rank-9-usb-v4's when the unit recompiles byte-identical, and
    otherwise the recompiled one, which must be a declared new unit."""
    chosen, result = [], {}
    for rel, path in platform:
        mine = central / 'CMakeFiles/a733-power-probe.dir' / rel
        if sha(mine) == sha(path):
            chosen.append((rel, path))
            result[rel] = 'rank-9-usb-v4, byte-identical'
        elif rel == MONITOR_OBJ:
            require(instructions(mine) == instructions(path),
                    'This tree\'s monitor.c changes rank-9-usb-v4 monitor instructions.')
            chosen.append((rel, path))
            result[rel] = 'rank-9-usb-v4, same instructions as this tree'
        else:
            require(rel in base_objects and sha(mine) == base_objects[rel]['sha256'],
                    f'This tree\'s {rel} differs from rank-9-usb-v4\'s and from the base\'s.')
            chosen.append((rel, mine))
            result[rel] = 'set-base, byte-identical'
    rp.objects_dir = work
    rp.commands = []
    sources = [rp.freertos / u[9:] if u.startswith('FREERTOS/') else ROOT / u for u in provider]
    objects = rp.compile(sources, linked=False, prefix='provider')
    returning_build.run_logged(rp.commands, work)
    units = {}
    for unit, obj in zip(provider, objects):
        if sha(obj) == sha(provider[unit]):
            units[unit] = provider[unit]
            result[unit] = 'rank-9-usb-v4, byte-identical'
        else:
            require(unit in PROVIDER_NEW, f'{unit} recompiled differs from rank-9-usb-v4\'s provider object and is not '
                                          'a declared new provider unit.')
            units[unit] = obj
            result[unit] = 'new provider unit' + (', same instructions as rank-9-usb-v4'
                                                  if instructions(obj) == instructions(provider[unit]) else '')
    return chosen, units, result


def base_image(central, origins, out):
    """The kernel check with the monitor's counter option, and its secure QEMU model."""
    directory = out / 'base'
    directory.mkdir()
    objects_dir = directory / 'objects'
    objects_dir.mkdir()
    entry = {'reproduced': {}}
    for target, suffix in TARGETS:
        # The compile method reproduces the central objects.
        for source, rel, general in (('checks/kernel.c', KERNEL_OBJ, False), ('platform/a733/monitor.c', MONITOR_OBJ, True)):
            obj = objects_dir / f'{Path(source).name}-{suffix}-plain.obj'
            compile_object(central, target, ROOT / source, obj, general_regs=general)
            require(sha(obj) == sha(central / f'CMakeFiles/{target}.dir' / rel),
                    f'Recompiled {source} for {target} differs from the central object.')
            entry['reproduced'][f'{target} {source}'] = 'byte-identical'
        monitor = objects_dir / f'monitor-{suffix}.obj'
        compile_object(central, target, ROOT / 'platform/a733/monitor.c', monitor, ('-DSUNSTONE_MONITOR_COUNTERS=1',),
                       general_regs=True)
        elf = directory / f'{suffix}.elf'
        linked = link_target(central, target, {MONITOR_OBJ: [(MONITOR_OBJ, monitor)]}, elf)
        if suffix == 'image':
            binary = directory / 'probe.bin'
            objcopy(elf, binary)
            entry.update(objects=origins.record(linked, BASE_OVERLAY, 'base'), overlay=list(BASE_OVERLAY),
                         monitor=str(monitor), elf=str(elf), elf_sha256=sha(elf), bin=str(binary),
                         bin_sha256=sha(binary), bin_bytes=binary.stat().st_size)
            continue
        status, log = qemu(elf, directory / 'qemu.log')
        require(status == 0 and KERNEL_PASS in log and len(log) <= LOG_ROOM,
                f'The base model did not pass the kernel check: {status}\n{log}')
        entry['qemu'] = {'normal': {'exit_status': status, 'log_sha256': sha(directory / 'qemu.log'),
                                    'log_bytes': len(log)}}
    entry['controlled'] = None
    return directory, entry


def build_v4_image(name, rp, units, platform, provider, flags, origins, out, provider_overlay=()):
    """One image on rank-9-usb-v4 and the base: platform objects, new units, then the named provider objects,
    of which provider_overlay names the declared new ones."""
    directory = out / name
    directory.mkdir()
    rp.objects_dir = directory
    rp.commands = []
    objects = rp.compile([ROOT / u for u in units], flags, linked=False, prefix='new')
    returning_build.run_logged(rp.commands, directory)
    compile_commands = rp.commands
    linked = dict(platform)
    linked.update({rel_object(ROOT / u): o for u, o in zip(units, objects)})
    linked.update({u + '.obj': provider[u] for u in provider})
    elf = directory / 'image.elf'
    command = [rp.compiler, '--target=aarch64-none-elf', rp.linker, '-nostdlib', '-static', '-Wl,--gc-sections',
               f'-Wl,-T,{ROOT}/platform/a733/link.ld', f'-Wl,-Map,{directory}/image.map',
               *[str(p) for _, p in platform], *map(str, objects), *[str(p) for p in provider.values()], '-o', str(elf)]
    subprocess.run(command, check=True)
    binary = directory / 'probe.bin'
    objcopy(elf, binary)
    overlay = sorted([rel_object(ROOT / u) for u in units] + list(provider_overlay))
    entry = {'compile': compile_commands, 'link': command, 'objects': origins.record(linked, overlay, name),
             'overlay': overlay, 'elf': str(elf), 'elf_sha256': sha(elf), 'bin': str(binary), 'bin_sha256': sha(binary),
             'bin_bytes': binary.stat().st_size}
    return directory, entry


def bundle4(central, origins, out, runner, base):
    """The counter, walk and WFI image on the base, and its two QEMU models."""
    directory = out / 'cpu'
    directory.mkdir()
    objects_dir = directory / 'objects'
    objects_dir.mkdir()
    entry = {}
    units = ['apps/board-bundles-cpu/main.c', 'platform/a733/cpu_pmu.c', 'arch/aarch64/device_memory.c']
    for target, suffix in TARGETS:
        monitor = objects_dir / f'monitor-{suffix}.obj'
        compile_object(central, target, ROOT / 'platform/a733/monitor.c', monitor,
                              ('-DSUNSTONE_MONITOR_COUNTERS=1',), general_regs=True)
        if suffix == 'image':
            require(sha(monitor) == base['objects'][MONITOR_OBJ]['sha256'],
                    'monitor.c with the counter option differs from the base\'s monitor object.')
        variants = [('', ())] if suffix == 'image' else [('', ()), ('-hang', ('-DBB4_MODEL_HANG=1',))]
        for variant, defines in variants:
            new = []
            for unit in units:
                obj = objects_dir / f'{Path(unit).stem}-{suffix}{variant}.obj'
                compile_object(central, target, ROOT / unit, obj, defines if unit.endswith('main.c') else ())
                new.append((rel_object(ROOT / unit), obj))
            elf = directory / f'{suffix}{variant}.elf'
            linked = link_target(central, target, {KERNEL_OBJ: new, MONITOR_OBJ: [(MONITOR_OBJ, monitor)]}, elf)
            if suffix == 'image':
                binary = directory / 'probe.bin'
                objcopy(elf, binary)
                overlay = sorted(rel for rel, _ in new)
                record = origins.record(linked, overlay, 'bundle 4')
                differ = sorted(rel for rel, o in record.items() if rel not in overlay
                                and base['objects'].get(rel, {}).get('sha256') != o['sha256'])
                require(not differ and set(base['objects']) - set(record) == {KERNEL_OBJ},
                        f'bundle 4 objects outside its overlay differ from the base: {differ}')
                entry.update(objects=record, overlay=overlay, elf=str(elf),
                             elf_sha256=sha(elf), bin=str(binary), bin_sha256=sha(binary),
                             bin_bytes=binary.stat().st_size)
                continue
            status, log = qemu(elf, directory / f'qemu{variant}.log')
            entry.setdefault('qemu', {})[variant or 'normal'] = {'exit_status': status,
                                                                  'log_sha256': sha(directory / f'qemu{variant}.log')}
            require(all(m in log for m in BUNDLE4_MARKERS), f'bundle 4 model{variant} misses a stage marker: {log}')
            if variant:
                require(status == 2 and EXPECT['cpu'] not in log and 'WFI wakes=' not in log and len(log) <= LOG_ROOM,
                        f'the stopped-tick model did not return through the secure timer: {status}')
                # The semihosting exit leaves no result page to read, so the
                # model matches the outcome without its progress record; the
                # runner check covers the progress comparison.
                terminal = dict(magic=0x534b4552, stage=5, status=status, interrupt=29)
                outcome = bundle4_outcome()
                found = [t for t in outcome['log_contains'] if t in log]
                require(runner.controlled_match(dict(outcome, progress=None), terminal, runner.fault_lines(log), found),
                        'the stopped-tick model does not match the controlled outcome\'s record and log')
            else:
                # Every value prints at a fixed width and every line count is
                # fixed, so the board's log has this length.
                require(status == 0 and 'COMPLETE board bundle 4: counters, walk A/B and WFI' in log
                        and 'STAGE 3 wfi complete' in log and len(log) <= LOG_ROOM,
                        f'bundle 4 model did not pass: {status}, {len(log)} log bytes\n{log}')
                wakes = int(re.search(r'WFI wakes=0x([0-9a-f]+)', log).group(1), 16)
                blocks = len(re.findall(r'^WALK (warm-up )?block=', log, re.M))
                require(900 <= wakes <= 1100 and blocks == 13, f'bundle 4 model: {wakes} wakes, {blocks} walk blocks')
                entry['qemu']['normal'].update(wakes=wakes, walk_blocks=blocks, log_bytes=len(log))
    entry['controlled'] = {'schema': 'kernel-controlled-outcomes-v1', 'outcomes': [bundle4_outcome()]}
    return directory, entry


def bundle4_outcome():
    return dict(name='wfi-stopped-waking', status=2, interrupt=29, fail_lines=0, fault=None,
                log_contains=list(BUNDLE4_MARKERS), progress=dict(PROGRESS))


def bundle6_outcomes():
    """A missed provisional bound returns to the ROM with status 1 after the summary."""
    return [dict(name='provisional-step-bound-missed', status=1, interrupt=None, fail_lines=0, fault=None,
                 log_contains=['BOUND permille=1550', 'FAILED board bundle 6: provisional step bound'])]


def bundle7_outcomes():
    return [dict(name='domain-1-failed', status=1, interrupt=None, fail_lines=0, fault=None,
                 log_contains=['FAILED board bundle 7: domain 1', 'TRACE END board bundle 7'])]


def bundle8_outcomes(elf):
    low, high = symbol_range(elf, 'bb8_access')
    ports, offsets = (2, 14, 15, 13), (0x10, 0x14, 0x18)
    outcomes = [dict(name=f'nsi-external-abort-0x{0x02020000 + 0x200 * p + o:08x}', status=1, interrupt=None,
                     fail_lines=1, fault=dict(esr_mask=SEA_MASK, esr_value=SEA_VALUE, far=0x02020000 + 0x200 * p + o,
                                              elr_low=low, elr_high=high, task='bundle8'),
                     log_contains=['BEGIN board bundle 8 NSI access']) for p in ports for o in offsets]
    outcomes.append(dict(name='nsi-readback-mismatch', status=1, interrupt=None, fail_lines=0, fault=None,
                         log_contains=['BEGIN board bundle 8 NSI access', 'FAIL board bundle 8: NSI readback mismatch']))
    return outcomes


def usb_origins(usb, origins, provider_new, v4_monitor):
    """Each object of a native USB image built from this tree: its origin and, for the platform and provider
    objects, its unit. Platform and provider objects must come from an image that passed, the base or bundle
    6's new provider objects; the builder links this tree's monitor.c object, whose instructions must equal
    rank-9-usb-v4's. Only the USB units and the accepted TinyUSB objects may be new."""
    known = dict(origins.known)
    for unit, path in provider_new.items():
        known.setdefault(sha(path), 'set-bundle-6')
    commands = json.loads((usb / 'commands.json').read_text())
    record, units = {}, {}
    for command in commands[:-2]:
        out = Path(command[command.index('-o') + 1])
        units[out.name] = command[command.index('-c') + 1]
    for item in json.loads((usb / 'reused-objects.json').read_text()):
        units.setdefault(Path(item['path']).name, item['original'])
    linked = [Path(x) for x in commands[-2] if x.endswith('.obj')]
    for path in linked:
        digest = sha(path)
        unit = units.get(path.name, '')
        origin = known.get(digest, 'new')
        if origin == 'new' and unit.endswith('a733-power-probe.dir/' + MONITOR_OBJ):
            require(instructions(path) == instructions(v4_monitor),
                    'The native USB image\'s monitor.c changes rank-9-usb-v4 monitor instructions.')
            origin = 'rank-9-usb-v4, same instructions'
        record[path.name] = {'unit': unit, 'sha256': digest, 'origin': origin}
    new = sorted(n for n, o in record.items() if o['origin'] == 'new')
    platform = [n for n in new if n.startswith('provider-') or ('a733-power-probe.dir/' in record[n]['unit'])]
    require(not platform, f'native USB image: platform or provider objects of no passed image or set image: {platform}')
    return {'objects': record, 'new': new, 'elf_sha256': sha(usb / 'build/probe.elf'),
            'bin_sha256': sha(usb / 'probe.bin')}


def model_check(out):
    """checks/board-bundles-apps-model.c on the host."""
    work = out / 'model'
    work.mkdir()
    exe = work / 'model'
    command = ['/usr/bin/clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
               '-DSUNSTONE_DRAM_PRIORITY=2', f'-I{ROOT}/include', f'-I{ROOT}/apps/board-bundles-dram',
               f'-I{ROOT}/apps/board-bundles-power', f'-I{ROOT}/apps/board-bundles-cpu',
               str(ROOT / 'checks/board-bundles-apps-model.c'),
               str(ROOT / 'apps/board-bundles-dram/sequence.c'), str(ROOT / 'drivers/dram/a733_dram.c'),
               str(ROOT / 'apps/board-bundles-power/trace.c'), '-o', str(exe)]
    subprocess.run(command, check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    (work / 'model.log').write_text(result.stdout + result.stderr)
    require(result.returncode == 0 and result.stdout.rstrip().endswith('PASS'), f'host model failed:\n{result.stdout}'
            f'{result.stderr}')
    return json.loads(result.stdout.splitlines()[0])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--central', type=Path, required=True, help='Built central tree of this revision')
    parser.add_argument('--rank4', type=Path, required=True, help='Central build of the passing kernel check')
    parser.add_argument('--bundle2', type=Path, required=True, help='Bundle 2\'s frozen bundle directory')
    parser.add_argument('--bundle2-run', type=Path, required=True, help='Bundle 2\'s passing run on the board')
    parser.add_argument('--v4', type=Path, required=True, help='rank-9-usb-v4 bundle directory')
    parser.add_argument('--tree', type=Path, required=True,
                        help='Source tree whose tools/check-a733-kernel.py carries the controlled-outcome progress record')
    parser.add_argument('--usb', type=Path, help='Evidence of apps/usb-returning/build.py --platform-build from this tree')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    # The provider objects record the compile directory: it must be this
    # tree, outside the cache, as for rank-9-usb-v4.
    require(Path.cwd().resolve() == ROOT and Path.home() / '.cache/sunstoneos' not in ROOT.parents,
            f'Run from the source tree {ROOT}, outside ~/.cache/sunstoneos.')
    central, out, v4 = args.central.resolve(), args.output.resolve(), args.v4.resolve()
    rank4, bundle2, tree = args.rank4.resolve(), args.bundle2.resolve(), args.tree.resolve()
    require(sha(rank4 / 'a733-kernel-check.elf') == PASSED_KERNEL_CHECK,
            'The rank 4 build is not the kernel check that passed on both boards.')
    require(sha(bundle2 / 'image.elf') == BUNDLE2_ELF, 'The bundle 2 directory is not bundle 2\'s frozen image.')
    session = json.loads((args.bundle2_run / 'session.json').read_text())
    require(session.get('complete') is True and session.get('image_sha256') == sha(bundle2 / 'probe.bin')
            and session['result']['status'] == 0 and session.get('port') == '3-3',
            'bb2-run is not a passing run of bundle 2.')
    out.mkdir(parents=True, exist_ok=False)
    runner = load('apps_runner', tree / 'tools/check-a733-kernel.py')
    result = {'physical_access': False, 'central': str(central), 'tree': str(tree), 'qemu_sha256': sha(QEMU),
              'reproduced': {}, 'images': {}}

    platform, provider, v4_linked = v4_parts(v4)
    origins = Origins(rank4, bundle2, v4_linked)
    builds = {}
    directory, base = base_image(central, origins, out)
    base['expect'] = EXPECT['base']
    builds['base'] = (directory, base)
    result['reproduced'].update({f'base {k}': v for k, v in base.pop('reproduced').items()})
    origins.add_base(base['objects'])

    rp = returning_build.ReturningPlatform(out / 'platform', build=central)
    (out / 'provider').mkdir()
    platform, provider, chosen = platform_and_provider(platform, provider, rp, central, base['objects'],
                                                       out / 'provider')
    result['reproduced'].update(chosen)
    result['table_flush'] = table_flush_check(dict(platform)['platform/a733/platform.c.obj'])
    provider_new = {u: p for u, p in provider.items() if chosen[u].startswith('new provider unit')}
    new_units = sorted(provider_new)

    directory, entry = bundle4(central, origins, out, runner, base)
    entry['expect'] = EXPECT['cpu']
    builds['cpu'] = (directory, entry)
    for name, units, flags, keep in (
            ('pmic', POWER_UNITS + ['apps/board-bundles-power/bundle6.c'], (), None),
            ('domain', POWER_UNITS + ['apps/board-bundles-power/bundle7.c'], (), None),
            ('dram', DRAM_UNITS, ('-DSUNSTONE_DRAM_PRIORITY=2',), ('FREERTOS/tasks.c', 'FREERTOS/queue.c'))):
        units_provider = {u: p for u, p in provider.items() if keep is None or u in keep}
        directory, entry = build_v4_image(name, rp, units, platform, units_provider, list(flags), origins, out,
                                          [u + '.obj' for u in new_units if u in units_provider])
        entry['expect'] = EXPECT[name]
        entry['controlled'] = None
        if name == 'pmic':
            entry['controlled'] = {'schema': 'kernel-controlled-outcomes-v1', 'outcomes': bundle6_outcomes()}
        elif name == 'domain':
            entry['controlled'] = {'schema': 'kernel-controlled-outcomes-v1', 'outcomes': bundle7_outcomes()}
        elif name == 'dram':
            entry['controlled'] = {'schema': 'kernel-controlled-outcomes-v1',
                                   'outcomes': bundle8_outcomes(Path(entry['elf']))}
        builds[name] = (directory, entry)
    result['model'] = model_check(out)
    if args.usb:
        result['usb'] = usb_origins(args.usb.resolve(), origins, provider_new, dict(platform)[MONITOR_OBJ])

    for name, (directory, entry) in builds.items():
        if entry['controlled'] is not None:
            runner.controlled_outcomes(json.dumps(entry['controlled']))
        entry['host_tools'] = {'check-a733-kernel.py': str(tree / 'tools/check-a733-kernel.py'),
                               'check-a733-smp.py': str(ROOT / 'tools/check-a733-smp.py'),
                               'usb-access.py': str(ROOT / 'tools/usb-access.py')}
        (directory / 'build.json').write_text(json.dumps(entry, indent=2) + '\n')
        result['images'][name] = {'build': str(directory / 'build.json'), 'elf_sha256': entry['elf_sha256'],
                                  'bin_bytes': entry['bin_bytes'],
                                  'new_objects': [r for r, o in entry['objects'].items() if o['origin'] == 'new'],
                                  'base_objects': [r for r, o in entry['objects'].items() if o['origin'] == 'set-base'],
                                  'controlled': [o['name'] for o in (entry['controlled'] or {}).get('outcomes', [])]}
        if 'qemu' in entry:
            result['images'][name]['qemu'] = entry['qemu']
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: {x: v[x] for x in ('elf_sha256', 'new_objects', 'base_objects')}
                      for k, v in result['images'].items()}, indent=1))
    if 'usb' in result:
        print(json.dumps({'usb_new': result['usb']['new'], 'usb_elf_sha256': result['usb']['elf_sha256']}))


if __name__ == '__main__':
    main()
