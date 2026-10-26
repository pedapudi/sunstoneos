#!/usr/bin/env python3
"""Execute the stage-2 entry path with modeled entry prerequisites.

The linked AArch64 monitor runs as in checks/standalone-boot-loader.py: the
actual boot0 header cleanup, cold-entry gate, stage-2 loader, cold SD
admission against the modeled boot0 controller state, SD core and SHA-256,
against a card image written by tools/boot-slots.py. The slot holds an image,
the integrated OS session page and an asset. The four entry prerequisites
are modeled as granted unless a case refuses one. WDT0, the ROM redirect and
RTC FEL registers and CCU word 0x24 are modeled. The check confirms the
watchdog arming sequence, the resident contract, the stamped session and the
branch to the image entry, then has the integrated OS image's own admission
(checks/os-image-model.c, resident build) accept that contract and session at
the entry counter value. It also compiles that model for the resident
microphone build and enters a slot with the 992-byte session.
"""
import argparse
import contextlib
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn import arm64_const as a64

if not __debug__:
    raise SystemExit('checks require assertions; do not pass -O')
ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


loader = module('standalone_boot_loader', 'checks/standalone-boot-loader.py')
slots, cold, Card = loader.slots, loader.cold, loader.Card
CARD_SECTORS, RAM, RAM_END, SDC0, RVBAR = (loader.CARD_SECTORS, loader.RAM, loader.RAM_END,
                                           loader.SDC0, loader.RVBAR)
HZ = 24000000
SESSION, CONTRACT, RESULT = 0x41f00000, 0x42004000, 0x42000000
SESSION_BYTES, AUDIO_SESSION_BYTES = 808, 992
PREREQUISITES = ('a733_stage2_remote_writers_excluded', 'a733_stage2_reset_policy',
                 'a733_stage2_entry_state', 'a733_stage2_cpu_cache_owned')
CONTRACT_MAGIC, RESULT_MAGIC = 0x415337334f524331, 0x415337334f525231
STATUS = {1: 'loaded', 6: 'entered', 7: 'entry-refused', 8: 'watchdog', 9: 'remote-writers', 10: 'cpu-cache'}
ARM_WRITES = [(0x02050018, 0x16aa0002), (0x02050010, 0x16aa0001),
              (0x02050014, 0x16aa0041), (0x0205000c, 0x14af)]


class Registers:
    """WDT0 at 0x02050000, CCU word 0x03000024, R_CPUCFG redirect words and the RTC FEL flag."""

    def __init__(self, preset, stuck=()):
        self.values = dict(preset)
        self.stuck = stuck
        self.writes = []

    def page(self, base):
        def read(uc, offset, size, user):
            assert size == 4, hex(base + offset)
            return self.values.get(base + offset, 0)

        def write(uc, offset, size, value, user):
            assert size == 4 and base == 0x02050000, hex(base + offset)
            address = base + offset
            self.writes.append((address, value))
            if offset in (0x10, 0x14, 0x18):
                assert value >> 16 == 0x16aa, hex(value)
                if address not in self.stuck:
                    self.values[address] = value & 0xffff
            else:
                assert offset == 0x0c and value == 0x14af, (hex(offset), hex(value))
        return read, write


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True, help='CMake build directory with a733-stage2')
    parser.add_argument('--boot0', type=Path, required=True)
    parser.add_argument('--policy-only', action='store_true', help='only changed normal-card reset and early refusal cases')
    parser.add_argument('--model', type=Path, required=True,
                        help='checks/os-image-model.c built for the resident lifetime')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    # The same model for the resident microphone build, whose session is 992 bytes.
    audio_model = args.output / 'os-image-model-resident-audio'
    if not args.policy_only:
        subprocess.run(['clang', '-std=c11', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-Iinclude', '-Iplatform/a733-smp-cached', '-Iapps/os', '-DSUNSTONE_BOARD_SUPERVISED=1',
                        '-DSUNSTONE_BOARD_USB2_DUAL=1', '-DSUNSTONE_BOARD_RESIDENT=1', '-DSUNSTONE_A733_OS_AUDIO=1',
                        '-DA733_SMP_CONTRACT_MAGIC=%#xULL' % CONTRACT_MAGIC, 'checks/os-image-model.c',
                        'drivers/storage/storage_digest.c', '-o', str(audio_model)], cwd=ROOT, check=True)
    raw = (args.build / 'a733-stage2.bin').read_bytes()
    table = loader.symbols(args.build / 'a733-stage2.elf')
    inherited, _ = cold.inherited_payload(args.boot0.read_bytes(), raw, args.output)
    disassembler = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    system = {}
    for offset in range(0, len(raw) - 3, 4):
        instruction = next(disassembler.disasm(raw[offset:offset + 4], 0x47000000 + offset), None)
        if instruction and instruction.mnemonic in ('mrs', 'msr', 'wfi', 'wfe'):
            system[instruction.address] = (instruction.mnemonic, instruction.op_str.lower())

    work = args.output / 'cards'
    work.mkdir()

    def tool(argv):
        with (work / 'boot-slots.log').open('a') as log, contextlib.redirect_stdout(log):
            slots.main(argv)

    empty = work / 'empty.img'
    with empty.open('xb') as stream:
        stream.truncate(CARD_SECTORS * 512)
    allocation = work / 'allocation.json'
    tool(['allocate', '--backup', str(empty), '--slot-a', '40960:8192', '--slot-b', '49152:8192',
          '--output', str(allocation)])
    image = bytes((i * 29 + 7) & 255 for i in range(6000))
    (work / 'image').write_bytes(image)
    (work / 'asset').write_bytes(bytes((i * 13 + 5) & 255 for i in range(3000)))

    def card(name, generation=1, session=True, model=args.model, change=None):
        """A card with slot A; the session names `generation`; the first plan is generation 1."""
        subprocess.run([str(model), '--write-session', str(work / (name + '.session')), str(generation)],
                       check=True)
        if change:
            path = work / (name + '.session')
            path.write_bytes(change(bytearray(path.read_bytes())))
        assets = ['--asset', '%s@0x44000000' % (work / 'asset')]
        if session:
            assets = ['--asset', '%s@%#x' % (work / (name + '.session'), SESSION)] + assets
        tool(['plan', '--allocation', str(allocation), '--card-image', str(empty),
              '--image', '%s@0x41000000' % (work / 'image'), '--entry', '0x41000000', *assets,
              '--output', str(work / (name + '-plan'))])
        plan = json.loads((work / (name + '-plan') / 'plan.json').read_text())
        assert plan['generation'] == 1 and plan['slot'] == 'A'
        target = work / (name + '.img')
        shutil.copyfile(empty, target)
        tool(['apply', '--plan', str(work / (name + '-plan') / 'plan.json'), '--card-image', str(target)])
        data = bytearray(target.read_bytes())
        target.unlink()
        return data, (work / (name + '.session')).read_bytes()

    cases = []

    def run(name, card_bytes, expected, granted=PREREQUISITES, preset=None, retained=None, stuck=(),
            model=args.model, session_bytes=SESSION_BYTES):
        registers = Registers(preset or {}, stuck)
        model_card = Card(card_bytes)
        board = loader.Board(model_card, lambda: values['cntpct_el0'])
        cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        cpu.mem_map(RAM, RAM_END - RAM)
        cpu.mem_map(RVBAR, 0x1000)
        cpu.mmio_map(SDC0, 0x1000, model_card.read, None, model_card.write, None)
        for base in (0x02050000, 0x03000000, 0x07050000, 0x07090000):
            read, write = registers.page(base)
            cpu.mmio_map(base, 0x1000, read, None, write, None)
        for base in (loader.PIO, loader.CCU):
            read, write = board.page(base)
            cpu.mmio_map(base, 0x1000, read, None, write, None)
        cpu.mem_write(0x47000000, inherited)
        if retained:
            cpu.mem_write(RESULT, retained)
        values = {'currentel': 12, 'mpidr_el1': 0x81000000, 'midr_el1': 0x412fd050,
                  'sctlr_el3': 0x30c50832, 'scr_el3': 0x30, 'cntfrq_el0': 0,
                  'cntpct_el0': 24000123, 'rvbar_el3': 0x47000000, 'esr_el3': 0,
                  'far_el3': 0, 'elr_el3': table['a733_stage2_main'], 'spsr_el3': 0x3cd}
        controls, stops, entered, foreign = [], [], [], []

        def instruction(u, address, size, context):
            mnemonic, operands = system[address]
            if mnemonic in ('wfi', 'wfe'):
                stops.append(mnemonic)
                u.emu_stop()
                return
            first, second = operands.split(', ')
            if mnemonic == 'mrs':
                value = values[second]
                if second == 'cntpct_el0':
                    values['cntpct_el0'] += 2400
                u.reg_write(getattr(a64, 'UC_ARM64_REG_' + first.upper()), value)
            else:
                assert first in ('daifset', 'vbar_el3', 'rmr_el3'), first
                controls.append([first, second if second.startswith('#') else
                                 u.reg_read(getattr(a64, 'UC_ARM64_REG_' + second.upper()))])
            u.reg_write(a64.UC_ARM64_REG_PC, address + 4)

        def grant(u, address, size, context):
            u.reg_write(a64.UC_ARM64_REG_X0, int(context in granted))
            u.reg_write(a64.UC_ARM64_REG_PC, u.reg_read(a64.UC_ARM64_REG_LR))

        def image_entry(u, address, size, context):
            entered.append(address)
            u.emu_stop()

        # Outside its own storage the loader may write only the RVBAR words,
        # the SD controller, the load windows, the contract and the retained
        # result header. Register pages are checked by their own models.
        def write(u, access, address, size, value, context):
            end = address + size
            if not (0x47008000 <= address and end <= table['a733_stage2_stack_top']
                    or RVBAR + 4 <= address and end <= RVBAR + 12
                    or SDC0 <= address and end <= SDC0 + 0x1000
                    or address in (loader.SMHC0_CLK, loader.PF_CFG0, loader.PF_PUL0) and size == 4
                    or 0x02050000 <= address and end <= 0x02051000
                    or 0x41000000 <= address and end <= 0x41000000 + len(image)
                    or SESSION <= address and end <= SESSION + session_bytes
                    or 0x44000000 <= address and end <= 0x44000000 + 3000
                    or CONTRACT <= address and end <= CONTRACT + 224
                    or RESULT <= address and end <= RESULT + 8):
                foreign.append(hex(address))

        for address in system:
            cpu.hook_add(UC_HOOK_CODE, instruction, begin=address, end=address)
        for symbol in PREREQUISITES:
            cpu.hook_add(UC_HOOK_CODE, grant, user_data=symbol, begin=table[symbol], end=table[symbol])
        cpu.hook_add(UC_HOOK_CODE, image_entry, begin=RAM, end=0x46ffffff)
        cpu.hook_add(UC_HOOK_MEM_WRITE, write)
        cpu.emu_start(0x47000000, 0, count=60000000)
        assert not foreign, (name, foreign[:4])
        assert not any(c[0] == 'sctlr_el3' for c in controls)
        report = bytes(cpu.mem_read(0x47008200, 512))
        fields = struct.unpack_from('<16Q', report)
        status, detail = fields[3], fields[4]
        result = {'case': name, 'status': STATUS.get(status, status), 'detail': detail,
                  'granted': list(granted), 'session_bytes': session_bytes,
                  'watchdog_writes': [[hex(a), hex(v)] for a, v in registers.writes]}
        assert fields[0] == loader.REPORT_MAGIC and STATUS.get(status) == expected, (name, result)
        if expected in ('remote-writers', 'cpu-cache'):
            assert not board.writes and not board.detect_reads, name
        else:
            assert [w[:2] for w in board.writes] == loader.SELECTED and board.detect_reads, name
        contract = bytes(cpu.mem_read(CONTRACT, 224))
        if expected == 'entered':
            assert entered == [0x41000000] and not stops and controls[-1] == ['daifset', '#0xf']
            assert registers.writes == ARM_WRITES and detail == 0x41000000
            armed, contract_address = struct.unpack_from('<2Q', report, 112)
            assert contract_address == CONTRACT
            session = bytes(cpu.mem_read(SESSION, session_bytes))
            magic, version, size, epoch, generation, inverse = struct.unpack_from('<6Q', contract)
            build_id = contract[48:80]
            times = struct.unpack_from('<4Q', contract, 80)
            lease, mode, configuration, output, fel, redirect, redirect_entry, alternate = \
                struct.unpack_from('<8Q', contract, 112)
            reserved = struct.unpack_from('<6Q', contract, 176)
            assert (magic, version, size, generation, inverse) == (CONTRACT_MAGIC, 2, 224, 1, (~1) & (2**64 - 1))
            assert epoch == armed and struct.unpack_from('<Q', session, 24)[0] == armed
            assert build_id == session[48:80] and lease == struct.unpack_from('<Q', session, 40)[0]
            assert times == (armed, armed + HZ, 2**64 - 1, armed + 4 * HZ)
            assert (mode, configuration, output, fel, redirect, redirect_entry, alternate) == (0x41, 1, 2, 0, 0, 0, 0)
            assert reserved[:2] == (SESSION, session_bytes)
            assert contract[192:224] == hashlib.sha256(session).digest()
            (args.output / (name + '.contract')).write_bytes(contract)
            (args.output / (name + '.session')).write_bytes(session)
            # The image's own resident admission, at the counter value the
            # loader last read: earlier than any instruction the image runs.
            admit = subprocess.run([str(model), '--admit', str(args.output / (name + '.contract')),
                                    str(args.output / (name + '.session')), str(values['cntpct_el0'])],
                                   capture_output=True, text=True)
            assert admit.returncode == 0 and admit.stdout.strip() == 'admitted', (name, admit.stdout, admit.stderr)
            moved = report[192:320]
            result.update({'armed_at': armed, 'admitted_by_image': True,
                           'retained_result_moved': any(moved)})
            if retained:
                assert moved == retained and bytes(cpu.mem_read(RESULT, 8)) == bytes(8)
            else:
                assert not any(moved)
        else:
            assert stops == ['wfi'] and not entered and controls[-1] == ['rmr_el3', 2]
            assert struct.unpack_from('<Q', contract)[0] != CONTRACT_MAGIC
            assert not any(report[112:128])
            if stuck:
                # The mode write did not read back: WDT0 may run, and the
                # detail is the nonzero arming counter value.
                assert registers.writes == ARM_WRITES and 24000123 < detail < values['cntpct_el0']
            else:
                assert not registers.writes and (expected != 'watchdog' or detail == 0)
        cases.append(result)
        return result

    nominal, _ = card('nominal')
    if args.policy_only:
        run('card-reset-high-boot-input', nominal, 'entered', preset={0x03000024: 0x100})
        run('card-reset-low-boot-input', nominal, 'entered')
        run('remote-refused-before-card', nominal, 'remote-writers', granted=PREREQUISITES[1:])
        run('reset-policy-refused', nominal, 'loaded', granted=tuple(p for p in PREREQUISITES if p != 'a733_stage2_reset_policy'))
        run('entry-state-refused', nominal, 'loaded', granted=tuple(p for p in PREREQUISITES if p != 'a733_stage2_entry_state'))
        run('cpu-cache-refused-before-card', nominal, 'cpu-cache', granted=PREREQUISITES[:3])
        run('watchdog-already-enabled', nominal, 'watchdog', preset={0x02050014: 1})
        run('rom-redirect-present', nominal, 'watchdog', preset={0x070501c0: 0xfa50392f})
        run('mode-not-read-back', nominal, 'watchdog', stuck=(0x02050014,))
        (args.output / 'result.json').write_text(json.dumps({'passed': True, 'physical_access': False,
            'physical_boot_qualified': False, 'monitor_sha256': hashlib.sha256(raw).hexdigest(), 'cases': cases,
            'limitations': ['Prerequisites and hardware effects are modeled; actual arming and contract code execute.']}, indent=2) + '\n')
        print('PASS selected stage-2 normal-card reset policy:', len(cases), 'instruction-model cases')
        return
    previous = struct.pack('<16Q', RESULT_MAGIC, 77, 1, 5, 3, 0x30a, 1000, 0, 2000, *([0] * 7))
    run('entered', nominal, 'entered')
    run('entered-after-retained-result', nominal, 'entered', retained=previous)
    # Entry needs every prerequisite; each one alone refuses it.
    run('prerequisites-refused', nominal, 'remote-writers', granted=())
    for refused in PREREQUISITES:
        run(refused[len('a733_stage2_'):].replace('_', '-') + '-refused', nominal,
            'remote-writers' if refused == PREREQUISITES[0] else 'cpu-cache' if refused == PREREQUISITES[3] else 'loaded',
            granted=tuple(p for p in PREREQUISITES if p != refused))
    audio, _ = card('audio', model=audio_model)
    run('entered-audio-session', audio, 'entered', model=audio_model, session_bytes=AUDIO_SESSION_BYTES)
    no_session, _ = card('no-session', session=False)
    assert run('no-session-extent', no_session, 'entry-refused')['detail'] == 1
    # A session extent of another size, here the 808 bytes and 92 more.
    longer, _ = card('longer-session', change=lambda b: b + bytes(92))
    assert run('session-size-other', longer, 'entry-refused', session_bytes=900)['detail'] == 1
    # The OS size field must equal the extent: an 808-byte session claiming 992.
    claims, _ = card('size-field-differs', change=lambda b: b[:608] + struct.pack('<Q', 992) + b[616:])
    assert run('session-size-field-differs', claims, 'entry-refused')['detail'] == 2
    other, _ = card('other-generation', generation=2)
    assert run('session-generation-differs', other, 'entry-refused')['detail'] == 2
    run('watchdog-already-enabled', nominal, 'watchdog', preset={0x02050014: 1})
    run('rom-redirect-present', nominal, 'watchdog', preset={0x070501c0: 0xfa50392f})
    run('rtc-fel-flag-present', nominal, 'watchdog', preset={0x07090108: 0x5aa5a55a})
    run('mode-not-read-back', nominal, 'watchdog', stuck=(0x02050014,))
    result = {'passed': True, 'physical_access': False, 'physical_boot_qualified': False,
              'monitor_sha256': hashlib.sha256(raw).hexdigest(), 'cases': cases,
              'limitations': [
                  'The entry prerequisites are modeled as granted unless a case refuses one. Native remote exclusion remains unproven. Card admission runs against the modeled boot0 controller state.',
                  'Watchdog, redirect, RTC and CCU registers are modeled; no reset, expiry or ROM behavior is exercised.',
                  'The branch target is observed; the image itself is not executed here.']}
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS stage-2 resident entry:', len(cases), 'instruction-model cases')


if __name__ == '__main__':
    main()
