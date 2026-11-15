#!/usr/bin/env python3
"""Drive the linked G2D open (apps/os/g2d.c) through its supply wait and its row wait, and refuse two compiled mutants.

The owner task's serve, do_open and tokens run as linked in the OS G2D check image under AArch64 emulation. The
service's rail, clock and PCK600 leases are already held, so tokens goes straight to the supply wait. The power-domain
owner is not linked in: the check sets the fields that the two deadline functions read (include/a733_power_domains.h)
as apps/board/main.c sets them (the owner's tick, 1,000 us; the provider's startup deadline, the session deadline;
the provider's clock), and it plays the owner by setting the authority word's DCDC2-verified bit and by answering the
domain-1 row at scheduled times. Every clock reads one model time, which advances only in the service's deadline
waits; a wait ends at its target or at the next scheduled owner event, whichever comes first. The entry registers
hold words that g2d.c's entry checks accept (PERI0 enabled and locked, its 300M gates open, the engine gated and in
reset, INTID 83 quiet); they are not a board measurement. The open is stopped at its first CCU write, the first step
after the row's ON answer.

Bounds, with T the time the open starts and limit the open's deadline, min(T + 500,000 us, the session end - 10 ms):
- the supply wait ends at limit: the startup deadline the supply deadline is anchored at is the session deadline,
  which lies past limit;
- the row wait ends at min(request time + 10,000 + 2,000 us, limit).
No register is written before the row's ON answer: a refusal and a row timeout leave every modeled register as the
open found it.

Cases:
- verified-before: DCDC2 is verified before the open. The open makes no supply wait, requests the row at T, and the
  owner answers ON 600 us later. Its one row wait targets the row deadline, and the record carries the owner's
  transition record.
- verified-during: DCDC2 is verified at T + 8,000 us, during the supply wait, which targets limit. The row is
  requested at the verification, reads PENDING, and is answered ON.
- supply-deadline-passes: DCDC2 is never verified. The one wait targets the supply deadline, T + 500,000 us, and the
  open is refused at it with G2D_STEP_SUPPLY, the service CLOSED and no row request.
- supply-deadline-clipped: as supply-deadline-passes, with the session ending 60 ms after T: the wait targets the
  open's deadline, T + 50,000 us, and the open is refused there.
- clock-read-fails: DCDC2 is not verified and the provider's clock read fails, so the supply deadline is 0: the open is
  refused at G2D_STEP_SUPPLY with no wait and no row request.
- row-answer-after-supply: DCDC2 is verified at T + 3,000 us and the owner answers the row 11,000 us after the request,
  inside the 12,000 us row deadline. The open proceeds.
- row-timeout: DCDC2 is verified before the open and the row is never answered. The row wait targets the row deadline
  and the open fails there with G2D_STEP_ROW_TIMEOUT, the service FAULT.
- row-deadline-clipped: as row-timeout, with the session ending 18 ms after T: the row wait targets the open's
  deadline, T + 8,000 us, not T + 12,000 us, and the open fails there.

Mutants, each g2d.c compiled with one text changed and linked with the image's own link command into the output
directory (the build directory is only read):
- the supply wait bounded by the former fixed G2D_CALL_US (500 ms) from now, unclipped and blind to the clock;
  supply-deadline-clipped and clock-read-fails must fail.
- the row wait bounded by the former A733_POWER_PCK_BOUND_US + 4,000 us; row-timeout must fail.

Usage: os-g2d-open-wait.py --build BUILD --output DIR, BUILD being the build directory of the ret-g2d gate image."""
import argparse
import hashlib
import re
import shlex
import subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_MEM_UNMAPPED, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_X0, UC_ARM64_REG_X1, \
    UC_ARM64_REG_X2
from harness import machine as harness_machine, environment, flat_members, Image, hook_addresses, save, skip_crumbs

GATE_RUNS = [['--build', '{build:ret-g2d}', '--output', '{output}']]
CHECK_INPUTS = ['apps/os/g2d.c', 'include/a733_power_domains.h', 'include/a733_power_domain_row.h',
                'include/sunstone_g2d.h']
ROOT = Path(__file__).resolve().parents[1]
UNIT = '/apps/os/g2d.c'
ELF_NAME = 'sunstone-a733-os-g2d-check.elf'
TARGET_DIR = 'CMakeFiles/sunstone-a733-os-g2d-check.dir'
CORE_DIR = 'CMakeFiles/sunstone-a733-os-core.dir'
CCU, R_CCU, PCK1, RTC, GICD = 0x02002000, 0x07010000, 0x07061000, 0x07090000, 0x03400000
STUBS, STACK = 0x60100000, 0x60800000
PMIC_NOW, NOTIFY = STUBS, STUBS + 0x100
T0 = 2_000_000
CALL_US = 500_000                      # G2D_CALL_US
PCK_BOUND_US, TICK_US = 10_000, 1000
DCDC2_VERIFIED = 1 << 2                # A733_POWER_DCDC2_VERIFIED
GENERATION_MASK = 0xffffffff >> 3      # A733_POWER_ROW_GENERATION_MASK
# Entry words that g2d.c's entry checks (peri0_ready and the engine and INTID checks) accept, not a board measurement:
# the 24 MHz DCXO (RTC 0x160) and PLL_REF at N = M = 100; PERI0 enabled and locked with N = 100 and bits 22:20 at 1,
# which peri0_ready reads as a divide by 8 (24 MHz x 100 / 8 = 300 MHz); its gates and status open; the engine's
# clock, bus gate and reset closed; the GIC words of INTID 83 clear.
ENTRY = {RTC + 0x160: 0x10000003, CCU: 0xf8636300, CCU + 0xa0: 0xfe106300, CCU + 0x1908: 0x0a300a30,
         CCU + 0x1988: 0x0a300000}
# The transition record the model owner publishes with its ON answer.
TRANSITION = {'policy_before': 0xa5a50100, 'status_before': 0x100, 'policy_written': 0xa5a50108, 'first_word': 0x100,
              'pending_word': 0x100, 'reached_word': 0x108, 'all_and': 0x100, 'all_or': 0x108, 'reads': 5,
              'spin_reads': 3, 'commands': 1}
MUTANTS = [
    ('const uint64_t until=a733_power_supply_deadline(&providers.domains,g2d.limit);',
     'const uint64_t until=now_us()+G2D_CALL_US;', 'supply wait on the fixed 500 ms', {'supply-deadline-clipped', 'clock-read-fails'}),
    ('const uint64_t row_limit=a733_power_row_deadline(&providers.domains,now_us(),g2d.limit);',
     'const uint64_t row_limit=now_us()+A733_POWER_PCK_BOUND_US+4000;', 'row wait on the fixed 14 ms', {'row-timeout'}),
]
CASES = {
    'verified-before': dict(verified=0, answer=600),
    'verified-during': dict(verified=T0 + 8000, answer=600),
    'supply-deadline-passes': dict(),
    'supply-deadline-clipped': dict(session=T0 + 60_000),
    'clock-read-fails': dict(clock_fails=True),
    'row-answer-after-supply': dict(verified=T0 + 3000, answer=11_000),
    'row-timeout': dict(verified=0),
    'row-deadline-clipped': dict(verified=0, session=T0 + 18_000),
}


def load(path):
    image = Image(path, {'board_application', 'a733_board_session', 'board_provider', 'a733_native_usb',
                         'a733_rail_leases', 'a733_power_domains', 'a733_power_service_config', 'a733_pmic',
                         'a733_pmic_config', 'a733_power_domain_row', 'a733_power_transition', 'a733_rail_token',
                         'a733_clock_token', 'ss_g2d_info', 'ss_g2d_transition'})
    fields, enums = None, {}
    with Path(path).open('rb') as stream:
        elf = ELFFile(stream)
        parts = [(int(s['p_vaddr']), bytes(s.data()), int(s['p_memsz']))
                 for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
        for cu in elf.get_dwarf_info().iter_CUs():
            if not cu.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith(UNIT):
                continue
            for die in cu.iter_DIEs():
                if die.tag == 'DW_TAG_variable' and die.attributes.get('DW_AT_name') and \
                        die.attributes['DW_AT_name'].value == b'g2d' and 'DW_AT_type' in die.attributes:
                    fields = flat_members(die.get_DIE_from_attribute('DW_AT_type'))
                if die.tag == 'DW_TAG_enumerator':
                    enums[die.attributes['DW_AT_name'].value.decode()] = die.attributes['DW_AT_const_value'].value
    assert fields, 'no g2d state in ' + UNIT
    return image, parts, fields, enums


def run_case(image, parts, fields, enums, name, verified=None, answer=None, session=60_000_000, clock_fails=False):
    uc = harness_machine()
    low = min(a for a, _, _ in parts) & ~0xfff
    high = (max(a + n for a, _, n in parts) + 0xfff) & ~0xfff
    uc.mem_map(low, high - low)
    for address, data, _ in parts:
        uc.mem_write(address, data)
    for base, size in ((CCU, 0x2000), (R_CCU, 0x1000), (PCK1, 0x1000), (RTC, 0x1000), (GICD, 0x1000), (STUBS, 0x1000)):
        uc.mem_map(base, size)
    uc.mem_map(STACK - 0x10000, 0x10000)
    skip_crumbs(uc, image)
    L = image.layouts

    def put(address, value, n=4): uc.mem_write(address, int(value).to_bytes(n, 'little'))
    def get(address, n=4): return int.from_bytes(uc.mem_read(address, n), 'little')
    for address, word in ENTRY.items():
        put(address, word)
    g2d = image.address('g2d', UNIT)
    def field(n): return g2d + fields[n]
    board = image.address('board')
    put(board + L['board_application']['session'] + L['a733_board_session']['deadline_us'], session, 8)
    providers = image.address('providers')
    domains = providers + L['board_provider']['domains']
    shared = providers + L['board_provider']['shared']
    pmic = shared + L['a733_native_usb']['pmic']
    authority = shared + L['a733_native_usb']['rails'] + L['a733_rail_leases']['authority']
    put(domains + L['a733_power_domains']['pmic'], pmic, 8)
    put(domains + L['a733_power_domains']['config'] + L['a733_power_service_config']['tick_us'], TICK_US)
    put(pmic + L['a733_pmic']['pmic_bus_ops'], PMIC_NOW, 8)     # its first member, now
    put(pmic + L['a733_pmic']['config'] + L['a733_pmic_config']['startup_deadline'], session, 8)
    if verified == 0:
        put(authority, DCDC2_VERIFIED)
    # The leases the start entry and an earlier open left held, the domain-1 row that the start entry registered, and
    # the posted open.
    put(field('rail') + L['a733_rail_token']['id'], 1, 8)
    put(field('clock') + L['a733_clock_token']['id'], 1, 8)
    put(field('pck_token'), 1, 8)
    row = field('row')
    RL = L['a733_power_domain_row']
    put(row + RL['domains'], domains, 8); put(row + RL['notify'], NOTIFY, 8); put(row + RL['domain'], 1)
    put(field('dev.phase'), enums['OS_DEVICE_CLOSED']); put(field('op'), enums['OPEN_OP']); put(field('request'), enums['POSTED'])

    state = {'time': T0, 'waits': [], 'request_at': None, 'answer_at': None, 'stopped': None, 'unmapped': None,
             'writes': []}
    events = []
    if verified:
        events.append((verified, 'verify'))

    def ret(m, value=None):
        if value is not None:
            m.reg_write(UC_ARM64_REG_X0, value & 0xffffffffffffffff)
        m.reg_write(UC_ARM64_REG_PC, m.reg_read(UC_ARM64_REG_LR))

    def clock_out(m):
        put(m.reg_read(UC_ARM64_REG_X1), state['time'], 8)
        ret(m, 1)

    def apply(kind):
        if kind == 'verify':
            put(authority, get(authority) | DCDC2_VERIFIED)
            return
        generation = (get(row + RL['request']) >> 1) & GENERATION_MASK
        t = row + RL['transition']
        TL = L['a733_power_transition']
        for k, v in TRANSITION.items():
            put(t + TL[k], v)
        for k, v in (('command_at', state['answer_at'] - 400), ('first_at', state['answer_at'] - 350),
                     ('pending_at', state['answer_at'] - 100), ('reached_at', state['answer_at'])):
            put(t + TL[k], v, 8)
        put(row + RL['state'], generation << 3 | enums['A733_POWER_ROW_ON'])

    def wait(m):
        target = m.reg_read(UC_ARM64_REG_X0)
        state['waits'].append(('row' if state['request_at'] is not None else 'supply', state['time'], target))
        due = sorted(e for e in events if e[0] <= target)
        if due:
            at, kind = due[0]
            events.remove(due[0])
            state['time'] = max(state['time'], at)
            apply(kind)
        else:
            state['time'] = max(state['time'], target)
        ret(m, 1)

    def notify(m):
        state['request_at'] = state['time']
        if answer is not None:
            state['answer_at'] = state['time'] + answer
            events.append((state['answer_at'], 'answer'))
        ret(m)

    def stop(why):
        def hook(m):
            state['stopped'] = why
            m.emu_stop()
        return hook

    def check(m):
        put(m.reg_read(UC_ARM64_REG_X2), 8)                    # domain 1 ON, its gates and PPU clock consistent
        ret(m, 1)

    def translate(m):
        address, out = m.reg_read(UC_ARM64_REG_X0), m.reg_read(UC_ARM64_REG_X2)
        uc.mem_write(out, address.to_bytes(8, 'little') + bytes(8))      # physical, Device-nGnRnE
        ret(m, 1)

    hooks = {
        'sunstone_deadline_register': lambda m: ret(m, 1),
        'sunstone_deadline_now_us': lambda m: ret(m, state['time']),
        'sunstone_deadline_wait_until': wait,
        'a733_counter_clock_now': clock_out,
        'ss_counter_ticks': lambda m: ret(m, state['time'] * 24),
        'a733_rail_poll': lambda m: ret(m, enums['A733_RAIL_READY']),
        'a733_power_domain_check': check,
        'aarch64_device_translate': translate,
        'board_live': lambda m: ret(m, 1),
        'a733_platform_lock_enter': stop('first CCU write'),
        'os_application_wake_task': stop('answered'),
    }
    for symbol, callback in hooks.items():
        hook_addresses(uc, {image.address(symbol)}, lambda m, a, s, u, c=callback: c(m))
    hook_addresses(uc, {image.address('pause_us', UNIT)}, lambda m, a, s, u: ret(m))
    hook_addresses(uc, {PMIC_NOW}, lambda m, a, s, u: ret(m, 0) if clock_fails else clock_out(m))

    def written(m, access, address, size, value, data):
        state['writes'].append((hex(address), hex(value)))
    for base, size in ((CCU, 0x2000), (R_CCU, 0x1000), (PCK1, 0x1000), (RTC, 0x1000), (GICD, 0x1000)):
        uc.hook_add(UC_HOOK_MEM_WRITE, written, begin=base, end=base + size - 1)
    hook_addresses(uc, {NOTIFY}, lambda m, a, s, u: notify(m))

    def unmapped(m, access, address, size, value, data):
        state['unmapped'] = (access, hex(address), hex(m.reg_read(UC_ARM64_REG_PC)))
        return False
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, unmapped)
    uc.reg_write(UC_ARM64_REG_SP, STACK - 0x100)
    uc.reg_write(UC_ARM64_REG_LR, 0)
    try:
        uc.emu_start(image.address('os_g2d_task', UNIT), 0, count=20_000_000)
    except Exception as error:
        raise AssertionError(f'{name}: emulation stopped at {hex(uc.reg_read(UC_ARM64_REG_PC))}: {error}; '
                             f'unmapped access {state["unmapped"]}')
    assert state['stopped'], (name, 'the open neither answered nor reached its first CCU write',
                              hex(uc.reg_read(UC_ARM64_REG_PC)), state['waits'])
    info = field('info')
    IL, XL = L['ss_g2d_info'], L['ss_g2d_transition']
    record = {'case': name, 'stopped': state['stopped'], 'waits': state['waits'], 'request_at': state['request_at'],
              'answer_at': state['answer_at'], 'end': state['time'], 'phase': get(field('dev.phase')),
              'register_writes': state['writes'],
              'fault_step': get(field('dev.fault_step')),
              'result': int.from_bytes(uc.mem_read(field('result'), 4), 'little', signed=True),
              'transition': {k: get(info + IL['transition'] + XL[k], 8 if k.endswith('_at') else 4)
                             for k in XL}}
    return record


def expect(record, enums, verified=None, answer=None, session=60_000_000, clock_fails=False):
    """The bounds and outcome the case must show; an empty list when it does."""
    problems = []
    limit = min(T0 + CALL_US, session - 10_000)
    supply = 0 if clock_fails else limit
    if record['register_writes']:
        problems.append(f"no register may be written before the row's ON answer: {record['register_writes'][:6]}")
    supply_waits = [w for w in record['waits'] if w[0] == 'supply']
    row_waits = [w for w in record['waits'] if w[0] == 'row']
    if verified == 0:
        if supply_waits:
            problems.append(f'a supply wait with DCDC2 already verified: {supply_waits}')
    elif clock_fails:
        if supply_waits:
            problems.append(f'a supply wait after a failed clock read: {supply_waits}')
    elif not supply_waits or supply_waits[0][2] != supply:
        problems.append(f'the supply wait must target {supply} us: {supply_waits}')
    if verified is None:
        if record['request_at'] is not None or record['stopped'] != 'answered':
            problems.append('a row request without a verified supply')
        if record['result'] != -1 or record['fault_step'] != enums['G2D_STEP_SUPPLY'] or record['phase'] != enums['OS_DEVICE_CLOSED']:
            problems.append(f'the open must be refused at the supply step, CLOSED: {record}')
        if record['end'] != (T0 if clock_fails else supply):
            problems.append(f'the refusal must come at the supply deadline {supply} us, not {record["end"]} us')
        return problems
    request = verified or T0
    if record['request_at'] != request:
        problems.append(f'the row must be requested at {request} us, not {record["request_at"]}')
        return problems
    row = min(request + PCK_BOUND_US + 2 * TICK_US, limit)
    if not row_waits or row_waits[0][2] != row:
        problems.append(f'the row wait must target {row} us: {row_waits}')
    if answer is None:
        if record['result'] != -1 or record['fault_step'] != enums['G2D_STEP_ROW_TIMEOUT'] or \
                record['phase'] != enums['OS_DEVICE_FAULT'] or record['end'] != row:
            problems.append(f'the open must fail at the row deadline {row} us with ROW_TIMEOUT, FAULT: {record}')
        return problems
    if record['stopped'] != 'first CCU write':
        problems.append(f'the open must proceed to its first CCU write after the ON answer: {record}')
    copied = record['transition']
    want = dict(TRANSITION, command_at=record['answer_at'] - 400, first_at=record['answer_at'] - 350,
                pending_at=record['answer_at'] - 100, reached_at=record['answer_at'])
    if copied != want:
        problems.append(f'the record must carry the row transition {want}: {copied}')
    return problems


def run_all(elf):
    image, parts, fields, enums = load(elf)
    rows = []
    for name, options in CASES.items():
        try:
            record = run_case(image, parts, fields, enums, name, **options)
            record['problems'] = expect(record, enums, **options)
        except AssertionError as error:
            record = {'case': name, 'problems': [str(error)]}
        record['passed'] = not record['problems']
        rows.append(record)
    return rows


def flags(build):
    """The compile variables of the OS core library, which compiles apps/os/g2d.c."""
    out = {}
    for line in (build / CORE_DIR / 'flags.make').read_text().splitlines():
        m = re.match(r'(C_DEFINES|C_INCLUDES|C_FLAGS) = (.*)', line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def mutant_image(build, out, index, old, new):
    """The image with g2d.c compiled from one changed text and linked by the image's own link command."""
    text = (ROOT / 'apps/os/g2d.c').read_text()
    assert text.count(old) == 1, ('mutation site must occur once', old)
    source = out / f'mutant-{index}' / 'apps/os/g2d.c'
    source.parent.mkdir(parents=True)
    source.write_text(text.replace(old, new))
    obj, elf = source.parent / 'g2d.c.obj', out / f'mutant-{index}' / ELF_NAME
    rule = next(line.strip() for line in (build / CORE_DIR / 'build.make').read_text().splitlines()
                if line.startswith('\t') and '-o ' + CORE_DIR + '/apps/os/g2d.c.obj -c ' in line)
    variables = flags(build)
    rule = re.sub(r'\$\((C_DEFINES|C_INCLUDES|C_FLAGS)\)', lambda m: variables[m.group(1)], rule)
    words = shlex.split(rule)
    for option in ('-MT', '-MF'):
        i = words.index(option)
        del words[i:i + 2]
    words.remove('-MD')
    words[words.index('-o') + 1] = str(obj)
    words[-1] = str(source)
    log = out / f'mutant-{index}.log'
    with log.open('w') as stream:
        subprocess.run(words, cwd=build, check=True, env=environment(), stdout=stream, stderr=subprocess.STDOUT)
        link = shlex.split((build / TARGET_DIR / 'link.txt').read_text())
        target = CORE_DIR + '/apps/os/g2d.c.obj'
        assert link.count(target) == 1, 'the image links g2d.c once'
        link[link.index(target)] = str(obj)
        link[link.index('-o') + 1] = str(elf)
        link = [re.sub(r'^-Wl,-Map,.*', '-Wl,-Map,' + str(elf.with_suffix('.map')), w) for w in link]
        subprocess.run(link, cwd=build, check=True, env=environment(), stdout=stream, stderr=subprocess.STDOUT)
    return elf


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    build, elf = a.build.resolve(), a.build.resolve() / ELF_NAME
    rows = run_all(elf)
    failures = [r for r in rows if not r['passed']]
    mutants = []
    if not failures:
        for index, (old, new, why, must_fail) in enumerate(MUTANTS):
            failed = {r['case'] for r in run_all(mutant_image(build, a.output, index, old, new)) if not r['passed']}
            mutants.append({'mutant': why, 'failed': sorted(failed), 'refused': must_fail <= failed})
    save(a.output / 'result.json', {
        'passed': not failures and all(m['refused'] for m in mutants), 'physical_access': False, 'image': str(elf),
        'image_sha256': hashlib.sha256(elf.read_bytes()).hexdigest(), 'cases': rows, 'mutants': mutants})
    for row in failures:
        print('FAIL', row['case'], '; '.join(row['problems'])[:800])
    for m in mutants:
        if not m['refused']:
            print('FAIL mutant survived:', m['mutant'], 'failed only', m['failed'])
    if failures or not all(m['refused'] for m in mutants):
        raise SystemExit(1)
    print(f'PASS G2D open waits: {len(rows)} cases on the supply and row deadlines, {len(mutants)} mutants refused')


if __name__ == '__main__':
    main()
