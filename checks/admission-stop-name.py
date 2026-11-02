#!/usr/bin/env python3
"""Check that the generic EL3 admission (platform/a733-smp-cached) names its stop, and that the naming fails when it is removed.

platform/a733-smp-cached/boot.c stops a refused contract with a733_smp_stop(1, 1). The stop passes its detail through
a733_smp_refusal_detail (a macro after the stop's definition), and platform/a733-smp-cached/admission.c answers with the
refused term of include/a733_admission_refusal.h. This check compiles admission.c for the host and, for a contract admitted
by the board's rules and then changed one field at a time, requires that a733_smp_contract_valid refuses it and that
a733_smp_refusal_detail(1, 1) returns the term tools/a733-crumbs.py decodes:
  magic, version, size, epoch, inverse generation, lease, build ID, device-memory words, armed time in the future, startup
  deadline passed, diagnostic deadline, expiry, and a watchdog field.
It also requires that an admitted contract and any other stop keep their detail, that boot.c routes every a733_smp_stop call
through the macro, and (negative control) that admission.c with its recording removed leaves the stop unnamed and fails the
same cases.
Usage: admission-stop-name.py --output DIR
"""
import argparse
import importlib.util
from pathlib import Path
import re
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['platform/a733-smp-cached/boot.c', 'platform/a733-smp-cached/admission.c', 'include/a733_admission_refusal.h',
                'tools/a733-crumbs.py']
HARNESS = r'''
#include "boot.h"
#include "a733_admission_refusal.h"
#include <stdio.h>
#include <string.h>
const unsigned char a733_smp_build_id[32] = {1, 2, 3};
unsigned a733_smp_contract_refusal(const struct a733_smp_contract *, uint64_t);
int main(void)
{
    uint64_t now = 1000000000ull, hz = 24000000ull;
    struct a733_smp_contract c;
    memset(&c, 0, sizeof c);
    c.magic = A733_SMP_CONTRACT_MAGIC; c.version = 1; c.bytes = sizeof c; c.epoch = 5; c.generation = 7;
    c.inverse_generation = ~7ull; c.lease = 9; memcpy(c.build_id, a733_smp_build_id, 32);
    c.armed_at = now - 100; c.startup_deadline = now + 1000; c.expires_at = c.armed_at + 4 * hz;
    c.diagnostic_deadline = c.expires_at - hz;
    c.watchdog_mode = 0x41; c.watchdog_configuration = 1; c.watchdog_output = 2; c.fel_flag = 0x5aa5a55a;
    c.redirect_flag = 0xfa50392f; c.redirect_entry = 0x20;
    struct a733_smp_contract m[16]; uint64_t at[16]; int n = 0;
#define CASE(edit, t) m[n] = c; edit; at[n++] = (t)
    CASE(m[n].magic ^= 1, now);
    CASE(m[n].version = 2, now);
    CASE(m[n].bytes = 8, now);
    CASE(m[n].epoch = 0, now);
    CASE(m[n].inverse_generation = 0, now);
    CASE(m[n].lease = 0, now);
    CASE(m[n].build_id[0] ^= 1, now);
    CASE(m[n].reserved[2] = 1, now);
    CASE(m[n].armed_at = now + 5, now);
    CASE((void)0, now + 2 * hz);
    CASE(m[n].diagnostic_deadline = m[n].expires_at, now);
    CASE(m[n].expires_at += 1, now);
    CASE(m[n].watchdog_mode = 0, now);
    CASE((void)0, now);
    printf("%d\n", n);
    for (int i = 0; i < n; ++i) {
        int ok = a733_smp_contract_valid(&m[i], at[i]);
        printf("%d %x %x %x\n", ok, (unsigned)a733_smp_contract_refusal(&m[i], at[i]), ok ? 1u : (unsigned)a733_smp_refusal_detail(1, 1),
               (unsigned)a733_smp_refusal_detail(3, 0x30d));
    }
    return 0;
}
'''
EXPECTED = [('magic_version', 'magic'), ('magic_version', 'version'), ('magic_version', 'size'), ('contract_field', 'identity'),
            ('contract_field', 'identity'), ('contract_field', 'identity'), ('build_id', None), ('device_memory', None),
            ('timing', 'armed'), ('timing', 'startup'), ('timing', 'diagnostic'), ('timing', 'expiry'),
            ('timing', 'watchdog_fields'), None]


def build_and_run(out, name, admission):
    (out / (name + '.c')).write_text(HARNESS)
    binary = out / name
    cmd = [harness.CLANG, '-std=gnu11', '-O1', '-w', '-DA733_SMP_CONTRACT_MAGIC=0x1234ULL', f'-I{ROOT}/platform/a733-smp-cached',
           f'-I{ROOT}/include', str(out / (name + '.c')), str(admission), '-o', str(binary)]
    done = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    assert done.returncode == 0, done.stderr
    done = subprocess.run([str(binary)], capture_output=True, text=True)
    assert done.returncode == 0, done.stderr
    lines = done.stdout.split('\n')
    return [tuple(int(x, 16) for x in line.split()) for line in lines[1:] if line]


def named(crumbs, detail):
    n = crumbs.refusal_detail(detail)
    return (n['term'], n.get('sub')) if n else None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    out = parser.parse_args().output
    out.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location('stop_name_crumbs', ROOT / 'tools/a733-crumbs.py')
    crumbs = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(crumbs)
    admission = ROOT / 'platform/a733-smp-cached/admission.c'
    rows = build_and_run(out, 'named', admission)
    assert len(rows) == len(EXPECTED), rows
    for i, (row, expected) in enumerate(zip(rows, EXPECTED)):
        ok, refusal, stop, other = row
        if expected is None:
            assert ok == 1 and refusal == 0, (i, row)
        else:
            assert ok == 0 and refusal == stop and named(crumbs, stop) == expected, (i, row, named(crumbs, stop), expected)
        assert other == 0x30d, (i, 'another stop lost its detail', row)
    # Negative control: the same admission without its recording leaves the stop unnamed, so the cases fail.
    text = admission.read_text()
    broken = re.sub(r'    refusal\.state=why\?2:1;refusal\.why=why;\n', '', text)
    assert broken != text
    (out / 'admission-broken.c').write_text(broken)
    bad = build_and_run(out, 'unrecorded', out / 'admission-broken.c')
    failures = [i for i, (row, expected) in enumerate(zip(bad, EXPECTED)) if expected and not (row[0] == 0 and row[1] == row[2])]
    assert len(failures) == sum(1 for e in EXPECTED if e), ('the naming did not fail when removed', failures)
    # boot.c: the macro follows the stop's definition and precedes every call.
    boot = (ROOT / 'platform/a733-smp-cached/boot.c').read_text()
    definition = boot.index('_Noreturn void a733_smp_stop(')
    macro = boot.index('#define a733_smp_stop(')
    calls = [m.start() for m in re.finditer(r'\ba733_smp_stop\(', boot)]
    after = [c for c in calls if c > macro]
    assert definition < macro and all(c < macro for c in calls if c <= definition + 40) and len(after) >= 8, (definition, macro, calls)
    assert 'a733_smp_refusal_detail((status),(detail))' in boot
    print(f'PASS admission stop name: {len(EXPECTED) - 1} refusals named, admitted contract and other stops unchanged; '
          f'{len(failures)} cases fail with the recording removed; {len(after)} stop calls routed through the macro')


if __name__ == '__main__':
    main()
