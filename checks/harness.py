#!/usr/bin/env python3
"""Shared host-check policy: assertions, sanitizer flags, host tools, refusals and resumable steps.

Import this module before a check makes any pass/fail decision. Checks rely on
Python assert statements, so importing it under `python3 -O` or PYTHONOPTIMIZE
stops the check instead of letting removed assertions record a pass.
"""
import json
import os
from pathlib import Path
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


def instruction_sites(parts, match):
    """Addresses of the 32-bit words in loaded image parts for which match(word) is true.

    parts holds (address, bytes) pairs. Data words that happen to match are
    included too; code hooks on them never run.
    """
    return [base + index * 4 for base, data in parts
            for index, (word,) in enumerate(struct.iter_unpack('<I', data[:len(data) & ~3])) if match(word)]


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
