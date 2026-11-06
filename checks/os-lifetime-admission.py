#!/usr/bin/env python3
"""Check that the linked OS image's EL3 admission selects the lifetime from the verified contract magic.

The check executes the image's own a733_smp_contract_valid (with a733_smp_contract_refusal and
a733_board_contract_refusal), a733_smp_refusal_detail, a733_smp_result_magic and a733_smp_power_address under the shared Unicorn machine (checks/os-sealed-session.py), at
EL3 with modeled system registers. Two callees are replaced, because their rules do not depend on the lifetime and
have their own checks: os_session_decode_why returns success with the board prefix copied into the decoded session,
and a733_board_session_valid returns success. Every other term runs: the OS magic, the board magic of the same
lifetime, the timing and watchdog terms, the build ID, the identity fields and both digests. For each image it
requires:

  one OS image (a build without SUNSTONE_A733_OS_CPU_RETURN; os_contract_profile names both OS magics)
    supervised   the supervised contract is admitted and sets a733_smp_lifetime to 1; the result magic is the
                 supervised one; the power policy word 0x07052000 + core * 0x1000 is not writable; the session
                 decode ran with os_admitting_lifetime 1, which is 0 again afterwards; os_lifetime returns 1 at EL3,
                 and at EL1 the sealed copy, without a load of either monitor word;
    resident     the resident contract (no diagnostic deadline, expiry at armed plus 4 s, WDT0 mode 0x41,
                 configuration 1, output 2, no FEL flag or redirect) is admitted and sets the word to 2; the result
                 magic is the resident one; the policy word of cores 1 to 7 is writable; the decode and
                 os_lifetime follow 2 as above;
    crossed      the resident magic with the supervised timing is refused as timing (diagnostic), the supervised
                 magic with the resident timing likewise, and either refusal leaves the word 0;
    FEL flag     a resident contract with the FEL flag set is refused as timing (watchdog_fields);
    once         after one lifetime is admitted, a contract of the other lifetime is refused as magic_version (magic)
                 and the word keeps its value; a second contract of the same lifetime is admitted;
  returning image (SUNSTONE_A733_OS_CPU_RETURN): the supervised contract is admitted with the word 1, and the
    resident contract is refused as magic_version (magic) with the word 0.
The lifetime and startup seconds come from the build's compile commands for apps/os/admission.c (or the returning
profile's patched copy, os-image/return-admission.c).
Usage: os-lifetime-admission.py --image ELF [--image ELF ...] --output DIR
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shlex
import struct
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'checks'))
import harness  # noqa: E402  Refuses to run with assertions disabled.
from unicorn import UC_HOOK_MEM_READ  # noqa: E402

GATE_RUNS = [['--image', '{elf:sup}', '--image', '{elf:ret-npu-load}', '--output', '{output}']]
CHECK_INPUTS = ['checks/os-sealed-session.py', 'include/a733_admission_refusal.h', 'include/a733_board_application.h']
spec = importlib.util.spec_from_file_location('os_lifetime_sealed', ROOT / 'checks/os-sealed-session.py')
sealed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sealed)
HZ = 24_000_000
SESSION, CONTRACT = 0x41f00000, 0x43200000
NO_DEADLINE = (1 << 64) - 1
HEADER = (ROOT / 'include/a733_board_application.h').read_text()
REFUSAL = (ROOT / 'include/a733_admission_refusal.h').read_text()


def constant(text, name):
    found = re.search(r'#define\s+%s\s+(?:UINT64_C\()?(0x[0-9a-fA-F]+|\d+)' % name, text)
    assert found, name
    return int(found[1], 0)


MAGIC = {name: constant(HEADER, name) for name in ('A733_OS_CONTRACT_MAGIC', 'A733_OS_RESIDENT_CONTRACT_MAGIC',
                                                    'A733_OS_RESULT_MAGIC', 'A733_OS_RESIDENT_RESULT_MAGIC')}


def refusal(term, sub):
    """A733_REFUSAL(term, sub) of include/a733_admission_refusal.h: 0x100 | term << 4 | sub."""
    assert re.search(r'#define A733_REFUSAL\(term, sub\) \(0x100u \| \(unsigned\)\(term\) << 4 \| \(unsigned\)\(sub\)\)', REFUSAL)
    value = {name: int(v) for name, v in re.findall(r'\b(A733_[A-Z_]+) = (\d+)', REFUSAL)}
    return 0x100 | value[term] << 4 | value[sub]


def build_seconds(image):
    """(lifetime seconds, startup seconds) for apps/os/admission.c in the image's build (the image's directory)."""
    commands = json.loads((Path(image).parent / 'compile_commands.json').read_text())
    # The returning profile compiles a patched copy (os-image/return-admission.c, cmake/a733-os-cpu-return.cmake).
    rows = [r for r in commands if r['file'].endswith(('apps/os/admission.c', 'os-image/return-admission.c'))]
    assert len(rows) == 1, (image, len(rows))
    words = rows[0].get('arguments') or shlex.split(rows[0]['command'])
    defines = dict(w[2:].split('=', 1) for w in words if w.startswith('-D') and '=' in w)
    seconds = {'A733_BOARD_SUPERVISED_SECONDS': 60, 'A733_BOARD_STARTUP_SECONDS': 1}
    for name in seconds:
        if name in defines:
            seconds[name] = int(defines[name].rstrip('uU'), 0)
    return seconds['A733_BOARD_SUPERVISED_SECONDS'], seconds['A733_BOARD_STARTUP_SECONDS'], 'SUNSTONE_A733_OS_CPU_RETURN' in defines


class Admission:
    """One fresh machine on the image: EL3 state, the session at its board address, the contract in DRAM."""

    def __init__(self, image):
        self.image = image
        self.m = sealed.Machine(image)
        L = image.layouts
        self.prefix_bytes = 592
        board = L['a733_board_session']
        self.identity = bytes(self.m.uc.mem_read(image.address('a733_smp_build_id'), 32))
        prefix = bytearray(self.prefix_bytes)
        struct.pack_into('<3Q', prefix, board['epoch'], 11, 22, 33)
        prefix[board['image']:board['image'] + 32] = self.identity
        self.session = bytes(prefix) + bytes((i * 5 + 1) & 0xff for i in range(432))
        self.m.uc.mem_write(SESSION, self.session)
        decoded_board = L['os_session']['board']
        self.m.stub('a733_board_session_valid', 1)
        address = image.address('os_session_decode_why')
        # The lifetime the decode applies (os_lifetime at EL3 reads os_admitting_lifetime first), at each decode.
        self.decoded = []

        def decode(uc, address, size, unused):
            self.decoded.append(self.word('os_admitting_lifetime'))
            out = uc.reg_read(sealed.GPR[2])
            uc.mem_write(out + decoded_board, bytes(uc.mem_read(SESSION, self.prefix_bytes)))
            uc.mem_write(uc.reg_read(sealed.GPR[3]), struct.pack('<I', 0))
            uc.reg_write(sealed.GPR[0], 1)
            uc.reg_write(sealed.UC_ARM64_REG_PC, uc.reg_read(sealed.UC_ARM64_REG_LR))
        harness.hook_addresses(self.m.uc, [address], decode)

    def contract(self, magic, resident, seconds, startup, fel=0):
        c = self.image.layouts['a733_smp_contract']
        raw = bytearray(c['reserved'] + 48)
        armed = 24
        values = {'magic': magic, 'version': 2, 'bytes': len(raw), 'epoch': 11, 'generation': 22,
                  'inverse_generation': ~22 & NO_DEADLINE, 'lease': 33, 'armed_at': armed,
                  'startup_deadline': armed + startup * HZ}
        if resident:
            values.update(diagnostic_deadline=NO_DEADLINE, expires_at=armed + 4 * HZ, watchdog_mode=0x41,
                          watchdog_configuration=1, watchdog_output=2, fel_flag=fel)
        else:
            values.update(diagnostic_deadline=armed + seconds * HZ, expires_at=armed + seconds * HZ)
        for name, value in values.items():
            struct.pack_into('<Q', raw, c[name], value)
        raw[c['build_id']:c['build_id'] + 32] = self.identity
        struct.pack_into('<2Q', raw, c['reserved'], SESSION, len(self.session))
        raw[c['reserved'] + 16:c['reserved'] + 48] = hashlib.sha256(self.session).digest()
        return bytes(raw)

    def admit(self, contract, now=HZ // 10):
        """0 when a733_smp_contract_valid admits the contract; otherwise the detail the EL3 stop records for the refused
        admission (a733_smp_refusal_detail(1, 1)), which is the board's stop detail."""
        self.m.uc.mem_write(CONTRACT, contract)
        if self.m.call('a733_smp_contract_valid', CONTRACT, now) == 1:
            return 0
        return self.m.call('a733_smp_refusal_detail', 1, 1)

    def word(self, name):
        return struct.unpack('<I', self.m.uc.mem_read(self.image.address(name), 4))[0]

    def lifetime(self):
        return self.word('a733_smp_lifetime')

    def el1_lifetime(self, sealed_copy):
        """os_lifetime at EL1, with board_session_lifetime (apps/os/sealed.c) returning sealed_copy; also the loads of the
        two monitor words it made, which EL1 cannot map."""
        monitor = [self.image.address(n) for n in ('a733_smp_lifetime', 'os_admitting_lifetime')]
        loads = []
        hooks = [self.m.uc.hook_add(UC_HOOK_MEM_READ, lambda uc, access, address, size, value, unused:
                                    loads.append(address), begin=w, end=w + 3) for w in monitor]
        self.m.stub('board_session_lifetime', sealed_copy)
        el, model = self.m.el, dict(self.m.model)
        self.m.at_el1()
        try:
            return self.m.call('os_lifetime'), loads
        finally:
            for h in hooks:
                self.m.uc.hook_del(h)
            self.m.el, self.m.model = el, model

    def result_magic(self):
        self.m.call('a733_smp_result_magic')
        return self.m.uc.reg_read(sealed.GPR[0])

    def policy_writable(self):
        return [self.m.call('a733_smp_power_address', core, 0x07052000 + core * 0x1000, 1) for core in range(1, 8)]


def one_image(path):
    image = sealed.Image(path)
    image.layouts.update(harness.Image(path, layout_names={'os_session'}).layouts)
    seconds, startup, returning = build_seconds(path)
    profile = struct.unpack('<2Q', bytes(sealed.Machine(image).uc.mem_read(image.address('os_contract_profile'), 16)))
    sup, res = MAGIC['A733_OS_CONTRACT_MAGIC'], MAGIC['A733_OS_RESIDENT_CONTRACT_MAGIC']
    cases = {}
    magic_refused = refusal('A733_REFUSAL_MAGIC_VERSION', 'A733_MAGIC_MAGIC')
    diagnostic = refusal('A733_REFUSAL_TIMING', 'A733_TIMING_DIAGNOSTIC')
    fields = refusal('A733_REFUSAL_TIMING', 'A733_TIMING_WATCHDOG_FIELDS')
    if returning:
        assert profile == (sup, 0), [hex(p) for p in profile]
        a = Admission(image)
        assert a.admit(a.contract(sup, False, seconds, startup)) == 0 and a.lifetime() == 1
        cases['returning-admits-supervised'] = True
        a = Admission(image)
        assert a.admit(a.contract(res, True, seconds, startup)) == magic_refused and a.lifetime() == 0
        cases['returning-refuses-resident-magic'] = True
        return seconds, cases
    assert profile == (sup, res), [hex(p) for p in profile]
    for name, magic, resident, word, result in (('supervised', sup, False, 1, MAGIC['A733_OS_RESULT_MAGIC']),
                                                ('resident', res, True, 2, MAGIC['A733_OS_RESIDENT_RESULT_MAGIC'])):
        a = Admission(image)
        assert a.lifetime() == 0
        assert a.admit(a.contract(magic, resident, seconds, startup)) == 0, name
        assert a.lifetime() == word, (name, a.lifetime())
        assert a.result_magic() == result, (name, hex(a.result_magic()))
        assert a.policy_writable() == [int(resident)] * 7, (name, a.policy_writable())
        cases[f'{name}-admitted-sets-lifetime-{word}'] = True
        # The decode applied the terms of the contract's lifetime; afterwards EL3 reads the admitted word and EL1 the sealed
        # copy, without a load of monitor memory.
        assert a.decoded == [word] and a.word('os_admitting_lifetime') == 0, (name, a.decoded)
        assert a.m.call('os_lifetime') == word, name
        assert a.el1_lifetime(word) == (word, []) and a.el1_lifetime(0) == (0, []), name
        cases[f'{name}-decode-and-os-lifetime-follow-{word}'] = True
        # Once written: the other lifetime is refused; the same lifetime again is admitted.
        assert a.admit(a.contract(sup if resident else res, not resident, seconds, startup)) == magic_refused, name
        assert a.lifetime() == word
        assert a.admit(a.contract(magic, resident, seconds, startup)) == 0 and a.lifetime() == word
        cases[f'{name}-then-other-lifetime-refused'] = True
    for name, magic, resident in (('resident-magic-supervised-timing', res, False),
                                  ('supervised-magic-resident-timing', sup, True)):
        a = Admission(image)
        assert a.admit(a.contract(magic, resident, seconds, startup)) == diagnostic, name
        assert a.lifetime() == 0 and a.word('os_admitting_lifetime') == 0
        cases[f'{name}-refused-timing'] = True
    a = Admission(image)
    assert a.admit(a.contract(res, True, seconds, startup, fel=0x5aa5a55a)) == fields and a.lifetime() == 0
    cases['resident-fel-flag-refused'] = True
    return seconds, cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, action='append', required=True)
    parser.add_argument('--output', type=Path, required=True)
    a = parser.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    report, total = {}, 0
    for path in a.image:
        seconds, cases = one_image(path.resolve())
        report[str(path)] = {'lifetime_seconds': seconds, 'cases': cases}
        total += len(cases)
        print(f'{path}: {len(cases)} cases, supervised lifetime {seconds} s')
    (a.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS os lifetime admission: {total} cases over {len(a.image)} images; the one OS image admits both lifetimes '
          'by magic with their own terms, writes the lifetime once and derives the result magic and the policy word '
          'from it; a returning image refuses the resident magic')


if __name__ == '__main__':
    main()
