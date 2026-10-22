#!/usr/bin/env python3
"""Check the source-tree facts that the once-per-boot supply verification relies on.

The domain owner reads DCDC2, DCDC4, CLDO2 and CLDO5 once per boot and then
trusts the state word, so no agent the OS controls may change those outputs
afterwards. This check fails when:

- a PSCI CPU_OFF, CPU_SUSPEND or SYSTEM_SUSPEND function identifier appears in
  OS sources, because firmware may change PMIC outputs on those calls;
- the PMIC provider admits a selector write to any output other than DCDC3 and
  DCDC5, or its native adapter can write any register other than the S_TWI0
  data and control registers;
- a source outside drivers/power names the S_TWI0 interrupt (GIC-600 INTID
  235), which only the provider may use.

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
    for p in problems:
        print(p)
    assert not problems, f'{len(problems)} problem(s)'
    print(f'PASS PMIC tree facts: no PSCI CPU_OFF or suspend in {scanned} sources, selector writes '
          'only to DCDC3 and DCDC5, INTID 235 only in the provider')


if __name__ == '__main__':
    main()
