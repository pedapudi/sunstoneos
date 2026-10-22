#!/usr/bin/env python3
"""Check the linked integrated OS image's EL3 publication hook for its workloads.

Only board_identity_prepare (apps/os/identity.c) executes, under unicorn. The
exception level, cache state, session validation, cache publication, the
headset profile rules and the four SID words are modeled; shared CPU startup
does not run. The build's link check names its options:

- every build publishes the three recognition inputs, and a second call
  publishes nothing;
- the base and microphone builds admit workloads 0 and 4 only;
- the radio build admits workload 1 only, publishes the radio profile and
  firmware span, reads the four chip-ID words and publishes them with the
  session's SID authority;
- the headset build admits workloads 0 and 4, copies the 832-byte Bluetooth
  profile from 0x43000000 and publishes the copy, the profile span and the
  chip-ID record; it refuses another profile size and a profile that the
  headset rules refuse;
- the build with both admits workload 1 and copies the Bluetooth profile
  from 0x43008000, after the radio profile, with the headset build's
  publications and refusals; the 1 MiB span covers both profiles.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as reg

SESSION = 0x50001000
WORKLOAD, SID_AUTHORITY, PROFILE_ADDRESS, PROFILE_BYTES = 400, 312, 352, 360
SID_WORDS = (0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00)
DEADLINE = 100000


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True, help='configured and built OS build directory')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    image = args.build / 'sunstone-a733-os.elf'
    config = json.loads((args.build / 'os-image/link-check.json').read_text())
    radio, headset = config.get('radio', False), config.get('headset', False)
    text = (args.build / 'mimus-board-application/assets.h').read_text()
    addresses = [int(v, 0) for v in re.search(r'mimus_asset_address\[3\]=\{([^}]*)\}', text).group(1).split(',')]
    sizes = [int(v, 0) for v in re.search(r'mimus_asset_bytes\[3\]=\{([^}]*)\}', text).group(1).split(',')]
    assets = [(a, n, DEADLINE) for a, n in zip(addresses, sizes)]
    with image.open('rb') as source:
        elf = ELFFile(source)
        segments = [(s['p_vaddr'], s.data()) for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
        symbols = {}
        for s in elf.get_section_by_name('.symtab').iter_symbols():
            if s['st_value'] and s['st_info']['type'] == 'STT_FUNC':
                symbols.setdefault(s.name, []).append((s['st_value'], s['st_size']))

    def symbol(name):
        assert len(symbols.get(name, [])) == 1, name
        return symbols[name][0][0]
    entry = symbol('board_identity_prepare')
    length = symbols['board_identity_prepare'][0][1]
    hooks = {symbol('board_session_snapshot'): 'snapshot', symbol('a733_smp_counter'): 'counter',
             symbol('a733_board_session_valid'): 'valid', symbol('a733_cpu_coherence_publish'): 'publish'}
    if headset:
        hooks[symbol('os_headset_profile_valid')] = 'profile'

    def run(workload, profile_bytes=832, profile_valid=1, calls_made=1):
        cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        cpu.mem_map(0x41000000, 0x7000000)
        cpu.mem_map(0x50000000, 0x100000)
        cpu.mem_map(0x03006000, 0x1000)
        for address, data in segments:
            cpu.mem_write(address, data)
        session = bytearray(592)
        struct.pack_into('<I', session, WORKLOAD, workload)
        struct.pack_into('<Q', session, SID_AUTHORITY, 0x5151)
        if radio:
            struct.pack_into('<2Q', session, PROFILE_ADDRESS, 0x43000000, 1872)
        cpu.mem_write(SESSION, bytes(session))
        cpu.mem_write(0x03006200, struct.pack('<4I', *SID_WORDS))
        profile = bytes((i * 7 + 3) & 255 for i in range(832))
        profile = profile[:12] + struct.pack('<I', profile_bytes) + profile[16:]
        cpu.mem_write(0x43008000 if radio and headset else 0x43000000, profile)
        reads = {}
        for insn in Cs(CS_ARCH_ARM64, CS_MODE_ARM).disasm(bytes(cpu.mem_read(entry, length)), entry):
            if insn.mnemonic == 'mrs':
                target, name = insn.op_str.lower().split(', ')
                assert name in ('currentel', 'sctlr_el3'), name
                reads[insn.address] = (getattr(reg, 'UC_ARM64_REG_' + target.upper()), 12 if name == 'currentel' else 0)
        calls, returned, checked = [], [], []

        def ret(value):
            cpu.reg_write(reg.UC_ARM64_REG_X0, value)
            cpu.reg_write(reg.UC_ARM64_REG_PC, cpu.reg_read(reg.UC_ARM64_REG_LR))

        def hook(machine, address, size, unused):
            if address == 0x500ff000:
                returned.append(cpu.reg_read(reg.UC_ARM64_REG_X0))
                if len(returned) < calls_made:
                    cpu.reg_write(reg.UC_ARM64_REG_X0, DEADLINE)
                    cpu.reg_write(reg.UC_ARM64_REG_PC, entry)
                else:
                    cpu.emu_stop()
            elif address in reads:
                target, value = reads[address]
                cpu.reg_write(target, value)
                cpu.reg_write(reg.UC_ARM64_REG_PC, address + 4)
            elif address in hooks:
                kind = hooks[address]
                if kind == 'snapshot':
                    ret(SESSION)
                elif kind == 'counter':
                    ret(1000)
                elif kind == 'valid':
                    ret(1)
                elif kind == 'profile':
                    copy = cpu.reg_read(reg.UC_ARM64_REG_X1)
                    checked.append(bytes(cpu.mem_read(copy, 832)) == profile and cpu.reg_read(reg.UC_ARM64_REG_X0) == SESSION)
                    ret(profile_valid)
                else:
                    calls.append(tuple(cpu.reg_read(reg.UC_ARM64_REG_X0 + n) for n in range(3)))
                    ret(1)
        cpu.hook_add(UC_HOOK_CODE, hook)
        cpu.reg_write(reg.UC_ARM64_REG_X0, DEADLINE)
        cpu.reg_write(reg.UC_ARM64_REG_SP, 0x500f0000)
        cpu.reg_write(reg.UC_ARM64_REG_LR, 0x500ff000)
        cpu.emu_start(entry, 0, count=1000000)
        return cpu, calls, returned, checked, profile

    def record(cpu, calls):
        """The published chip-ID record: the session's SID authority and the four words."""
        address, size, _ = calls[-1]
        assert size == 24, size
        authority, *words = struct.unpack('<Q4I', bytes(cpu.mem_read(address, 24)))
        assert authority == 0x5151 and tuple(words) == SID_WORDS, (authority, words)

    cases = []
    admitted = (1,) if radio else (0, 4)
    for workload in (0, 1, 2, 3, 4, 6, 8):
        cpu, calls, returned, checked, profile = run(workload)
        accepted = workload in admitted
        assert returned == [int(accepted)], (workload, returned)
        if not accepted:
            assert calls == [] and not checked, (workload, calls)
        elif headset:
            assert checked == [True] and len(calls) == 6, (checked, calls)
            copy = calls[3][0]
            assert calls[:3] == assets and calls[3][1:] == (832, DEADLINE) and calls[4] == (0x43000000, 0x100000, DEADLINE)
            assert bytes(cpu.mem_read(copy, 832)) == profile
            record(cpu, calls)
        elif radio:
            assert calls[:5] == assets + [(0x43000000, 1872, DEADLINE), (0x43010000, 0xf0000, DEADLINE)], calls
            assert len(calls) == 6, calls
            record(cpu, calls)
        else:
            assert calls == assets, calls
        cases.append({'workload': workload, 'accepted': accepted, 'publications': len(calls)})
    workload = admitted[0]
    _, calls, returned, _, _ = run(workload, calls_made=2)
    assert returned[0] == 1 and returned[1] == 0 and len(calls) == len(assets) + (3 if radio or headset else 0), returned
    cases.append({'workload': workload, 'second-call-refused': True})
    if headset:
        for name, change in (('profile-size-752', {'profile_bytes': 752}), ('profile-refused', {'profile_valid': 0})):
            _, calls, returned, _, _ = run(admitted[0], **change)
            assert returned == [0] and calls == assets, (name, returned, calls)
            cases.append({'workload': 4, name: 'refused'})
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({'passed': True, 'image_sha256': hashlib.sha256(image.read_bytes()).hexdigest(),
                                       'radio': radio, 'headset': headset, 'cases': cases, 'physical_access': False},
                                      indent=2) + '\n')
    print('PASS OS identity hook (%s): workloads %s admitted, others refused, %d cases' %
          ('radio and headset' if radio and headset else 'radio' if radio else 'headset' if headset else 'base',
           list(admitted), len(cases)))


if __name__ == '__main__':
    main()
