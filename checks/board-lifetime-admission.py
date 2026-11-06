#!/usr/bin/env python3
"""Check that the board contract check selects its timing and watchdog terms from the verified contract magic.

apps/board/admission.c checks the board prefix of every OS contract (apps/os/admission.c passes the board magic of the
lifetime that the OS magic names). The OS unit defines A733_BOARD_ADMITS_RESIDENT before it includes the file, so one OS
image admits both lifetimes; a board image admits its own supervised magic alone. This check compiles the file for the host
both ways, with the board session of the supervised layout, and requires:
  os, supervised        the supervised contract (60 s deadline, WDT0 fields zero) is admitted;
  os, resident          the resident contract (no deadline, expiry at armed plus 4 s, WDT0 mode 0x41, configuration 1,
                        output 2, no FEL flag or redirect) is admitted;
  os, crossed           the resident magic with the supervised timing, and the supervised magic with the resident timing,
                        are refused as timing (diagnostic);
  os, FEL flag          a resident contract with the FEL flag set is refused as timing (watchdog_fields);
  os, power policy      the per-core power policy word 0x07052000 + core * 0x1000 is admitted for a write only while the EL3
                        lifetime word (a733_smp_lifetime) is resident;
  board, resident       the resident contract is refused as magic_version (magic), and the policy word is never admitted.
Negative control: the OS build with the lifetime branch forced to the supervised terms fails the resident case.
Usage: board-lifetime-admission.py --output DIR
"""
import argparse
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/board/admission.c', 'include/a733_board_application.h', 'include/a733_admission_refusal.h',
                'platform/a733-smp-cached/boot.h', 'drivers/storage/storage_digest.c']
HARNESS = r'''
#include "a733_board_application.h"
#include "a733_admission_refusal.h"
#include "a733_storage_digest.h"
#include "boot.h"
#include <stdio.h>
#include <string.h>
const unsigned char a733_smp_build_id[32] = {1};
unsigned a733_smp_lifetime;
uint64_t a733_smp_result_magic(void) { return 0; }
#define HZ A733_SMP_COUNTER_HZ
static int failures;
static void expect(const char *name, unsigned got, unsigned want)
{
    printf("%s %s refusal=0x%x want=0x%x\n", got == want ? "ok" : "FAIL", name, got, want);
    if (got != want) ++failures;
}
int main(void)
{
    struct a733_board_session s = {.magic = A733_BOARD_SESSION_MAGIC, .version = A733_BOARD_SESSION_VERSION,
        .bytes = sizeof s, .epoch = 1, .generation = 2, .lease = 3, .topology = 4, .cold_master = 5,
        .memory_authority = 6, .recovery_authority = 7, .workload = 2, .pmic_owner = 10, .usb0_owner = 11,
        .fabric_hz = {24000000, 24000000, 24000000}, .usb_supply_uv = {3300000, 3300000, 1200000, 900000},
        .usb0_cold_reference = 1};
    memset(s.image, 1, 32); memset(s.qualification, 2, 32); memset(s.recovery, 2, 32);
    memset(s.wiring, 3, 32); memset(s.memory, 4, 32);
    const uint64_t armed = 24, now = 100;
    struct a733_smp_contract supervised = {.magic = A733_BOARD_SUPERVISED_CONTRACT_MAGIC,
        .version = A733_BOARD_CONTRACT_VERSION, .bytes = sizeof supervised, .epoch = 1, .generation = 2,
        .inverse_generation = ~UINT64_C(2), .lease = 3, .armed_at = armed,
        .startup_deadline = armed + A733_BOARD_STARTUP_SECONDS * HZ,
        .diagnostic_deadline = armed + A733_BOARD_SUPERVISED_SECONDS * HZ,
        .expires_at = armed + A733_BOARD_SUPERVISED_SECONDS * HZ,
        .reserved = {A733_BOARD_SESSION_ADDRESS, sizeof s}};
    memcpy(supervised.build_id, a733_smp_build_id, 32);
    storage_digest_bytes(&s, sizeof s, (uint8_t *)&supervised.reserved[2]);
    struct a733_smp_contract resident = supervised;
    resident.magic = A733_BOARD_RESIDENT_CONTRACT_MAGIC;
    resident.diagnostic_deadline = A733_SMP_NO_DEADLINE; resident.expires_at = armed + 4 * HZ;
    resident.watchdog_mode = 0x41; resident.watchdog_configuration = 1; resident.watchdog_output = 2;
    const unsigned diagnostic = A733_REFUSAL(A733_REFUSAL_TIMING, A733_TIMING_DIAGNOSTIC); (void)diagnostic;
#ifdef A733_BOARD_ADMITS_RESIDENT
    expect("os-supervised", a733_board_contract_refusal(&supervised, &s, a733_smp_build_id, now), 0);
    expect("os-resident", a733_board_contract_refusal(&resident, &s, a733_smp_build_id, now), 0);
    struct a733_smp_contract c = supervised; c.magic = A733_BOARD_RESIDENT_CONTRACT_MAGIC;
    expect("os-resident-magic-supervised-timing", a733_board_contract_refusal(&c, &s, a733_smp_build_id, now), diagnostic);
    c = resident; c.magic = A733_BOARD_SUPERVISED_CONTRACT_MAGIC;
    expect("os-supervised-magic-resident-timing", a733_board_contract_refusal(&c, &s, a733_smp_build_id, now), diagnostic);
    c = resident; c.fel_flag = 0x5aa5a55a;
    expect("os-resident-fel-flag", a733_board_contract_refusal(&c, &s, a733_smp_build_id, now),
           A733_REFUSAL(A733_REFUSAL_TIMING, A733_TIMING_WATCHDOG_FIELDS));
    for (unsigned lifetime = 0; lifetime < 3; ++lifetime) {
        a733_smp_lifetime = lifetime;
        for (unsigned core = 1; core < 8; ++core) {
            char name[64]; snprintf(name, sizeof name, "os-policy-lifetime%u-core%u", lifetime, core);
            expect(name, (unsigned)a733_smp_power_address(core, UINT32_C(0x07052000) + core * 0x1000, 1),
                   lifetime == A733_LIFETIME_RESIDENT);
        }
    }
#else
    expect("board-supervised", a733_board_contract_refusal(&supervised, &s, a733_smp_build_id, now), 0);
    expect("board-resident", a733_board_contract_refusal(&resident, &s, a733_smp_build_id, now),
           A733_REFUSAL(A733_REFUSAL_MAGIC_VERSION, A733_MAGIC_MAGIC));
    a733_smp_lifetime = A733_LIFETIME_RESIDENT;
    expect("board-policy", (unsigned)a733_smp_power_address(1, UINT32_C(0x07053000), 1), 0);
#endif
    return failures != 0;
}
'''


def build_and_run(out, name, admission, os_image):
    (out / (name + '.c')).write_text(HARNESS)
    binary = out / name
    cmd = [harness.CLANG, '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', *harness.SANITIZE,
           '-DSUNSTONE_BOARD_SUPERVISED=1', *(['-DA733_BOARD_ADMITS_RESIDENT=1'] if os_image else []),
           f'-I{ROOT}/platform/a733-smp-cached', f'-I{ROOT}/include', f'-I{ROOT}/apps/board',
           str(out / (name + '.c')), str(admission), str(ROOT / 'drivers/storage/storage_digest.c'), '-o', str(binary)]
    done = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    assert done.returncode == 0, done.stderr
    done = subprocess.run([str(binary)], capture_output=True, text=True, env=harness.SANITIZER_ENV)
    (out / (name + '.log')).write_text(done.stdout + done.stderr)
    return done.returncode, done.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    out = parser.parse_args().output
    out.mkdir(parents=True, exist_ok=True)
    admission = ROOT / 'apps/board/admission.c'
    code, text = build_and_run(out, 'os', admission, True)
    assert code == 0 and text.count('ok ') == 26, text
    code, text = build_and_run(out, 'board', admission, False)
    assert code == 0 and text.count('ok ') == 3, text
    # Negative control: the timing branch forced to the supervised terms refuses the resident contract.
    source = admission.read_text()
    forced = source.replace('    if(lifetime==A733_LIFETIME_RESIDENT){', '    if(0){', 1)
    assert forced != source
    (out / 'admission-forced.c').write_text(forced)
    code, text = build_and_run(out, 'forced', out / 'admission-forced.c', True)
    assert code != 0 and 'FAIL os-resident ' in text, text
    print('PASS board lifetime admission: the OS build admits both lifetimes by magic with their own timing terms, '
          'refuses crossed terms, and admits the power policy word only for the resident lifetime; the board build '
          'refuses the resident magic; forcing the supervised terms fails the resident case')


if __name__ == '__main__':
    main()
