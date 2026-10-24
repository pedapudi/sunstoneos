#!/usr/bin/env python3
"""Check the source-tree facts that keep PMIC outputs under the provider's control.

The power-domain owner (docs/power-domains.md) reads DCDC2 and DCDC4 once per
boot, so no agent the OS controls may change those outputs afterwards. This
check fails when:

- a PSCI CPU_OFF, CPU_SUSPEND or SYSTEM_SUSPEND function identifier appears in
  OS sources, because firmware may change PMIC outputs on those calls;
- the PMIC provider admits a selector write to any output other than DCDC3 and
  DCDC5, or its native adapter can write any register other than the S_TWI0
  data and control registers;
- a source outside drivers/power names the S_TWI0 interrupt (GIC-600 INTID
  235), which only the provider may use;
- the native adapter's time read, which the provider makes once per bus
  phase, calls the counter clock (a global ticket lock with interrupts
  masked), or its register accessors query the kernel's mutex holder (a
  kernel critical section) or check the task: owned(), read32() and write32()
  must not call current() or an a733_task_context_ predicate, because
  a733_pmic_poll() checks the provider task at entry and the adapter's
  enter() when it takes the I2C mutex;
- outside the adapter itself and the board-bundle forwarders and cost
  windows (apps/board-bundles-power/power.c), a source uses the operations of
  a struct a733_pmic_native other than as the &...operations argument of
  a733_pmic_init(), names struct a733_pmic_ops, or uses the register,
  mutex or retention operations among the ops of a struct a733_pmic. Any
  such use could reach the accessors without the
  poll's task check: a call, a copy of the table, or a function pointer
  passed on. The rule is keyed on the declared type: it collects the names
  that each translation unit, with the project headers it includes, declares
  as struct a733_pmic_native or struct a733_pmic.

It reads source text only and runs no build.
"""
import argparse
import re
from pathlib import Path
if not __debug__:
    raise SystemExit('checks require assertions; unset PYTHONOPTIMIZE and do not pass -O')

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ['drivers', 'platform', 'apps', 'boot', 'arch', 'runtime', 'include', 'patches']
# SMC32 and SMC64 forms.
PSCI = {'CPU_SUSPEND': (0x84000001, 0xc4000001), 'CPU_OFF': (0x84000002,),
        'SYSTEM_SUSPEND': (0x8400000e, 0xc400000e)}


def files(root):
    for d in SOURCES:
        base = root / d
        if base.is_dir():
            yield from (p for p in base.rglob('*') if p.suffix in {'.c', '.h', '.S', '.s', '.patch', '.cc'})


ADAPTER_FILES = {Path('drivers/power/a733_pmic_native.c'), Path('include/a733_pmic_native.h'),
                 Path('drivers/power/a733_pmic.c'), Path('include/a733_pmic.h'),
                 Path('apps/board-bundles-power/power.c'), Path('apps/board-bundles-power/power.h')}


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def unit_text(root, path, seen):
    """The comment-free text of a source and of the project headers it includes, recursively."""
    if path in seen or not path.is_file():
        return ''
    seen.add(path)
    text = strip_comments(path.read_text(errors='replace'))
    parts = [text]
    for name in re.findall(r'#\s*include\s+"([^"]+)"', text):
        for base in (path.parent, root / 'include', root / 'platform/a733'):
            if (base / name).is_file():
                parts.append(unit_text(root, (base / name).resolve(), seen))
                break
    return '\n'.join(parts)


def statement(text, start, end):
    """The statement around text[start:end]: from the previous ';', '{' or '}' to the next ';'."""
    left = max(text.rfind(c, 0, start) for c in ';{}') + 1
    right = text.find(';', end)
    return text[left:right if right >= 0 else len(text)]


def adapter_uses(root):
    """Uses of the native adapter's operations, keyed on the declared type."""
    problems = []
    for path in files(root):
        rel = path.relative_to(root)
        if rel in ADAPTER_FILES:
            continue
        own = strip_comments(path.read_text(errors='replace'))
        if not re.search(r'\b(a733_pmic_native|a733_pmic|a733_pmic_ops)\b', own):
            continue
        unit = unit_text(root, path.resolve(), set())
        natives = set(re.findall(r'struct\s+a733_pmic_native\s*\*?\s*(?:const\s+)?(\w+)', unit))
        providers = set(re.findall(r'struct\s+a733_pmic\s*\*?\s*(?:const\s+)?(\w+)\s*[;,=)\[]', unit))
        for m in re.finditer(r'\bstruct\s+a733_pmic_ops\b', own):
            line = own.count('\n', 0, m.start()) + 1
            problems.append(f'{rel}:{line}: names struct a733_pmic_ops outside the provider and its adapter')
        for m in re.finditer(r'\b(\w+)\s*(?:\.|->)\s*operations\b', own):
            if m.group(1) not in natives:
                continue
            line = own.count('\n', 0, m.start()) + 1
            text = statement(own, m.start(), m.end())
            argument = re.search(r'a733_pmic_init\s*\([^;]*&\s*[\w.\->]*\b' + m.group(1) +
                                 r'\s*(?:\.|->)\s*operations\s*,', text)
            if not argument:
                problems.append(f'{rel}:{line}: uses the native PMIC adapter\'s operations other than as '
                                'a733_pmic_init()\'s argument')
        # The register, mutex and retention operations; now and outside are
        # read-only queries that the power service and consumers make.
        for m in re.finditer(r'\b(\w+)\s*(?:\.|->)\s*ops\s*\.\s*(?:read|write|enter|leave|retain)\b', own):
            if m.group(1) in providers:
                line = own.count('\n', 0, m.start()) + 1
                problems.append(f'{rel}:{line}: uses the PMIC provider\'s copy of the adapter operations')
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', type=Path, default=ROOT)
    root = ap.parse_args().root.resolve()
    numbers = re.compile(r'0x[0-9a-fA-F]+')
    named = re.compile(r'PSCI_(CPU_OFF|CPU_SUSPEND|SYSTEM_SUSPEND)\b')
    interrupt = re.compile(r'\b235\b')
    hints = re.compile(r'irq|intid|interrupt|gic|spi', re.I)
    banned = {v for ids in PSCI.values() for v in ids}
    problems = []
    scanned = 0
    for path in files(root):
        rel = path.relative_to(root)
        text = path.read_text(errors='replace')
        scanned += 1
        for n, line in enumerate(text.splitlines(), 1):
            if named.search(line) or any(int(x, 16) in banned for x in numbers.findall(line)):
                problems.append(f'{rel}:{n}: PSCI power-off or suspend call: {line.strip()}')
            if interrupt.search(line) and hints.search(line) and rel.parts[:2] != ('drivers', 'power'):
                problems.append(f'{rel}:{n}: names INTID 235 outside the PMIC provider: {line.strip()}')
    assert scanned > 100, f'only {scanned} source files under {root}'
    pmic = (root / 'drivers/power/a733_pmic.c').read_text()
    writable = re.search(r'static int writable\([^)]*\)\s*\{(.*?)\n\}', pmic, re.S)
    assert writable, 'writable() not found'
    rails = set(re.findall(r'A733_RAIL_[A-Z0-9]+', writable[1]))
    if rails != {'A733_RAIL_DCDC3', 'A733_RAIL_DCDC5'}:
        problems.append(f'drivers/power/a733_pmic.c: writable() admits {sorted(rails)}')
    native = (root / 'include/a733_pmic_native.h').read_text()
    allow = re.search(r'if\(writing\)return (.*?);', native)
    if not allow or set(re.findall(r'0x[0-9a-fA-F]+', allow[1])) != {'0x07083008', '0x0708300c'}:
        problems.append('include/a733_pmic_native.h: the native adapter may write more than S_TWI0 data and control')
    adapter = (root / 'drivers/power/a733_pmic_native.c').read_text()

    def body(name):
        found = re.search(r'static int ' + name + r'\([^)]*\)\s*\{(.*?)\n\}', adapter, re.S)
        assert found, f'{name}() not found in the native adapter'
        return found[1]
    if re.search(r'counter_clock_now|locks->now', body('now')):
        problems.append('drivers/power/a733_pmic_native.c: now() takes the counter clock lock')
    for name in ['owned', 'read32', 'write32']:
        if 'xSemaphoreGetMutexHolder' in body(name):
            problems.append(f'drivers/power/a733_pmic_native.c: {name}() queries the mutex holder per access')
        if re.search(r'\bcurrent\(|a733_task_context_', body(name)):
            problems.append(f'drivers/power/a733_pmic_native.c: {name}() checks the task per access')
    problems += adapter_uses(root)
    for p in problems:
        print(p)
    assert not problems, f'{len(problems)} problem(s)'
    print(f'PASS PMIC tree facts: no PSCI CPU_OFF or suspend in {scanned} sources, selector writes '
          'only to DCDC3 and DCDC5, INTID 235 only in the provider, no lock in the native adapter\'s '
          'per-phase time read or per-access ownership check, no task check per access, adapter register '
          'operations called only by the provider and the bundle forwarders')


if __name__ == '__main__':
    main()
