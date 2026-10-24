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
  a struct a733_pmic_native other than as the &...operations third argument
  of a733_pmic_init(), names struct a733_pmic_ops, or uses the ops of a
  struct a733_pmic other than to read its now or outside query. Any such
  use could reach the accessors without the poll's task check: a call, a
  copy of the table, or a function pointer passed on. The rule is keyed on
  the declared type: it collects the names that each translation unit, with
  the project headers it includes, declares as struct a733_pmic_native or
  struct a733_pmic, including functions that return one. A member access
  counts as a use when the expression before it ends in such a name, or
  ends in ')' or ']' and the brackets contain such a name or a cast to the
  type, or follow such a name: (&native)->operations,
  ((struct a733_pmic_native *)x)->operations, natives[0].operations and
  get()->operations all count.

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


def opening(text, close):
    """The index of the bracket that text[close], a ')' or ']', closes; -1 if none."""
    pair, depth = {')': '(', ']': '['}[text[close]], 0
    for i in range(close, -1, -1):
        depth += text[i] == text[close]
        depth -= text[i] == pair
        if not depth:
            return i
    return -1


def operand(text, end):
    """The start of the postfix expression that ends before text[end], and its object's possible names.

    The names are the last name of the expression, such as a variable, a
    member, an array or a called function, and every name inside the
    brackets that follow it, so that (&x)->, ((T *)x)-> and x[0]. all yield x.
    Names further out denote enclosing objects and are not returned; a cast
    stays in the expression text."""
    names, i, last = set(), end, True
    while True:
        while i and text[i - 1].isspace():
            i -= 1
        if i and text[i - 1] in ')]':
            j = opening(text, i - 1)
            if j < 0:
                break
            if last:
                names |= set(re.findall(r'[A-Za-z_]\w*', text[j:i]))
            i = j
            continue
        name = re.search(r'[A-Za-z_]\w*$', text[max(0, i - 256):i])
        if not name:
            break
        if last:
            names.add(name[0])
        last = False
        i -= len(name[0])
        k = i
        while k and text[k - 1].isspace():
            k -= 1
        if text[k - 1:k] == '.':
            i = k - 1
        elif text[k - 2:k] == '->':
            i = k - 2
        else:
            break
    return i, names


def init_argument(text, start, end):
    """Whether &text[start:end] is the whole third argument, the operations, of a733_pmic_init()."""
    if not re.search(r'&\s*$', text[max(0, start - 64):start]) or not re.match(r'\s*,', text[end:]):
        return False
    depth, commas = 0, 0
    for i in range(start - 1, -1, -1):
        depth += {')': 1, ']': 1, '}': 1, '(': -1, '[': -1, '{': -1}.get(text[i], 0)
        commas += not depth and text[i] == ','
        if depth < 0:
            return (text[i] == '(' and commas == 2 and
                    re.search(r'\ba733_pmic_init\s*$', text[max(0, i - 64):i]) is not None)
    return False


ACCESS_NATIVE = re.compile(r'(?:\.|->)\s*operations\b')
ACCESS_PROVIDER = re.compile(r'(?:\.|->)\s*ops\b')
QUERY = re.compile(r'\s*\.\s*(?:now|outside)\b(?!\s*=[^=])')


def adapter_uses(root):
    """Uses of the native adapter's operations, keyed on the declared type."""
    problems = []
    for path in files(root):
        rel = path.relative_to(root)
        if rel in ADAPTER_FILES:
            continue
        own = strip_comments(path.read_text(errors='replace'))
        if not re.search(r'\b(operations|ops|a733_pmic_ops)\b', own):
            continue
        unit = unit_text(root, path.resolve(), set())
        natives = set(re.findall(r'struct\s+a733_pmic_native\b[\s*]*(?:const\b[\s*]*)?(\w+)', unit))
        providers = set(re.findall(r'struct\s+a733_pmic\b[\s*]*(?:const\b[\s*]*)?(\w+)\s*[;,=)(\[]', unit))
        for m in re.finditer(r'\bstruct\s+a733_pmic_ops\b', own):
            line = own.count('\n', 0, m.start()) + 1
            problems.append(f'{rel}:{line}: names struct a733_pmic_ops outside the provider and its adapter')
        for m in ACCESS_NATIVE.finditer(own):
            begin, names = operand(own, m.start())
            if not names & natives and not re.search(r'\bstruct\s+a733_pmic_native\b', own[begin:m.start()]):
                continue
            if not init_argument(own, begin, m.end()):
                line = own.count('\n', 0, m.start()) + 1
                problems.append(f'{rel}:{line}: uses the native PMIC adapter\'s operations other than as '
                                'a733_pmic_init()\'s argument')
        # Only the now and outside queries, which the power service and
        # consumers make, may be read from a provider's copy of the adapter
        # operations. A call of any other operation, a copy of the table or a
        # pointer into it could reach the accessors without the poll's task check.
        for m in ACCESS_PROVIDER.finditer(own):
            begin, names = operand(own, m.start())
            if not names & providers and not re.search(r'\bstruct\s+a733_pmic\b', own[begin:m.start()]):
                continue
            if QUERY.match(own, m.end()):
                continue
            line = own.count('\n', 0, m.start()) + 1
            problems.append(f'{rel}:{line}: uses the PMIC provider\'s copy of the adapter operations other '
                            'than the now and outside queries')
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
          'per-phase time read or per-access ownership check, no task check per access, adapter '
          'operations used only by the provider and the bundle forwarders, and only the now and outside '
          'queries read from a provider\'s copy')


if __name__ == '__main__':
    main()
