#!/usr/bin/env python3
"""Execute the linked return of a faulted video decoder service (apps/os/video_decode.c) through the production quiesce
registry.

The image's own os_quiesce_register, os_quiesce_step, decoder quiesce step, owner task and decoder platform binding run
under AArch64 emulation, with the owner task run between supervisor passes as its task would be. The binding is
initialized by its own a733_video_platform_init from a configuration at the vendor point; the decoder is then left as
the stage-1 board run 1794712072331160946-bd447f26 left it, refused in hold B (cold step 4) with PLL_VE0 running and
open, the module gate open, and reset and bus gate released. The CCU, RTC, R_CCU, GIC, PCK and decoder pages are
plain registers, and every register access passes the image's own Device-mapping check. The clock and PCK lease
registries answer that the decoder's leases are held. Time advances 10 ms per supervisor pass; the supervisor's
shared bound is 3 s (OS_FAILURE_RETURN_US, apps/os/cpu-return.c), after which the image stops at site 0x742 and the
watchdog reset ends the session.

Cases, each starting from a service that failed in hold B (phase FAULT, step VD_STEP_COLD):
- quiesce-closes: the quiesce asks the owner task, which runs the service's one close in the vendor order: module
  gate, bus gate, reset, then PLL_VE0's output gate, lock enable, enable and LDO. The registry reaches DONE within the
  bound with no fault note, the service is CLOSED with the first fault kept and the record marked fault-closed, and no
  decoder register is touched.
- application-closes: the application's close request (as apps/vdec-check/main.c makes after a failed open) runs the
  same close and is answered -1; the registry then reaches DONE.
- close-step-fails: the CCU bus-gate word does not hold its write. The close fails, the quiesce step reports FAULT
  with site 0x941 once per pass, and the registry never reaches DONE: the session fails at the bound.
- uncertain-access: the binding's failure was an access with an unknown effect. The close touches nothing and the
  registry reports FAULT (site 0x941).
- quiesce-interleaved: the quiesce step runs on another core while the owner task closes. The owner's close is run
  once to record the phase and close_started words after each of its stores. The quiesce step is then run once for
  every pair of those states (i <= j), with its first load of the two words served from state i and its later loads
  from state j, as when the owner stores between the step's loads. No pair may report FAULT: the close succeeds.
The image of the hold-B run (c1a4e4fc...) fails the first two cases: its quiesce step reports FAULT on every pass
until the bound. The image of the stage-1 rerun (7d63776e...) fails quiesce-interleaved: its close stored
close_started before CLOSING and its quiesce step loaded phase first.

Usage: os-vdec-fault-return.py --image ELF --output DIR"""
import argparse
import hashlib
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import (UC_ARM64_REG_PC, UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_X0,
                                 UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3)
from harness import machine as harness_machine, Image, hook_addresses, save, skip_crumbs

GATE_RUNS = [['--image', '{elf:ret-vdec}', '--output', '{output}']]
UNIT = '/apps/os/video_decode.c'
CCU, RTC, R_CCU, GIC, PCK, VE = 0x02002000, 0x07090000, 0x07010000, 0x03400000, 0x07062000, 0x01c0e000
STACKS, SENTINEL, NOTE, SCRATCH = 0x60000000, 0x60100000, 0x60100100, 0x60100200
BOUND_US, PASS_US = 3_000_000, 10_000
QUIESCE = {'PENDING': 0, 'DONE': 1, 'FAULT': 2}
SITE_DECODER = 0x941
VD_OWNER = 0x5645440000000001
OSC_PCK, STABLE, COLD_NS_ROM = 1 | 1 << 3, 1, 1
PLL_INHERITED = 1 << 24
# The board's state after hold B refused (run bd447f26): PLL_VE0 running the vendor profile with its output open,
# the module gate open at mux 1, reset and bus gate released, the MBUS clock gate opened with its key.
HOLD_B = {0x220: 0xf9204d00, 0xa88: 0x81000000, 0xa8c: 0x00040004, 0x5e4: 0x00040306, 0x5c0: 0x1101037f,
          0x5e0: 0xf1045803, 0x1b10: 0x00020000, 0x1918: 0x00030003, 0x1998: 0x00030000}
LAYOUTS = {'board_application', 'a733_board_session', 'a733_video_platform', 'a733_video_decoder', 'a733_video_unit',
           'a733_video_platform_config', 'a733_clock_token', 'a733_gpu_resource_lease', 'a733_power_domain_row',
           'vdec_check_record'}


class Registers:
    """Plain 32-bit registers on one page, with optional words that keep a fixed value."""
    def __init__(self, base, size, words, stuck=None):
        self.base, self.size, self.words, self.stuck, self.writes, self.reads = base, size, dict(words), stuck or {}, [], []

    def read(self, uc, offset, size, data):
        self.reads.append(offset)
        return self.words.get(offset, 0)

    def write(self, uc, offset, size, value, data):
        self.writes.append((offset, value))
        self.words[offset] = self.stuck.get(offset, value)


def members(image):
    """Member offsets of the service's anonymous state structure and its enumerators."""
    layout, enums = None, {}
    with open(image, 'rb') as stream:
        elf = ELFFile(stream)
        for cu in elf.get_dwarf_info().iter_CUs():
            if not cu.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith(UNIT):
                continue
            for die in cu.iter_DIEs():
                if die.tag == 'DW_TAG_variable' and die.attributes.get('DW_AT_name') and \
                        die.attributes['DW_AT_name'].value == b'vd' and 'DW_AT_type' in die.attributes:
                    layout = {c.attributes['DW_AT_name'].value.decode(): c.attributes['DW_AT_data_member_location'].value
                              for c in die.get_DIE_from_attribute('DW_AT_type').iter_children() if c.tag == 'DW_TAG_member'}
                if die.tag == 'DW_TAG_enumerator':
                    enums[die.attributes['DW_AT_name'].value.decode()] = die.attributes['DW_AT_const_value'].value
    return layout, enums


def run_case(image, parts, layout, enums, name, stuck=None, uncertain=False, application_close=False, interleave=False):
    uc = harness_machine()
    low = min(a for a, _, _ in parts) & ~0xfff
    high = (max(a + n for a, _, n in parts) + 0xfff) & ~0xfff
    uc.mem_map(low, high - low)
    for address, data, _ in parts:
        uc.mem_write(address, data)
    uc.mem_map(STACKS, 0x100000)
    uc.mem_map(SENTINEL, 0x1000)
    uc.mem_write(NOTE, (0xd65f03c0).to_bytes(4, 'little'))             # ret
    skip_crumbs(uc, image)
    ccu = Registers(CCU, 0x2000, HOLD_B, stuck)
    others = {'rtc': Registers(RTC, 0x1000, {0x160: 0x983fb0f7}), 'r_ccu': Registers(R_CCU, 0x1000, {0x1ac: 1}),
              'gic': Registers(GIC, 0x1000, {}), 'pck': Registers(PCK, 0x1000, {0: 8, 8: 8}),
              've': Registers(VE, 0x1000, {})}
    ccu.words[0] = 0xf8675f00
    for regs in (ccu, *others.values()):
        uc.mmio_map(regs.base, regs.size, regs.read, None, regs.write, None)
    state = {'time': 1000, 'requested': 0, 'notes': [], 'slice_done': False}
    L = image.layouts
    vd = image.address('vd', UNIT)

    def put(address, value, n=4): uc.mem_write(address, int(value).to_bytes(n, 'little'))
    def get(address, n=4): return int.from_bytes(uc.mem_read(address, n), 'little')
    config = vd + layout['config']
    platform = vd + layout['platform']
    decoder = platform + L['a733_video_platform']['decoder']
    unit = decoder + L['a733_video_decoder']['unit']
    def unit_field(field): return unit + L['a733_video_unit'][field]
    def config_field(field): return config + L['a733_video_platform_config'][field]

    board = image.address('board')
    put(board + L['board_application']['session'] + L['a733_board_session']['deadline_us'], 60_000_000, 8)

    def ret(m, value=None):
        if value is not None:
            m.reg_write(UC_ARM64_REG_X0, value & 0xffffffffffffffff)
        m.reg_write(UC_ARM64_REG_PC, m.reg_read(UC_ARM64_REG_LR))

    def now(m):
        state['time'] += 1
        ret(m, state['time'])

    def counter_now(m):
        state['time'] += 1
        put(m.reg_read(UC_ARM64_REG_X1), state['time'], 8)
        ret(m, 1)

    def pmic_now(m):
        put(m.reg_read(UC_ARM64_REG_X1), state['time'], 8)
        ret(m, 1)

    def translate(m):
        address, out = m.reg_read(UC_ARM64_REG_X0), m.reg_read(UC_ARM64_REG_X2)
        # physical, Device-nGnRnE (MAIR attribute 0), outer shareable (PAR_EL1.SH 2)
        uc.mem_write(out, address.to_bytes(8, 'little') + (0).to_bytes(4, 'little') + (2).to_bytes(4, 'little'))
        ret(m, 1)

    def wait(m):
        state['slice_done'] = True
        m.emu_stop()

    def note(m):
        state['notes'].append((m.reg_read(UC_ARM64_REG_X0), m.reg_read(UC_ARM64_REG_X1)))
        ret(m)

    hooks = {
        'sunstone_deadline_register': lambda m: ret(m, 1),
        'sunstone_deadline_now_us': now,
        'sunstone_deadline_wait_until': wait,
        'ulTaskGenericNotifyTake': wait,  # the owner task's idle block
        'a733_counter_clock_now': counter_now,
        'a733_pmic_now': pmic_now,
        'ss_counter_ticks': lambda m: ret(m, state['time'] * 24),
        'aarch64_device_translate': translate,
        'a733_platform_lock_enter': lambda m: ret(m, 1),
        'a733_platform_lock_leave': lambda m: ret(m),
        'a733_clock_token_status': lambda m: ret(m, 0),       # A733_CLOCK_OK: the decoder's clock lease is held
        'a733_gpu_resources_status': lambda m: ret(m, 0),     # A733_GPU_RESOURCE_OK: its PCK lease is held
        'board_live': lambda m: ret(m, 1),
        'os_cpu_return_requested': lambda m: ret(m, state['requested']),
        'os_cpu_return_failing': lambda m: ret(m, 0),
        'os_application_wake_task': lambda m: ret(m),
    }
    for symbol, callback in hooks.items():
        if symbol in image.symbols:
            hook_addresses(uc, {image.address(symbol)}, lambda m, a, s, u, c=callback: c(m))
    hook_addresses(uc, {NOTE}, lambda m, a, s, u: note(m))
    hook_addresses(uc, {SENTINEL}, lambda m, a, s, u: m.emu_stop())

    def call(function, *args, stack=STACKS + 0x80000):
        for register, value in zip((UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3), args):
            uc.reg_write(register, value)
        uc.reg_write(UC_ARM64_REG_SP, stack)
        uc.reg_write(UC_ARM64_REG_LR, SENTINEL)
        uc.emu_start(function, SENTINEL, count=5_000_000)
        return uc.reg_read(UC_ARM64_REG_X0)

    # The configuration tokens() builds, then the binding's own initialization.
    put(config_field('clocks'), SCRATCH, 8); put(config_field('power'), SCRATCH, 8)
    put(config_field('owner'), VD_OWNER, 8); put(config_field('handoff'), 1, 8)
    put(config_field('module'), 0x81000000); put(config_field('module_hz'), 624_000_000)
    for i, (word, hz) in enumerate(((0, 24_000_000), (0x80000000, 24_000_000), (0x80000000, 24_000_000))):
        put(config_field('fabric') + 4 * i, word); put(config_field('fabric_hz') + 4 * i, hz)
    put(config_field('start'), COLD_NS_ROM)
    row = vd + layout['row']
    put(config_field('row'), row, 8); put(row + L['a733_power_domain_row']['domain'], 2)
    assert call(image.address('a733_video_platform_scopes'), config, SCRATCH + 0x100) == 1, (name, 'scopes refused')
    shared = get(SCRATCH + 0x100, 16)
    call(image.address('a733_video_platform_exclusive'), config)
    exclusive = uc.reg_read(UC_ARM64_REG_X0) | uc.reg_read(UC_ARM64_REG_X1) << 64
    clock, token = config_field('clock'), L['a733_clock_token']
    put(clock + token['id'], 1, 8); put(clock + token['owner'], VD_OWNER, 8)
    put(clock + token['shared'], shared, 16); put(clock + token['exclusive'], exclusive, 16)
    lease, pck_layout = config_field('pck'), L['a733_gpu_resource_lease']
    put(lease + pck_layout['token'], 1, 8); put(lease + pck_layout['mask'], OSC_PCK); put(lease + pck_layout['mode'], STABLE)
    assert call(image.address('a733_video_platform_init'), platform, config, image.address('ops', UNIT), 0) == 1, \
        (name, 'the binding refused its configuration')
    # The decoder as hold B's refusal left it.
    put(unit_field('failed'), 1)
    for field, value in (('pll_inherited', PLL_INHERITED), ('pll_state', 1), ('pll_started', 1), ('pll_locked', 1),
                         ('pll_done', 1)):
        put(unit_field(field), value)
    if uncertain:
        # An uncertain access keeps the register mutex (a733_video_unit_refuse_held); an image before the
        # failure-cause flags has no uncertain member.
        put(unit_field('locked'), 1)
        if 'uncertain' in L['a733_video_unit']:
            put(unit_field('uncertain'), 1)
    for field, value in (('admitted', 1), ('requested', 1), ('cold_step', 4)):
        put(decoder + L['a733_video_decoder'][field], value)
    put(vd + layout['initialized'], 1)
    put(vd + layout['phase'], enums['FAULT'])
    put(vd + layout['fault_step'], enums['VD_STEP_COLD']); put(vd + layout['fault_value'], 0x204)
    if 'info' in layout:
        info = vd + layout['info']
        put(info + L['vdec_check_record']['magic'], 0x49444556)

    task = image.address('os_video_decode_task', UNIT)
    def owner_slice():
        state['slice_done'] = False
        call(task, 0, stack=STACKS + 0x40000)
        assert state['slice_done'], (name, 'the owner task did not reach its wait')

    close_answer = None
    words = {vd + layout['phase']: 'phase', vd + layout['close_started']: 'close_started'}
    states, interleavings = [], []
    if interleave:
        # The owner's stores to the two words, each state as the store leaves it.
        states.append({k: get(a) for a, k in words.items()})
        def stored(m, access, address, size, value, data):
            if address in words:
                states.append({**states[-1], words[address]: value & 0xffffffff})
        writes = uc.hook_add(UC_HOOK_MEM_WRITE, stored, begin=min(words), end=max(words) + 3)
    if application_close or interleave:
        put(vd + layout['op'], enums['CLOSE_OP']); put(vd + layout['request'], enums['POSTED'])
        owner_slice()
        if get(vd + layout['request']) == enums['DONE']:
            close_answer = get(vd + layout['result']) - (1 << 32) if get(vd + layout['result']) >> 31 else \
                get(vd + layout['result'])
    quiesce = image.address('video_decode_quiesce', UNIT)
    if interleave:
        uc.hook_del(writes)
        after = {a: get(a) for a in words}
        for i in range(len(states)):
            for j in range(i, len(states)):
                loads = []
                def serve(m, access, address, size, value, data, i=i, j=j):
                    if address in words:
                        # A read hook runs before the load: the word it loads is the one written here.
                        put(address, states[i if not loads else j][words[address]])
                        loads.append(words[address])
                reads = uc.hook_add(UC_HOOK_MEM_READ, serve, begin=min(words), end=max(words) + 3)
                result = call(image.address('quiesce_step', UNIT), quiesce) & 0xffffffff
                uc.hook_del(reads)
                for a, v in after.items():
                    put(a, v)
                interleavings.append({'first': states[i], 'later': states[j], 'loads': loads, 'result': result})
        state['notes'].clear()
    assert call(image.address('os_quiesce_register'), quiesce) == 1, (name, 'registration refused')
    state['requested'] = 1
    start, results = state['time'], []
    while state['time'] - start < BOUND_US:
        result = call(image.address('os_quiesce_step'), NOTE) & 0xffffffff
        results.append(result)
        if result == QUIESCE['DONE']:
            break
        owner_slice()
        state['time'] += PASS_US
    record = {}
    if 'info' in layout and 'fault_closed' in L['vdec_check_record']:
        record = {k: get(info + L['vdec_check_record'][k]) for k in ('fault_closed', 'fault_step', 'failed')}
    return {
        'case': name, 'passes': len(results), 'last_result': results[-1],
        'done_within_bound': results[-1] == QUIESCE['DONE'], 'elapsed_us': state['time'] - start,
        'phase': get(vd + layout['phase']), 'recorded_fault_step': get(vd + layout['fault_step']),
        'close_answer': close_answer, 'record': record, 'store_states': states, 'interleavings': interleavings,
        'notes': [(hex(s), hex(v)) for s, v in state['notes']],
        'ccu': {hex(k): hex(ccu.words.get(k, 0)) for k in (0x220, 0xa88, 0xa8c, 0x5e4)},
        'ccu_writes': [(hex(a), hex(v)) for a, v in ccu.writes],
        've_accesses': len(others['ve'].reads) + len(others['ve'].writes),
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    image = Image(a.image, LAYOUTS)
    layout, enums = members(a.image)
    with a.image.open('rb') as stream:
        parts = [(int(s['p_vaddr']), bytes(s.data()), int(s['p_memsz']))
                 for s in ELFFile(stream).iter_segments() if s['p_type'] == 'PT_LOAD']
    assert layout and enums['CLOSED'] == 0 and enums['FAULT'] == 4
    cases = [('quiesce-closes', {}), ('application-closes', {'application_close': True}),
             ('close-step-fails', {'stuck': {0xa8c: 0x00040004}}), ('uncertain-access', {'uncertain': True}),
             ('quiesce-interleaved', {'interleave': True})]
    rows, failures = [], []
    for name, options in cases:
        row = run_case(image, parts, layout, enums, name, **options)
        problems = []
        if name in ('close-step-fails', 'uncertain-access'):
            if row['done_within_bound'] or row['last_result'] != QUIESCE['FAULT']:
                problems.append('a failed or refused close must leave the service FAULT')
            if not row['notes'] or any(int(s, 16) & 0xfff != SITE_DECODER for s, _ in row['notes']):
                problems.append('fault notes with site 0x941 expected')
            if name == 'uncertain-access' and row['ccu_writes']:
                problems.append('an uncertain failure must not be touched')
        else:
            if not row['done_within_bound']:
                problems.append(f'the registry did not reach DONE within {BOUND_US} us: the return stops at 0x742')
            if row['notes']:
                problems.append('a closed service must not publish a fault note')
            if row['phase'] != enums['CLOSED'] or row['recorded_fault_step'] != enums['VD_STEP_COLD']:
                problems.append('the service must be CLOSED with its first fault kept')
            ccu = {int(k, 16): int(v, 16) for k, v in row['ccu'].items()}
            if ccu[0xa88] & 0x80000000 or ccu[0xa8c] & 0x40004 or ccu[0x220] != 0x01204d00 or ccu[0x5e4] != 0x40306:
                problems.append('module gate, bus gate and reset must be closed and PLL_VE0 disabled to 0x01204d00')
            if row['record'].get('fault_closed') != 1:
                problems.append('the record must say the fault was closed')
            if name == 'application-closes' and row['close_answer'] != -1:
                problems.append('the application close of a faulted service must be answered -1')
            if name == 'quiesce-interleaved':
                if len(row['store_states']) < 3 or not row['interleavings']:
                    problems.append('the close must store phase and close_started')
                faults = [x for x in row['interleavings'] if x['result'] == QUIESCE['FAULT']]
                if faults:
                    problems.append(f'the quiesce reported FAULT while a close that succeeds ran: {faults[0]}')
        if row['ve_accesses']:
            problems.append('the close must not touch a decoder register')
        row['passed'] = not problems
        row['problems'] = problems
        if problems:
            failures.append(name)
        rows.append(row)
    root = Path(__file__).resolve().parents[1]
    save(a.output / 'result.json', {
        'passed': not failures, 'physical_access': False, 'image': str(a.image),
        'image_sha256': hashlib.sha256(a.image.read_bytes()).hexdigest(), 'cases': rows,
        'sources_sha256': {n: hashlib.sha256((root / n).read_bytes()).hexdigest()
                           for n in ('apps/os/video_decode.c', 'apps/os/quiesce.c', 'drivers/video/a733_video_platform.c',
                                     'drivers/video/a733_video_unit.c', 'checks/os-vdec-fault-return.py')}})
    for row in rows:
        print('PASS' if row['passed'] else 'FAIL', row['case'], f"{row['passes']} passes, {row['elapsed_us']} us",
              '; '.join(row['problems']))
    if failures:
        raise SystemExit(f'{len(failures)} of {len(rows)} cases failed')
    print('PASS faulted video decoder service: closes and returns within the bound; a failed or uncertain close stays '
          'FAULT')


if __name__ == '__main__':
    main()
