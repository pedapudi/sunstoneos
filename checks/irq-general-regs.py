#!/usr/bin/env python3
"""Exception code of each image linked with configSS_IRQ_SAVES_FP 0 uses no FP or SIMD register outside a task's own
context save and restore.

IRQ entry saves the 32 Q registers, FPSR and FPCR unless the build sets SUNSTONE_IRQ_FP_UNSAVED (cmake/irq-fp.cmake),
which makes configSS_IRQ_SAVES_FP 0 in platform/a733-smp-cached/FreeRTOSConfig.h; every A733 configuration includes
that file, and the option is the only way to 0. An image may skip the save only when nothing an exception runs changes
those registers (docs/driver-validation.md, rule 7), and this check proves that for every image the gate builds with 0.
GATE_RUNS gives it, with --walk, the builds of the image recipes (tools/gate_images.py) and OS variants
(tools/integration-gate.py) whose configuration sets the option, and the check refuses a GATE_RUNS whose --walk builds
are not exactly those. Every other build keeps the save and needs no entry, so a new image keeps it until it sets the
option and is walked. Builds outside the gate keep it too: the QEMU OS images (platform/qemu-os/FreeRTOSConfig.h sets
1) because checks/qemu-os-boot.py builds them inside its own run, where this check cannot read them, and the
application variants that no gate build configures.

The check finds the images itself. In each build, the compile command of each FreeRTOS port assembly unit is run through
the preprocessor to read configSS_IRQ_SAVES_FP, and the CMake link commands (CMakeFiles/TARGET.dir/link.txt) name the
executables that link each unit, their ELF and their linker map. Every built executable linked with 0 is walked. It
fails when a --walk build does not set the option, and when a --walk build configures an executable with 0 that no
given build built. The unpatched FreeRTOS port, which some probes link, defines no setting: its IRQ entry has never
saved the FP registers.

The check takes about 11 minutes on all walked builds. To cut that, walk only the images whose inputs changed.

The walk starts at every slot of the EL1 vector table a733_cached_el1_vectors (IRQ, SVC and fault entries) and follows
every direct branch and call. An indirect call or tail call may reach every function whose address the image takes, in
data or by ADR or ADRP and ADD, except the task-only functions of TASK_ONLY. It then requires:

1. no FP or SIMD instruction is reachable, except the task context transfers of portSAVE_CONTEXT and
   portRESTORE_CONTEXT: a CBZ on the core's FPU-context flag that skips exactly one complete save or restore of Q0-Q31,
   FPSR and FPCR;
2. every C and C++ unit with a reachable function is compiled with -mgeneral-regs-only (compile_commands.json and the
   linker map of the build), so the compiler cannot add SIMD there, including inlined memcpy and memset;
3. each function of TASK_ONLY whose address the image takes has that address taken only in the units its entry names.
   These functions reach FP code; their callers are tasks, which run with their own FP context, the boot code before
   the scheduler starts, or a secondary core's power-on entry at EL3. An entry names the unit that defines the
   function, so a static name that several units define (observe, worker) excludes only that unit's function.

The walk includes the panic path (configASSERT calls platform_panic) and the fault entries, so printf, memchr and the
other FP users of newlib are proven unreachable from them as well. Four mutations must each fail: an FP instruction
planted in a function that only an indirect call reaches, the guard of an IRQ-path context save replaced by a NOP,
-mgeneral-regs-only removed from a reachable unit, and the TASK_ONLY entry of a function whose direct calls reach FP
removed. It writes only its result; the preprocessor runs write nothing.
"""
import argparse
import ast
import bisect
import collections
import copy
import importlib.util
import json
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'checks'))
import harness  # noqa: E402,F401  Refuses to run with assertions disabled.

# The gate builds whose configuration sets SUNSTONE_IRQ_FP_UNSAVED; main refuses a GATE_RUNS that walks a different set.
GATE_RUNS = [[
    '--walk', '{build:sup}', '--walk', '{build:headset}', '--walk', '{build:os-usb-ledger}',
    '--walk', '{build:resident-stop}', '--walk', '{build:card-profile}', '--walk', '{build:ret-npu-load}',
    '--walk', '{build:ret-gpu}', '--walk', '{build:ret-g2d}', '--walk', '{build:ret-latency}',
    '--walk', '{build:ret-latency-bar}', '--walk', '{build:ret-latency-bar-precise}',
    '--walk', '{build:ret-radio-scan}', '--walk', '{build:ret-walks-cached}', '--walk', '{build:ret-walks-uncached}',
    '--walk', '{build:storage-read}', '--walk', '{build:storage-read-p1}', '--walk', '{build:a733-applications}',
    '--walk', '{build:cpu-reliability-model}',
    '--output', '{output}']]
CHECK_INPUTS = ('patches/freertos-smp-asm.patch', 'patches/freertos-mask-returns.patch', 'patches/freertos-latency-asm.patch',
                'patches/freertos-partitioned-port-asm.patch', 'patches/freertos-gpu-call.patch',
                'platform/a733-smp-cached/FreeRTOSConfig.h', 'platform/qemu-os/FreeRTOSConfig.h',
                'platform/a733-smp-cached/platform.c', 'platform/a733-smp-cached/irq_table.c',
                'platform/a733-smp-cached/continue.S', 'kernel/scheduler', 'drivers', 'docs/driver-validation.md',
                # the configuration chain that sets configSS_IRQ_SAVES_FP, and the builds the check discovers it in
                'include/FreeRTOSConfig.h', 'platform/a733-storage-maintenance/FreeRTOSConfig.h',
                'apps/board/FreeRTOSConfig.h', 'apps/camera/FreeRTOSConfig.h', 'apps/hdmi/FreeRTOSConfig.h',
                'CMakeLists.txt', 'cmake', 'tools/gate_images.py', 'tools/integration-gate.py', 'checks/fp-context.S')
VECTORS = 'a733_cached_el1_vectors'
# Functions that reach FP code and whose addresses only task code, boot code or the secondary cores' EL3 entry uses:
# (name, the unit that defines it) -> (the units that may take its address, why).
APP = 'apps/cpu-npu-concurrency/main.c'
FP_TASK = 'the FP-context task entry of the diagnostic, which creates one per core; check_fp_context (checks/fp-context.S) ' \
          'verifies that its own FP registers survive task switches'
TASK_ONLY = {
    ('a733_cpu_secondary_continue', 'continue.S'): (
        ('platform/a733/cpu_entry_native.c',), 'the EL3 continuation that a powered-on secondary core branches to from '
        'a733_cpu_secondary_entry; it parks unless it runs at EL3, and its boot path reaches os_application_prepare'),
    ('a733_cpu_secondary_entry', 'platform/a733/cpu_entry.S'): (
        ('platform/a733/cpu_entry_native.c', 'boot.c', 'boot-supervised.c'), 'the reset entry of the secondary cores, '
        'which the boot code gives the power controller as each core\'s start address; it parks unless it runs at EL3'),
    ('cpu_npu_load_init', APP): (('sunstone-tables.c',), 'the init hook of the CPU and NPU load application, which '
                                 'os_application_prepare calls at boot before the scheduler starts'),
    ('cpu_npu_load_main', APP): (('sunstone-tables.c',), 'the CPU and NPU load application main, which the OS starts '
                                 'as the application task'),
    ('latency_main', 'apps/latency/native.c'): (('sunstone-tables.c',), 'the latency application main, which the OS '
                                                'starts as the application task'),
    ('loader', 'apps/os/large_load.c'): (('apps/os/large_load.c',), 'the large-region loader task entry; it hashes '
                                         'with the SHA2 instructions'),
    ('observe', APP): ((APP,), 'the NPU observer of the CPU and NPU load application, registered by its init hook; '
                       'only the NPU service worker task calls it (drivers/npu/npu_service.c worker, '
                       'apps/os/npu-observe.c sample)'),
    ('os_application_prepare', 'apps/os/application.c'): (
        ('apps/os/start.c',), 'the application preparation, which board_application_start calls directly at boot '
        'before the scheduler starts; start.c takes its address only for the failure note'),
    ('sink_consume', 'apps/os/large_load.c'): (('apps/os/large_load.c',), 'the FAT32 sink of the loader task, which '
                                               'it calls from its own reads'),
    ('table_walks_main', 'apps/table-walks/main.c'): (('sunstone-tables.c',), 'the table-walks application main, '
                                                      'which the OS starts as the application task'),
    ('worker', APP): ((APP,), 'the cpu-gemm task entry of the CPU and NPU load application, which its init hook '
                      'creates'),
    ('worker', 'platform/a733-smp-cached/diagnostic.c'): (('platform/a733-smp-cached/diagnostic.c',), FP_TASK),
    ('worker', 'platform/a733-smp-observe/diagnostic.c'): (('platform/a733-smp-observe/diagnostic.c',), FP_TASK),
    ('main_entry', 'a733-smp-cached-diagnostic.dir/application.c'): (
        ('a733-smp-cached-diagnostic.dir/application.c',), 'the cpu-rel-main task entry of the CPU reliability model '
        '(checks/cpu-reliability-model/application.c), which its init creates'),
    ('worker', 'apps/cpu-runtime-qualification/reliability.c'): (
        ('apps/cpu-runtime-qualification/reliability.c',), 'the cpu-rel task entry of the CPU reliability '
        'qualification, which its main creates on each worker core'),
}
FP_REGISTER = re.compile(r'^[qdshbv]\d+$')


class Image:
    """The functions, code bytes, address-taken functions and units of one linked image."""

    def __init__(self, build, elf_path, map_path):
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
        from elftools.elf.elffile import ELFFile
        self.disassembler = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        self.disassembler.detail = True
        self.disassembler.skipdata = True
        elf = ELFFile(open(elf_path, 'rb'))
        self.code = [(s['sh_addr'], bytearray(s.data())) for s in elf.iter_sections()
                     if s['sh_flags'] & 4 and s['sh_type'] == 'SHT_PROGBITS' and s['sh_size']]
        self.data = [(s['sh_addr'], s.data()) for s in elf.iter_sections()
                     if s['sh_type'] == 'SHT_PROGBITS' and s['sh_flags'] & 2 and not s['sh_flags'] & 4]
        self.names, self.sized, self.address, self.symbols = {}, {}, {}, collections.defaultdict(set)
        for section in elf.iter_sections():
            if section.header.sh_type != 'SHT_SYMTAB':
                continue
            for symbol in section.iter_symbols():
                kind, value = symbol['st_info']['type'], symbol['st_value']
                if not symbol.name or symbol.name.startswith('$') or kind not in ('STT_FUNC', 'STT_NOTYPE') \
                        or not self.in_code(value):
                    continue
                if kind == 'STT_FUNC' or value not in self.names:
                    self.names[value] = symbol.name
                if kind == 'STT_FUNC' and symbol['st_size']:
                    self.sized[value] = symbol['st_size']
                self.address.setdefault(symbol.name, value)
                if kind == 'STT_FUNC':
                    self.symbols[symbol.name].add(value)
        self.starts = sorted(self.names)
        self.functions = set(self.sized)
        self.units = self.read_map(map_path)
        self.taken = self.address_taken()

    def in_code(self, address):
        return any(base <= address < base + len(data) for base, data in self.code)

    def words(self, address, size):
        for base, data in self.code:
            if base <= address < base + len(data):
                return bytes(data[address - base:min(address - base + size, len(data))])
        return b''

    def patch(self, address, word):
        for base, data in self.code:
            if base <= address < base + len(data):
                data[address - base:address - base + 4] = struct.pack('<I', word)
                return
        raise AssertionError(hex(address))

    def extent(self, address):
        """A sized function's own size; for an assembly label, the bytes up to the next symbol."""
        if address in self.sized:
            return self.sized[address]
        index = bisect.bisect_right(self.starts, address)
        following = self.starts[index] if index < len(self.starts) else address + 0x1000
        return min(following, next(base + len(data) for base, data in self.code if base <= address < base + len(data))) - address

    def name(self, address):
        return self.names.get(address, hex(address))

    def read_map(self, path):
        """Input section ranges of the linker map: (start, end, unit path or library member)."""
        ranges = []
        for line in path.read_text().splitlines():
            match = re.match(r'\s*([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)\s+\d+\s+(\S+?):\((\S+)\)', line)
            if match and int(match.group(2), 16):
                start = int(match.group(1), 16)
                ranges.append((start, start + int(match.group(2), 16), match.group(3)))
        ranges.sort()
        self.range_starts = [r[0] for r in ranges]
        return ranges

    def defined(self, name, unit):
        """The addresses of the functions called name that unit defines."""
        return {a for a in self.symbols.get(name, ()) if unit in (self.unit(a) or '')}

    def unit(self, address):
        index = bisect.bisect_right(self.range_starts, address) - 1
        if index >= 0 and address < self.units[index][1]:
            return self.units[index][2]
        return None

    def address_taken(self):
        """Function address -> the units that take it: 8-byte data words, ADR, and ADRP with ADD."""
        taken = collections.defaultdict(set)
        for base, data in self.data:
            for offset in range(0, len(data) - 7, 8):
                value = struct.unpack_from('<Q', data, offset)[0]
                if value in self.functions:
                    taken[value].add(self.unit(base + offset))
        from capstone import arm64
        for address, size in self.sized.items():
            pages = {}
            for instruction in self.disassembler.disasm(self.words(address, size), address):
                if instruction.id == 0:  # data that SKIPDATA steps over
                    continue
                operands = instruction.operands
                if instruction.mnemonic == 'adrp':
                    pages[operands[0].reg] = operands[1].imm
                    continue
                value = None
                if instruction.mnemonic == 'adr':
                    value = operands[1].imm
                elif instruction.mnemonic == 'add' and len(operands) == 3 and operands[2].type == arm64.ARM64_OP_IMM \
                        and operands[1].reg in pages:
                    value = pages[operands[1].reg] + operands[2].imm
                if value in self.functions:
                    taken[value].add(self.unit(address))
        return taken


def is_fp(disassembler, instruction):
    from capstone import arm64
    if set(instruction.groups) & {arm64.ARM64_GRP_FPARMV8, arm64.ARM64_GRP_NEON}:
        return True
    if any(o.type == arm64.ARM64_OP_REG and FP_REGISTER.match(disassembler.reg_name(o.reg) or '') for o in instruction.operands):
        return True
    return instruction.mnemonic in ('mrs', 'msr') and re.search(r'\bfp[cs]r\b', instruction.op_str) is not None


def context_transfers(disassembler, instructions):
    """Addresses of the guarded task FP context saves and restores: CBZ over one complete transfer of Q0-Q31, FPSR, FPCR."""
    allowed = set()
    for index, instruction in enumerate(instructions):
        if instruction.mnemonic != 'cbz':
            continue
        target = int(instruction.op_str.split('#')[-1], 16)
        block = []
        for following in instructions[index + 1:]:
            if following.address >= target:
                break
            block.append(following)
        if not block or block[-1].address + 4 != target:
            continue
        registers, status, other = [], set(), False
        for item in block:
            text = item.op_str.replace(' ', '')
            if item.mnemonic in ('stp', 'ldp') and re.match(r'^q\d+,q\d+,\[sp', text):
                registers += [int(r) for r in re.findall(r'q(\d+)', text)]
            elif item.mnemonic in ('mrs', 'msr') and re.search(r'fp[cs]r', text):
                status.add(re.search(r'fp[cs]r', text).group(0))
            elif not (item.mnemonic in ('stp', 'ldp') and re.match(r'^w9,w10,\[sp', text)):
                other = True
        if not other and sorted(registers) == list(range(32)) and status == {'fpcr', 'fpsr'}:
            allowed.update(item.address for item in block)
    return allowed


BRANCHES = ('b', 'bl', 'cbz', 'cbnz', 'tbz', 'tbnz')


def instructions_from(image, address):
    """A sized function's instructions; for an assembly label, those reached from it by fall-through and by branches
    to addresses that start no symbol, so that code after an ERET (the port's unused weak handler) is not counted."""
    if address in image.sized:
        return list(image.disassembler.disasm(image.words(address, image.sized[address]), address)), set()
    found, pending, entered = {}, [address], set()
    while pending:
        at = pending.pop()
        while at not in found and image.in_code(at):
            if at != address and at in image.names:
                entered.add(at)  # falls or branches into another symbol: an edge to it
                break
            instruction = next(image.disassembler.disasm(image.words(at, 4), at), None)
            if instruction is None:
                break
            found[at] = instruction
            mnemonic, text = instruction.mnemonic, instruction.op_str
            if instruction.id == 0 or mnemonic in ('eret', 'ret', 'br'):
                break
            if (mnemonic in BRANCHES[2:] or mnemonic.startswith('b.')) and '#' in text:
                pending.append(int(text.split('#')[-1], 16))
            if mnemonic == 'b':
                at = int(text.split('#')[-1], 16)
                continue
            at += 4
    return [found[a] for a in sorted(found)], entered


def scan(image, address, cache):
    """(direct targets, indirect branch sites, FP instruction sites) of the function or label at address."""
    if address in cache:
        return cache[address]
    instructions, entered = instructions_from(image, address)
    inside = {i.address for i in instructions}
    allowed = context_transfers(image.disassembler, instructions)
    targets, indirect, fp = set(entered), [], []
    for instruction in instructions:
        mnemonic, text = instruction.mnemonic, instruction.op_str
        if instruction.id == 0:  # data that SKIPDATA steps over
            continue
        if is_fp(image.disassembler, instruction) and instruction.address not in allowed:
            fp.append('%x: %s %s' % (instruction.address, mnemonic, text))
        if mnemonic in BRANCHES or mnemonic.startswith('b.'):
            if '#' in text:
                target = int(text.split('#')[-1], 16)
                if target not in inside and (address in image.sized and not address <= target < address + image.sized[address]
                                             or address not in image.sized):
                    targets.add(target)
        elif mnemonic in ('br', 'blr'):
            indirect.append('%x: %s %s' % (instruction.address, mnemonic, text))
    cache[address] = (targets, indirect, fp)
    return cache[address]


def vector_entries(image):
    """The branch targets of the 16 slots of the EL1 vector table, walked instruction by instruction to the first symbol."""
    base = image.address[VECTORS]
    roots, seen, pending = set(), set(), [base + 0x80 * slot for slot in range(16)]
    while pending:
        at = pending.pop()
        while at not in seen:
            seen.add(at)
            if at != base and at in image.names and not image.name(at).startswith('.'):
                roots.add(at)
                break
            instruction = next(image.disassembler.disasm(image.words(at, 4), at), None)
            assert instruction is not None, 'undecodable vector code at %x' % at
            mnemonic, text = instruction.mnemonic, instruction.op_str
            if mnemonic in ('eret', 'ret'):
                break
            if mnemonic in ('br', 'blr'):
                raise AssertionError('indirect branch in the vector code at %x' % at)
            if mnemonic == 'bl' or mnemonic.startswith('b.') or mnemonic in ('cbz', 'cbnz', 'tbz', 'tbnz'):
                pending.append(int(text.split('#')[-1], 16))
            if mnemonic == 'b':
                at = int(text.split('#')[-1], 16)
                continue
            at += 4
    return roots


def walk(image, task_only, cache):
    """Reachable function -> (the caller it was first reached from, how)."""
    excluded = {a for name, unit in task_only for a in image.defined(name, unit)}
    candidates = sorted(a for a in image.taken if a not in excluded)
    seen, stack, indirect_done = {}, [(root, None, 'vector') for root in sorted(vector_entries(image))], False
    while stack:
        address, parent, how = stack.pop()
        if address in seen:
            continue
        seen[address] = (parent, how)
        targets, indirect, _ = scan(image, address, cache)
        stack.extend((t, address, 'direct') for t in targets if image.in_code(t))
        if indirect and not indirect_done:
            indirect_done = True
            stack.extend((t, address, 'indirect') for t in candidates)
    return seen


def reaches_fp(image, address, cache):
    """Whether the function at address, or a function its direct branches and calls reach, uses FP."""
    seen, stack = set(), [address]
    while stack:
        at = stack.pop()
        if at in seen:
            continue
        seen.add(at)
        targets, _, fp = scan(image, at, cache)
        if fp:
            return True
        stack.extend(t for t in targets if image.in_code(t))
    return False


def chain(image, seen, address, limit=8):
    names = []
    while address is not None and len(names) < limit:
        names.append(image.name(address) + (' (indirect)' if seen[address][1] == 'indirect' else ''))
        address = seen[address][0]
    return ' <- '.join(names)


def fp_problems(image, seen, cache):
    found = []
    for address in sorted(seen):
        fp = scan(image, address, cache)[2]
        if fp:
            found.append('%s uses FP or SIMD registers in exception context (%s): %s' %
                         (image.name(address), '; '.join(fp[:3]), chain(image, seen, address)))
    return found


def unit_problems(image, seen, commands, build):
    """Reachable functions of C and C++ units compiled without -mgeneral-regs-only."""
    flags = {}
    for entry in commands:
        output = entry.get('output') or ''
        if output:
            flags[str((Path(entry['directory']) / output).resolve())] = shlex.split(entry['command'])
    found, units = [], set()
    for address in seen:
        unit = image.unit(address)
        if not unit or '.a(' in unit or not re.search(r'\.(c|cc|cpp|cxx)\.obj$', unit):
            continue
        units.add(unit)
    for unit in sorted(units):
        arguments = flags.get(str((build / unit).resolve()))
        assert arguments is not None, 'no compile command for %s' % unit
        if '-mgeneral-regs-only' not in arguments:
            found.append('%s is reachable from an exception vector but is not compiled with -mgeneral-regs-only' % unit)
    return found, units


def task_only_problems(image, task_only, cache):
    """Each TASK_ONLY function present has its address taken only in its declared units."""
    found = []
    for (name, defined), (allowed, _) in task_only.items():
        for address in image.defined(name, defined):
            for unit in image.taken.get(address, ()):
                if not unit or not any(part in unit for part in allowed):
                    found.append('%s is task-only, but %s takes its address' % (name, unit))
    return found


def check(build, elf, map_path, task_only=TASK_ONLY, commands=None, mutate=None):
    image = Image(build, elf, map_path)
    if mutate:
        mutate(image)
    commands = commands if commands is not None else json.loads((build / 'compile_commands.json').read_text())
    cache = {}
    seen = walk(image, task_only, cache)
    found = fp_problems(image, seen, cache)
    units_found, units = unit_problems(image, seen, commands, build)
    found += units_found + task_only_problems(image, task_only, cache)
    return image, seen, units, found, cache


def port_setting(entry):
    """configSS_IRQ_SAVES_FP as the compile command of one FreeRTOS port assembly unit defines it: '0', '1', or None
    for the unpatched port, whose IRQ entry has never saved the FP registers."""
    arguments = shlex.split(entry['command']) if 'command' in entry else list(entry['arguments'])
    at = arguments.index('-o')
    arguments = [a for a in arguments[:at] + arguments[at + 2:] if a != '-c'] + ['-E', '-dM', '-o', '-']
    macros = subprocess.run(arguments, cwd=entry['directory'], capture_output=True, text=True, check=True).stdout
    match = re.search(r'^#define configSS_IRQ_SAVES_FP (\S+)$', macros, re.M)
    return match.group(1) if match else None


def linked_images(build):
    """(target, ELF, linker map, port setting) of each executable the build configures that links a port assembly unit,
    from compile_commands.json and the link commands CMake writes (CMakeFiles/TARGET.dir/link.txt)."""
    commands = json.loads((build / 'compile_commands.json').read_text())
    ports = {entry['output']: entry for entry in commands if entry['file'].endswith('/portASM.S')}
    settings, found = {}, []
    links = sorted((build / 'CMakeFiles').glob('*.dir/link.txt'))
    assert links, 'no link commands in %s: the check reads the link.txt files of the Unix Makefiles generator' % build
    for link in links:
        words = shlex.split(link.read_text())
        linked = [w for w in words if w in ports]
        if not linked:
            continue
        values = {settings.setdefault(w, port_setting(ports[w])) for w in linked}
        assert len(values) == 1 and '-o' in words, (link, values)
        maps = [w[len('-Wl,-Map,'):] for w in words if w.startswith('-Wl,-Map,')]
        found.append((link.parent.name[:-len('.dir')], build / words[words.index('-o') + 1],
                      build / maps[0] if maps else None, values.pop()))
    return found


UNSAVED = re.compile(r'^-DSUNSTONE_IRQ_FP_UNSAVED(:BOOL)?=(ON|1|TRUE|YES|Y)$', re.I)


def gate_builds():
    """Every image recipe of tools/gate_images.py and OS variant of tools/integration-gate.py: name -> whether its
    configuration sets SUNSTONE_IRQ_FP_UNSAVED (cmake/irq-fp.cmake)."""
    spec = importlib.util.spec_from_file_location('gate_images', ROOT / 'tools/gate_images.py')
    images = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(images)
    tree = ast.parse((ROOT / 'tools/integration-gate.py').read_text())
    variants = next(ast.literal_eval(node.value) for node in tree.body if isinstance(node, ast.Assign)
                    and any(isinstance(t, ast.Name) and t.id == 'VARIANTS' for t in node.targets))
    sets = lambda flags: any(UNSAVED.match(flag) for flag in flags)  # noqa: E731
    return {**{name: sets(recipe['configure']) for name, recipe in images.IMAGES.items()},
            **{name: sets(options) for name, options in variants.items()}}


def cache_sets_unsaved(build):
    """Whether the CMake cache of a build has SUNSTONE_IRQ_FP_UNSAVED on."""
    cache = (build / 'CMakeCache.txt').read_text()
    return re.search(r'^SUNSTONE_IRQ_FP_UNSAVED:BOOL=(ON|1|TRUE|YES)$', cache, re.M | re.I) is not None


def prove(build, elf, map_path):
    """Walk one image, then require each mutation to fail."""
    image, seen, units, found, cache = check(build, elf, map_path)
    assert not found, '%s:\n%s' % (elf, '\n'.join(found[:20]))
    irq = image.address['FreeRTOS_IRQ_Handler']
    assert irq in seen and image.address.get('vApplicationIRQHandler') in seen, 'the walk misses the IRQ handler'
    assert 'platform_panic' not in image.address or image.address['platform_panic'] in seen, \
        'the walk misses the panic path'
    transfers = sum(len(context_transfers(image.disassembler, list(image.disassembler.disasm(
        image.words(a, image.extent(a)), a)))) for a in seen if a in image.names and
        image.name(a) in ('FreeRTOS_IRQ_Handler', 'FreeRTOS_SWI_Handler'))
    reaching = sorted(key for key in TASK_ONLY if image.defined(*key) & set(image.taken))
    mutations = []
    commands = json.loads((build / 'compile_commands.json').read_text())
    # 1: an FP instruction (FMOV D0, XZR) planted at the entry of a function that only an indirect call reaches.
    victim = next(a for a in sorted(seen) if seen[a][1] == 'indirect' and a in image.sized and image.unit(a)
                  and '.c.obj' in image.unit(a))
    _, _, _, bad, _ = check(build, elf, map_path, commands=commands, mutate=lambda i: i.patch(victim, 0x9e6703e0))
    assert any(line.startswith(image.name(victim) + ' uses FP') for line in bad), (image.name(victim), bad[:3])
    mutations.append('planted FMOV in %s' % image.name(victim))
    # 2: the CBZ that guards the IRQ path's task context save replaced by a NOP.
    guard = next(i.address for i in image.disassembler.disasm(image.words(irq, image.extent(irq)), irq)
                 if i.mnemonic == 'cbz' and context_transfers(image.disassembler, list(image.disassembler.disasm(
                     image.words(i.address, 0x60), i.address))))
    _, _, _, bad, _ = check(build, elf, map_path, commands=commands, mutate=lambda i: i.patch(guard, 0xd503201f))
    assert any(line.startswith('FreeRTOS_IRQ_Handler uses FP') for line in bad), bad[:3]
    mutations.append('unguarded the context save at %x' % guard)
    # 3: -mgeneral-regs-only removed from the unit of vApplicationIRQHandler.
    unit = image.unit(image.address['vApplicationIRQHandler'])
    changed = copy.deepcopy(commands)
    for entry in changed:
        if str((build / entry.get('output', '')).resolve()) == str((build / unit).resolve()):
            entry['command'] = entry['command'].replace('-mgeneral-regs-only', '')
    _, _, _, bad, _ = check(build, elf, map_path, commands=changed)
    assert any('-mgeneral-regs-only' in line and unit in line for line in bad), bad[:3]
    mutations.append('removed -mgeneral-regs-only from %s' % unit.split('.dir/')[-1])
    # 4: a task-only function that reaches FP made a candidate of the indirect calls. The entry is one whose direct
    # calls reach FP in this image: the secondary cores' entries reach FP only through the GPU image's boot path. The
    # unmutated image has no FP problem, so any FP line is the drop's.
    to_fp = [key for key in reaching if any(reaches_fp(image, a, cache) for a in image.defined(*key))]
    if to_fp:
        key = to_fp[0]
        reduced = {k: v for k, v in TASK_ONLY.items() if k != key}
        _, _, _, bad, _ = check(build, elf, map_path, task_only=reduced, commands=commands)
        assert any('uses FP' in line for line in bad), (key, bad[:3])
        mutations.append('dropped the task-only entry of %s (%s)' % key)
    assert len(mutations) >= 3, mutations
    print('%s: %d functions reachable from the EL1 vectors, %d units built with -mgeneral-regs-only, no FP outside '
          'the guarded task context transfers; refused: %s' % (elf.name, len(seen), len(units), '; '.join(mutations)))
    return {'image': str(elf), 'functions': len(seen), 'units': len(units), 'context_transfers': transfers,
            'address_taken': len(image.taken), 'task_only_present': ['%s (%s)' % key for key in reaching],
            'mutations_refused': mutations}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--walk', action='append', type=Path, required=True,
                        help='a gate build that sets SUNSTONE_IRQ_FP_UNSAVED, configured with compile_commands.json '
                             'and built: every executable it links with configSS_IRQ_SAVES_FP 0 is walked')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    named = {m.group(1) for at, part in enumerate(GATE_RUNS[0][1:], 1) if GATE_RUNS[0][at - 1] == '--walk'
             for m in re.finditer(r'\{build:([A-Za-z0-9_-]+)\}', part)}
    setting = {name for name, unsaved in gate_builds().items() if unsaved}
    assert setting == named, ('GATE_RUNS must walk exactly the gate builds that set SUNSTONE_IRQ_FP_UNSAVED: it walks %s, '
                              'and %s set it' % (sorted(named), sorted(setting)))
    results, configured, walked = [], {}, set()
    for build in args.walk:
        assert (build / 'compile_commands.json').is_file(), 'no compile_commands.json in %s' % build
        assert cache_sets_unsaved(build), '%s does not set SUNSTONE_IRQ_FP_UNSAVED' % build
        for target, elf, map_path, value in linked_images(build):
            if value != '0':
                continue
            configured.setdefault(target, set()).add(str(build))
            if not elf.is_file():
                continue
            assert map_path and map_path.is_file(), 'no linker map for %s' % elf
            result = prove(build, elf, map_path)
            results.append(dict(result, target=target, build=str(build)))
            walked.add(target)
    unwalked = sorted(set(configured) - walked)
    assert not unwalked, ('these executables link a port with configSS_IRQ_SAVES_FP 0, but no build given to the check '
                          'built them: %s' % ', '.join(unwalked))
    (args.output / 'result.json').write_text(json.dumps(results, indent=1))
    print('PASS exception code uses general registers only in %d images of %d executables with configSS_IRQ_SAVES_FP 0'
          % (len(results), len(walked)))


if __name__ == '__main__':
    main()
