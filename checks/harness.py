#!/usr/bin/env python3
"""Shared host-check policy and helpers: assertions, sanitizer flags, host tools, refusals,
resumable steps, ELF layouts and ranged emulation hooks.

Import this module before a check makes any pass/fail decision. Checks rely on
Python assert statements, so importing it under `python3 -O` or PYTHONOPTIMIZE
stops the check instead of letting removed assertions record a pass.
"""
import json
import os
from pathlib import Path
import re
import struct
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


class Image:
    """Loadable segments, symbols, functions and structure layouts of an ELF image.

    parts holds (address, bytes) for each PT_LOAD segment; symbols maps a symbol
    name to its addresses; functions maps a (compilation unit, name) pair to the
    addresses of DWARF subprograms; layouts maps a structure name to its member
    offsets. layout_names selects the structures a fixture consumes, because
    unrelated compilation units may declare different private structures with
    the same name; any other repeated name must have one layout.
    """

    def __init__(self, path, layout_names=None):
        from elftools.elf.elffile import ELFFile
        with Path(path).open('rb') as f:
            e = ELFFile(f)
            self.parts = [(int(p['p_vaddr']), p.data()) for p in e.iter_segments() if p['p_type'] == 'PT_LOAD']
            self.symbols = {}
            self.functions = {}
            self.layouts = {}
            for s in e.get_section_by_name('.symtab').iter_symbols():
                self.symbols.setdefault(s.name, set()).add(int(s['st_value']))
            for cu in e.get_dwarf_info().iter_CUs():
                unit = cu.get_top_DIE().attributes['DW_AT_name'].value.decode()
                for d in cu.iter_DIEs():
                    if 'DW_AT_name' not in d.attributes:
                        continue
                    n = d.attributes['DW_AT_name'].value.decode()
                    if d.tag == 'DW_TAG_subprogram' and 'DW_AT_low_pc' in d.attributes:
                        self.functions.setdefault((unit, n), set()).add(int(d.attributes['DW_AT_low_pc'].value))
                    if d.tag == 'DW_TAG_structure_type' and (layout_names is None or n in layout_names):
                        fields = {c.attributes['DW_AT_name'].value.decode(): int(c.attributes['DW_AT_data_member_location'].value)
                                  for c in d.iter_children() if c.tag == 'DW_TAG_member' and 'DW_AT_name' in c.attributes
                                  and 'DW_AT_data_member_location' in c.attributes}
                        if fields:
                            if n in self.layouts:
                                assert self.layouts[n] == fields, n
                            self.layouts[n] = fields

    def address(self, n, unit=None):
        """The one nonzero address of symbol n, or of function n in the unit whose name ends with unit."""
        values = self.symbols.get(n, set()) - {0}
        if unit:
            values = set().union(*(v for (u, name), v in self.functions.items() if name == n and u.endswith(unit)))
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
