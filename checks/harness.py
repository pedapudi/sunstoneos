#!/usr/bin/env python3
"""Shared host-check policy and helpers: assertions, sanitizer flags, host tools, refusals,
resumable steps, ELF layouts and ranged emulation hooks.

Import this module before a check makes any pass/fail decision. Checks rely on
Python assert statements, so importing it under `python3 -O` or PYTHONOPTIMIZE
stops the check instead of letting removed assertions record a pass.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import sys
import subprocess
import traceback

if not __debug__:
    raise SystemExit('checks require assertions; unset PYTHONOPTIMIZE and do not pass -O')

CACHE = Path(os.environ.get('SUNSTONE_CACHE', Path.home() / '.cache/sunstoneos'))
# UBSan and ASan both stop at the first report; without -fno-sanitize-recover,
# UBSan prints 'runtime error:' and the check still exits 0. Clang and GCC accept it.
SANITIZE = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
# Backstop for executables whose build recovers from UBSan reports.
SANITIZER_ENV = {'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
CLANG = Path(os.environ.get('SUNSTONE_HOST_CLANG', '/usr/bin/clang-21'))
LLD = Path(os.environ.get('SUNSTONE_HOST_LLD', CACHE / 'host/usr/bin/ld.lld-21'))


def copy_image(source, destination):
    """Copy a disk image without writing its empty blocks.

    The sweeps of fat32-rw.py copy a 52 MiB card image tens of thousands of times, and the zero blocks are almost all of it
    (55 of 13,312 blocks hold data). A dense copy writes the whole file each time and saturated the disk of the host the
    board runner shares; `cp --sparse=always --reflink=auto` clones the file where the filesystem can and otherwise writes only
    the blocks with data."""
    subprocess.run(['cp', '--sparse=always', '--reflink=auto', '--', str(source), str(destination)], check=True)
    log = os.environ.get('SUNSTONE_COPY_LOG')            # a measurement hook: one line per copy, "apparent allocated" in bytes
    if log:
        info = os.stat(destination)
        with open(log, 'a') as stream:
            stream.write(f'{info.st_size} {info.st_blocks * 512}\n')


def available_gib():
    """MemAvailable in GiB, or None when the system does not report it."""
    try:
        for line in Path('/proc/meminfo').read_text().splitlines():
            if line.startswith('MemAvailable:'):
                return int(line.split()[1]) / 1048576
    except (OSError, ValueError, IndexError):
        pass
    return None


_ram_directories = []


def _release_ram_directories(*_):
    import shutil
    while _ram_directories:
        shutil.rmtree(_ram_directories.pop(), ignore_errors=True)


def ram_directory(prefix):
    """A new directory in RAM (/dev/shm) for disk images that the check discards, so that a run by hand does not write them to the
    shared disk. The gate's temporary directory is already there. The caller removes the directory."""
    import tempfile
    base = Path(tempfile.gettempdir())
    if not str(base.resolve()).startswith('/dev/shm'):
        base = Path('/dev/shm') if os.access('/dev/shm', os.W_OK) else base
    directory = Path(tempfile.mkdtemp(prefix=prefix, dir=base))
    if not _ram_directories:
        import atexit
        import signal
        atexit.register(_release_ram_directories)
        if signal.getsignal(signal.SIGTERM) == signal.SIG_DFL:
            signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))      # so that atexit runs when the gate stops the check
    _ram_directories.append(directory)
    return directory


def machine():
    """The one emulated machine for checks that execute an integrated OS image.

    The images use the large-system atomics (CAS, LDADD) that the default emulated core lacks, so a check on the default core
    stops at the first such instruction. The core model must be chosen before any memory is mapped, so the machine is made here
    and the caller maps and loads it. checks/fault-capture.py builds its machine the same way."""
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
    from unicorn.arm64_const import UC_CPU_ARM64_MAX
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.ctl_set_cpu_model(UC_CPU_ARM64_MAX)
    return uc


def environment():
    """Child environment: no bytecode files in source snapshots, halting sanitizers."""
    return {**os.environ, 'PYTHONDONTWRITEBYTECODE': '1', **SANITIZER_ENV}


def refused(action, reason, kind=ValueError):
    """Run a negative case and return its refusal message.

    The case passes only when action() raises `kind` with `reason` in its
    message. An accepted input fails the case. Any other exception propagates,
    so a fixture error or an unrelated earlier refusal cannot stand in for the
    check that the case exercises. `kind` may be a tuple of exception types.
    """
    try:
        action()
    except kind as error:
        message = str(error)
        assert reason in message, f'refused for another reason: {message!r}; expected {reason!r}'
        return message
    raise AssertionError(f'accepted; expected a refusal containing {reason!r}')


def artifact_refused(action, directory):
    """Require destination refusal while preserving every entry and file digest."""
    def snapshot():
        entries = {}
        for path in directory.rglob('*'):
            mode = stat.S_IFMT(path.lstat().st_mode)
            if mode == stat.S_IFLNK:
                content = os.readlink(path)
            elif mode == stat.S_IFREG:
                with path.open('rb') as stream:
                    content = hashlib.file_digest(stream, 'sha256').hexdigest()
            else:
                content = None
            entries[path.relative_to(directory)] = (mode, content)
        return entries
    before = snapshot()
    refused(action, str(directory), FileExistsError)
    assert snapshot() == before, 'Refused artifact creation changed its destination'


class Image:
    """Loadable segments, symbols, functions and structure layouts of an ELF image.

    parts holds (address, bytes) for each PT_LOAD segment; symbols maps a symbol
    name to its addresses; functions maps a (compilation unit, name) pair to the
    addresses of DWARF subprograms and variables maps it to the addresses of
    statically allocated DWARF variables; layouts maps a structure name to its member
    offsets. layout_names selects the structures a fixture consumes, because
    unrelated compilation units may declare different private structures with
    the same name; any other repeated name must have one layout.
    """

    def __init__(self, path, layout_names=None):
        from elftools.elf.elffile import ELFFile
        from elftools.dwarf.dwarf_expr import DWARFExprParser
        with Path(path).open('rb') as f:
            e = ELFFile(f)
            dwarf = e.get_dwarf_info()
            expressions = DWARFExprParser(dwarf.structs)
            self.parts = [(int(p['p_vaddr']), p.data()) for p in e.iter_segments() if p['p_type'] == 'PT_LOAD']
            self.symbols = {}
            self.functions = {}
            self.variables = {}
            self.layouts = {}
            self.read_only = set()
            for s in e.get_section_by_name('.symtab').iter_symbols():
                self.symbols.setdefault(s.name, set()).add(int(s['st_value']))
                index = s['st_shndx']
                if isinstance(index, int) and not e.get_section(index)['sh_flags'] & 1:       # SHF_WRITE clear
                    self.read_only.add((s.name, int(s['st_value'])))
            for cu in dwarf.iter_CUs():
                unit = cu.get_top_DIE().attributes['DW_AT_name'].value.decode()
                for d in cu.iter_DIEs():
                    # An out-of-line copy of a function that is also inlined
                    # carries its name on the abstract instance it points to.
                    if (d.tag == 'DW_TAG_subprogram' and 'DW_AT_name' not in d.attributes and
                            'DW_AT_low_pc' in d.attributes and 'DW_AT_abstract_origin' in d.attributes):
                        origin = d.get_DIE_from_attribute('DW_AT_abstract_origin')
                        if 'DW_AT_name' in origin.attributes:
                            self.functions.setdefault((unit, origin.attributes['DW_AT_name'].value.decode()), set()).add(
                                int(d.attributes['DW_AT_low_pc'].value))
                    if 'DW_AT_name' not in d.attributes:
                        continue
                    n = d.attributes['DW_AT_name'].value.decode()
                    if d.tag == 'DW_TAG_subprogram' and 'DW_AT_low_pc' in d.attributes:
                        self.functions.setdefault((unit, n), set()).add(int(d.attributes['DW_AT_low_pc'].value))
                    location = d.attributes.get('DW_AT_location')
                    if d.tag == 'DW_TAG_variable' and location is not None and location.form == 'DW_FORM_exprloc':
                        ops = expressions.parse_expr(location.value)
                        if len(ops) == 1 and ops[0].op_name in ('DW_OP_addr', 'DW_OP_addrx'):
                            at = ops[0].args[0] if ops[0].op_name == 'DW_OP_addr' else dwarf.get_addr(cu, ops[0].args[0])
                            self.variables.setdefault((unit, n), set()).add(int(at))
                    if d.tag == 'DW_TAG_structure_type' and (layout_names is None or n in layout_names):
                        fields = {c.attributes['DW_AT_name'].value.decode(): int(c.attributes['DW_AT_data_member_location'].value)
                                  for c in d.iter_children() if c.tag == 'DW_TAG_member' and 'DW_AT_name' in c.attributes
                                  and 'DW_AT_data_member_location' in c.attributes}
                        if fields:
                            if n in self.layouts:
                                assert self.layouts[n] == fields, n
                            self.layouts[n] = fields

    def address(self, n, unit=None):
        """The one nonzero address of symbol n, or of function or static variable n in the unit whose name ends with unit."""
        values = self.symbols.get(n, set()) - {0}
        if len(values) > 1 and not unit:
            # Several objects share a short name (the read-only board description beside the writable board application
            # state): when exactly one is writable, the name means that one, which is what the checks poke.
            writable = {v for v in values if (n, v) not in self.read_only}
            if len(writable) == 1:
                values = writable
        if unit:
            values = set().union(*(v for table in (self.functions, self.variables)
                                   for (u, name), v in table.items() if name == n and u.endswith(unit)))
        assert len(values) == 1, (n, unit, values)
        return next(iter(values))


def instruction_sites(parts, match):
    """Addresses of the 32-bit words in loaded image parts for which match(word) is true.

    parts holds (address, bytes) pairs. Data words that happen to match are
    included too; code hooks on them never run.
    """
    return [base + index * 4 for base, data in parts
            for index, (word,) in enumerate(struct.iter_unpack('<I', data[:len(data) & ~3])) if match(word)]


# AArch64 DSB, DMB and ISB with every operand bit masked: (mask, value).
BARRIERS = {'dsb': (0xfffff0ff, 0xd503309f), 'dmb': (0xfffff0ff, 0xd50330bf), 'isb': (0xfffff0ff, 0xd50330df)}


def barriers(word):
    """The names of the BARRIERS entries that word encodes."""
    return [name for name, (mask, value) in BARRIERS.items() if word & mask == value]


def trace_sections(log, classify, names):
    """Instruction counts per section of a QEMU log written with -d int,exec,nochain,in_asm.

    A section begins at each semihosting call that CPU 0 takes and ends at the
    next one. It counts the instructions CPU 0 executes in it, from the
    translated blocks (in_asm) and their executions (exec, nochain), and, for
    each name in names, the instructions for which classify(word) returns a
    list containing that name. Code before the first call is not counted.
    """
    blocks, sections, counts, words, address = {}, [], None, None, None
    exec_line = re.compile(r'^Trace 0: \S+ \[[0-9a-f]+/([0-9a-f]+)/')
    with Path(log).open(errors='replace') as stream:
        for line in stream:
            if line.startswith('IN:'):
                words, address = [], None
                continue
            if words is not None:
                if address is None and line.startswith('0x'):
                    address = int(line.split(':')[0], 16)
                    continue
                if line.startswith('OBJD-T:'):
                    data = bytes.fromhex(line.split(':', 1)[1].strip())
                    words += [int.from_bytes(data[i:i + 4], 'little') for i in range(0, len(data), 4)]
                    continue
                blocks[address] = words
                words = None
            if 'Taking exception 16 [Semihosting call] on CPU 0' in line:
                counts = {'instructions': 0, **{name: 0 for name in names}}
                sections.append(counts)
                continue
            match = exec_line.match(line)
            if match and counts is not None:
                for word in blocks[int(match[1], 16)]:
                    counts['instructions'] += 1
                    for name in classify(word):
                        counts[name] += 1
    return sections


def system_register_access(word):
    """True for the AArch64 MRS, MSR (register), SYS and SYSL encodings.

    SYS includes address translation (AT) and cache and TLB maintenance.
    Hints such as WFE, barriers and MSR (immediate) do not match.
    """
    return word & 0xffd00000 == 0xd5100000 or word & 0xffd80000 == 0xd5080000


# SVC #0x5c0d: the EL1 breadcrumb call of returning images (include/sunstone_crumb.h).
CRUMB_SVC = 0xd40b81a1
# SVC #0x5c0e: the EL1 first-failure record call (board_fail_at, board_fail_note).
FAIL_SVC = 0xd40b81c1


SEALED_READY, SEALED_LIFETIME, LIFETIME_RETURN = 4096, 4100, 3     # apps/os/sealed-lifetime.h, include/a733_board_application.h


def seal_lifetime(uc, image, lifetime=LIFETIME_RETURN):
    """Give a function-level model the sealed session's lifetime copy (apps/os/sealed.c), which os_returning() and
    BOARD_RESIDENT() read: the ready flag and the lifetime. An image without the sealed object is left as it is."""
    if 'os_sealed' in image.symbols:
        base = image.address('os_sealed')
        uc.mem_write(base + SEALED_READY, (1).to_bytes(4, 'little'))
        uc.mem_write(base + SEALED_LIFETIME, int(lifetime).to_bytes(4, 'little'))


def skip_crumbs(uc, image, crumbs=None, failures=None):
    """Step over each breadcrumb and first-failure SVC in image as the EL1 vector would return from it.

    Function-level models have no exception vectors, so an SVC would stop
    emulation. When crumbs is a list, (stage, value) is appended for each
    breadcrumb call; when failures is a list, (x0, x1) is appended for each
    first-failure call. Return the hooked addresses.
    """
    from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_X0, UC_ARM64_REG_X1

    def step(machine, address, size, unused):
        word = int.from_bytes(machine.mem_read(address, 4), "little")
        log = crumbs if word == CRUMB_SVC else failures
        if log is not None:
            log.append((machine.reg_read(UC_ARM64_REG_X0), machine.reg_read(UC_ARM64_REG_X1)))
        machine.reg_write(UC_ARM64_REG_PC, address + 4)
    return hook_addresses(uc, instruction_sites(image.parts, lambda word: word in (CRUMB_SVC, FAIL_SVC)), step)


def hook_addresses(uc, addresses, callback):
    """Register callback as a Unicorn code hook on each address alone.

    A code hook without a range calls into Python before every emulated
    instruction. A model that acts only at known addresses, such as an end
    sentinel, hooked symbols and system-register accesses, runs many times
    faster when it hooks just those addresses. Return the hooked addresses.
    """
    from unicorn import UC_HOOK_CODE
    addresses = set(addresses)
    for address in sorted(addresses):
        uc.hook_add(UC_HOOK_CODE, callback, begin=address, end=address)
    return addresses


def save(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + '\n')


def _free(out, stem, attempt):
    while True:
        path = out / (stem + ('' if attempt == 1 else '-attempt' + str(attempt)) + '.log')
        if not path.exists():
            return attempt, path
        attempt += 1


class Steps:
    """Labelled commands and cases recorded in <out>/steps.json for --run-only reruns.

    A label that passed with the same command is skipped. A rerun appends to the
    existing command log, writes <label>-attempt<N>.log beside earlier logs and
    moves the earlier step record into the label's history, so a failed
    attempt's log, exit status and command all survive the retry.
    """

    def __init__(self, out, commands_name):
        self.out = Path(out)
        self.path = self.out / 'steps.json'
        self.steps = json.loads(self.path.read_text()) if self.path.exists() else {}
        self.commands_path = self.out / commands_name
        self.commands = json.loads(self.commands_path.read_text()) if self.commands_path.exists() else []

    def save(self):
        save(self.path, self.steps)

    def _history(self, name):
        previous = self.steps.get(name, {})
        earlier = {key: value for key, value in previous.items() if key != 'history'}
        return previous, previous.get('history', []) + ([earlier] if earlier else [])

    def command(self, label, values, timeout=None):
        values = list(map(str, values))
        previous, history = self._history(label)
        if previous.get('exit') == 0:
            if previous['command'] != values:
                raise ValueError(label + ' passed with a different command')
            return
        self.commands.append(values)
        save(self.commands_path, self.commands)
        attempt, log = _free(self.out, label, previous.get('attempt', 1 if previous else 0) + 1)
        self.steps[label] = {'state': 'running', 'command': values, 'attempt': attempt, 'log': str(log), 'history': history}
        self.save()
        try:
            result = subprocess.run(values, capture_output=True, env=environment(), timeout=timeout)
            output, code = result.stdout + result.stderr, result.returncode
        except subprocess.TimeoutExpired as error:
            output = (error.stdout or b'') + (error.stderr or b'') + b'\nDeclared bounded process deadline expired.\n'
            code = 124
        log.write_bytes(output)
        self.steps[label].update(exit=code, state='complete')
        self.save()
        if code:
            raise RuntimeError(label + ' failed; inspect preserved log')

    def case(self, name, result=None):
        """Callback for *-cases.py modules: True when already passed, else record the attempt."""
        previous, history = self._history(name)
        if result is None:
            if previous.get('exit') == 0:
                return True
            self.steps[name] = {'state': 'running', 'history': history}
            self.save()
            return False
        self.steps[name] = {'exit': 0, **result, 'history': previous.get('history', [])}
        self.save()

    def failed(self, error, stem='runtime-failure'):
        """Preserve the current traceback and mark interrupted cases failed."""
        _, log = _free(self.out, stem, 1)
        log.write_text(traceback.format_exc())
        for value in self.steps.values():
            if value.get('state') == 'running':
                value.update(state='failed', error=str(error), failure_log=str(log))
        self.save()
