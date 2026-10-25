#!/usr/bin/env python3
"""Check the shared SRAM routing of the finite NPU image on its linked instructions.

  sram-route-check.py --evidence DIR --output RESULT.json

DIR holds image.elf from apps/npu-finite/build.py. Unicorn runs the image's
own startup sequencer and NPU allowlist adapter from the register pass to
READY and through local stop, with the CCU, PCK600 and SYSCTRL words as
modeled memory. Scheduler state, the current task, the counter, the register
lock, the domain-4 check and the power service's answer to the domain-4 row
are modeled. The cases cover:

- a cold start: the domain-4 ON request before any device write, PLL_NPU
  enabled after power, and SYSCTRL 0x03000008 bit 1 cleared after the PLL
  output gate opens and before the first write of the NPU bus and reset word;
- local stop, which routes the SRAM back to the CPU after the resets;
- a second start over the state that local stop leaves, which adopts PLL_NPU
  without a PLL or gate-control write;
- the two routing refusals: an entry word that already routes the SRAM to the
  NPU, and a word that ignores the write;
- the register window of the convolution backend, closed before READY and
  open at READY;
- the image's only RMR_EL3 write and its only caller, the EL3 return path,
  whose SRAM restore apps/npu-finite/check.py executes;
- no loadable segment, symbol or finite launch region in the shared SRAM at
  0x00074000-0x000f3fff.

No device access.
"""
import argparse, hashlib, json, re, struct, sys
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR, \
    UC_ARM64_REG_PSTATE

p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('--evidence', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--rate', choices=('vendor', '492', '852'), default='vendor',
               help="the image's profile: the vendor point for the speed bin (default), or build.py --rate-492 or --rate-852")
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'checks'))
import harness  # noqa: E402

elf = a.evidence / 'image.elf'
LAYOUTS = ('a733_npu_startup', 'a733_npu_startup_config', 'a733_npu_platform_config', 'a733_npu_startup_providers',
           'a733_power_domain_row', 'a733_counter_clock', 'a733_clock_leases', 'a733_platform_locks',
           'a733_power_domains', 'a733_pmic', 'a733_pmic_config', 'npu_convolution', 'supported_model')
im = harness.Image(elf, layout_names=LAYOUTS)
L = im.layouts
SYSCTRL, SHARED = 0x03000008, (0x00074000, 0x000f4000)
CCU, PCK, PLL = 0x02002000, 0x07064000, 0x020022a0
BUS, MODULE, GATES = CCU + 0xb04, CCU + 0xb00, CCU + 0x1920
SENTINEL, STACK = 0x43fff000, 0x43ffeff0


def enum(header, names):
    text = (root / header).read_text()
    for body in re.findall(r'enum[^{]*\{([^}]*)\}', text):
        members = [m.strip().split('=')[0].strip() for m in re.sub(r'/\*.*?\*/', '', body, flags=re.S).split(',')
                   if m.strip()]
        if all(n in members for n in names):
            return {n: members.index(n) for n in names}
    raise AssertionError((header, names))


STAGE = enum('include/a733_npu_startup.h', ('A733_NPU_STARTUP_PASS', 'A733_NPU_STARTUP_ROW',
                                            'A733_NPU_STARTUP_SETTLE', 'A733_NPU_STARTUP_READY',
                                            'A733_NPU_STARTUP_STOPPED'))
RESULT = enum('include/a733_npu_platform.h', ('A733_NPU_PLATFORM_READY', 'A733_NPU_PLATFORM_PENDING',
                                             'A733_NPU_PLATFORM_RETAINED'))
ROW_ON = enum('include/a733_power_domain_row.h', ('A733_POWER_ROW_ON',))['A733_POWER_ROW_ON']
# AHB, NSI and MBUS on the 600 MHz PERI0 output at 200, 600 and 600 MHz, as in
# checks/npu-application-clock-reference.py, with a 24 MHz oscillator.
FABRIC, RATES = (0x03000002, 0x83000000, 0x81000000), (200000000, 600000000, 600000000)
COLD = {0x07090160: 0x10000002, CCU: 0xf8636300, CCU + 0xa0: 0xff126310, CCU + 0xac: 0x30000000,
        CCU + 0x1908: 0x8fff0fff, CCU + 0x1988: 0x0fff0000, CCU + 0x500: FABRIC[0], CCU + 0x580: FABRIC[1],
        CCU + 0x588: FABRIC[2], BUS: 1 << 19, CCU + 0xc00: 1 << 31, CCU + 0xc0c: 1 << 16,
        CCU + 0x2ac: 0x30000000, CCU + 0x2b0: 0x000b0000, PLL: 0x41104500, SYSCTRL: 2,
        0x0300624c: 0x496}  # Port 3-3's SID speed-bin word: code 4, column vf0400.
HOOKED = ('xTaskGetSchedulerState', 'xTaskGetCurrentTaskHandle', 'a733_counter_clock_now', 'a733_platform_lock_enter',
          'a733_platform_lock_leave', 'a733_platform_lock_retain', 'a733_power_domain_check', 'a733_clock_release',
          'a733_rail_release', 'a733_gpu_resources_release')
CURRENTEL, CNTPCT = 0xd5384240, 0xd53be020
SITES = sorted(harness.instruction_sites(im.parts, lambda w: w & 0xffffffe0 in (CURRENTEL, CNTPCT)))


class Machine:
    """The linked sequencer over modeled memory, from the register pass."""

    def __init__(self, words, domain=0, stuck=False):
        self.u = u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        u.mem_map(0x02000000, 0x07000000)
        u.mem_map(0x40000000, 0x04000000)
        u.reg_write(UC_ARM64_REG_PSTATE, 0x3c5)
        for at, data in im.parts:
            if data:
                u.mem_write(at, data)
        self.domain, self.stuck, self.time, self.writes, self.running = domain, stuck, 1000, [], False
        base = 0x43000000
        self.s, self.task = base, base + 0x10000
        counter, clocks, locks, domains, pmic = base + 0x8000, base + 0x9000, base + 0xa000, base + 0xb000, base + 0xc000
        cfg, prov = base + 0xd000, base + 0xd800
        self.put(counter + L['a733_counter_clock']['initialized'], 1, 4)
        self.put(clocks + L['a733_clock_leases']['initialized'], 1, 4)
        self.put(locks + L['a733_platform_locks']['initialized'], 1, 4)
        self.put(domains + L['a733_power_domains']['self'], domains)
        self.put(domains + L['a733_power_domains']['pmic'], pmic)
        # The owner's service wake (ops.notify, the third callback) returns at once.
        self.put(domains + L['a733_power_domains']['ops'] + 16, im.address('a733_platform_lock_leave'))
        self.put(pmic + L['a733_pmic']['config'] + L['a733_pmic_config']['startup_deadline'], 2000000)
        f = cfg + L['a733_npu_startup_config']['fabric']
        for i in range(3):
            self.put(f + L['a733_npu_platform_config']['fabric'] + 4 * i, FABRIC[i], 4)
            self.put(f + L['a733_npu_platform_config']['rate_hz'] + 4 * i, RATES[i], 4)
        self.put(cfg + L['a733_npu_startup_config']['owner'], 31)
        self.put(cfg + L['a733_npu_startup_config']['deadline_us'], 10 ** 9)
        self.put(cfg + L['a733_npu_startup_config']['pck_client'], 4, 4)
        for name, value in (('counter', counter), ('clocks', clocks), ('locks', locks), ('pck', base + 0xe000),
                            ('rails', base + 0xf000), ('domains', domains)):
            self.put(prov + L['a733_npu_startup_providers'][name], value)
        for at, v in words.items():
            self.put(at, v, 4)
        self.put(PCK, domain, 4)
        self.put(PCK + 8, domain, 4)
        self.hooks = {im.address(n): n for n in HOOKED}
        for at in set(self.hooks) | set(SITES) | {SENTINEL}:
            u.hook_add(UC_HOOK_CODE, self.code, begin=at, end=at)
        u.hook_add(UC_HOOK_MEM_WRITE, self.write, begin=0x02000000, end=0x08ffffff)
        u.hook_add(UC_HOOK_MEM_READ, self.read, begin=0x02000000, end=0x08ffffff)
        assert self.call('a733_npu_startup_init', self.s, cfg, prov, 0, 0) == 1
        # The default image links no rate selection; an optional mode is
        # selected as its application does.
        if a.rate != 'vendor':
            assert self.call('a733_npu_startup_rate', self.s, int(a.rate)) == 1
        assert self.call('a733_npu_startup_bind_task', self.s, self.task) == 1
        self.put(self.s + L['a733_npu_startup']['stage'], STAGE['A733_NPU_STARTUP_PASS'], 4)
        self.running = True

    def put(self, at, value, n=8):
        self.u.mem_write(at, int(value).to_bytes(n, 'little'))

    def get(self, at, n=8):
        return int.from_bytes(self.u.mem_read(at, n), 'little')

    def field(self, name, n=4):
        return self.get(self.s + L['a733_npu_startup'][name], n)

    def returned(self, value):
        self.u.reg_write(UC_ARM64_REG_X0, value)
        self.u.reg_write(UC_ARM64_REG_PC, self.u.reg_read(UC_ARM64_REG_LR))

    def code(self, u, at, size, unused):
        if at == SENTINEL:
            self.done = True
            u.emu_stop()
            return
        name = self.hooks.get(at)
        if name is None:
            word = self.get(at, 4)
            if word & 0xffffffe0 == CURRENTEL:
                u.reg_write(UC_ARM64_REG_X0 + (word & 31), 4)       # EL1
            else:
                self.time += 1
                u.reg_write(UC_ARM64_REG_X0 + (word & 31), self.time * 24)
            u.reg_write(UC_ARM64_REG_PC, at + 4)
            return
        if name == 'xTaskGetSchedulerState':
            self.returned(2 if self.running else 1)
        elif name == 'xTaskGetCurrentTaskHandle':
            self.returned(self.task)
        elif name == 'a733_counter_clock_now':
            self.time += 1
            self.put(u.reg_read(UC_ARM64_REG_X0 + 1), self.time)
            self.returned(1)
        elif name == 'a733_power_domain_check':
            self.put(u.reg_read(UC_ARM64_REG_X0 + 2), self.domain, 4)
            self.returned(1)
        elif name == 'a733_platform_lock_leave':
            self.returned(0)
        else:
            self.returned(1)

    def write(self, u, access, at, size, value, unused):
        self.writes.append((at, value))

    def read(self, u, access, at, size, value, unused):
        if at == PLL:
            word = self.get(PLL, 4)
            if word & 0xa0000000 == 0xa0000000:
                self.put(PLL, word | 1 << 28, 4)                     # Locked once enabled.
        elif at == CCU + 0x19a0:
            self.put(at, 0x10000 if self.get(GATES, 4) & 0x10001 == 0x10001 else 0, 4)
        elif at == SYSCTRL and self.stuck:
            self.put(SYSCTRL, 2, 4)                                 # A word that ignores writes.

    def call(self, name, *args):
        self.done = False
        for i, v in enumerate(args):
            self.u.reg_write(UC_ARM64_REG_X0 + i, v)
        self.u.reg_write(UC_ARM64_REG_SP, STACK)
        self.u.reg_write(UC_ARM64_REG_LR, SENTINEL)
        self.u.emu_start(im.address(name), 0, count=20000000)
        assert self.done, name
        return self.u.reg_read(UC_ARM64_REG_X0) & 0xffffffff

    def step(self):
        return self.call('a733_npu_startup_step', self.s)

    def answer_row(self):
        """The power service answers the ON request; domain 4 reads ON."""
        row = self.s + L['a733_npu_startup']['row']
        request = self.get(row + L['a733_power_domain_row']['request'], 4)
        assert request & 1
        self.put(row + L['a733_power_domain_row']['state'], (request >> 1) << 3 | ROW_ON, 4)
        self.put(PCK, 8, 4)
        self.put(PCK + 8, 8, 4)

    def window(self):
        return self.field('window_open')

    def first(self, match):
        return next((i for i, w in enumerate(self.writes) if match(*w)), None)


results = []


def case(name, ok, **facts):
    assert ok, (name, facts)
    results.append({'case': name, 'passed': True, **facts})


def boot(m):
    """Pass, row, enable, open, settle and final; the results of the three calls."""
    first = m.step()
    after_pass = (m.field('stage'), list(m.writes))
    m.answer_row()
    second = m.step()
    after_open = (m.field('stage'), m.window())
    m.time += 3000                                                   # Past the 2 ms settle.
    third = m.step()
    return first, after_pass, second, after_open, third


# A cold start with domain 4 OFF.
m = Machine(COLD, domain=0)
first, after_pass, second, after_open, third = boot(m)
gate = m.first(lambda at, v: at == PLL and v & 1 << 27)
route = m.first(lambda at, v: at == SYSCTRL)
reset = m.first(lambda at, v: at == BUS)
case('cold-start-power-before-clocks-and-routing-before-reset-release',
     first == RESULT['A733_NPU_PLATFORM_PENDING'] and after_pass == (STAGE['A733_NPU_STARTUP_ROW'], []) and
     second == RESULT['A733_NPU_PLATFORM_PENDING'] and after_open == (STAGE['A733_NPU_STARTUP_SETTLE'], 0) and
     third == RESULT['A733_NPU_PLATFORM_READY'] and m.field('stage') == STAGE['A733_NPU_STARTUP_READY'] and
     m.window() == 1 and m.writes[0][0] == GATES and None not in (gate, route, reset) and
     gate < route and route + 1 == reset and m.writes[route][1] == 0 and m.get(SYSCTRL, 4) == 0,
     writes=len(m.writes), pll_output_gate_write=gate, routing_write=route, first_reset_write=reset)
before = len(m.writes)
stop = m.call('a733_npu_startup_stop', m.s)
tail = m.writes[before:]
case('local-stop-routes-the-sram-back-after-the-resets',
     stop == RESULT['A733_NPU_PLATFORM_READY'] and m.window() == 0 and
     m.field('stage') == STAGE['A733_NPU_STARTUP_STOPPED'] and tail[-1] == (SYSCTRL, 2) and
     [at for at, v in tail[:-1]] == [MODULE, BUS, BUS, BUS, BUS] and m.get(BUS, 4) & 0xf0001 == 1 << 19,
     stop_writes=len(tail))
left = {at: m.get(at, 4) for at in (PLL, GATES, BUS, MODULE)}

# A second start in the same power lifetime: the ROM return keeps bit 1 set.
m = Machine({**COLD, **left, SYSCTRL: 2}, domain=8)
first, after_pass, second, after_open, third = boot(m)
case('second-start-adopts-the-locked-pll-and-the-on-domain',
     first == RESULT['A733_NPU_PLATFORM_PENDING'] and second == RESULT['A733_NPU_PLATFORM_PENDING'] and
     third == RESULT['A733_NPU_PLATFORM_READY'] and m.field('pll_adopted') == 1 and
     not [w for w in m.writes if w[0] in (PLL, GATES)] and m.writes[0] == (SYSCTRL, 0) and
     m.writes[1][0] == BUS and m.get(PLL, 4) == left[PLL] and m.window() == 1,
     pll_word=hex(left[PLL]), writes=len(m.writes))

# An entry word that already routes the SRAM to the NPU refuses in the
# read-only pass, before the domain-4 request.
m = Machine({**COLD, SYSCTRL: 0}, domain=0)
first = m.step()
request = m.get(m.s + L['a733_npu_startup']['row'] + L['a733_power_domain_row']['request'], 4)
case('routing-refused-in-the-pass-when-entry-routes-to-npu',
     first == RESULT['A733_NPU_PLATFORM_RETAINED'] and not m.writes and not request & 1 and m.window() == 0)
# A word that ignores the write retains after PLL_NPU locks, before any reset release.
m = Machine(COLD, domain=0, stuck=True)
first = m.step()
m.answer_row()
second = m.step()
case('routing-refused-when-the-word-ignores-the-write',
     first == RESULT['A733_NPU_PLATFORM_PENDING'] and second == RESULT['A733_NPU_PLATFORM_RETAINED'] and
     m.first(lambda at, v: at in (BUS, MODULE)) is None and m.window() == 0 and m.field('sram_routed') == 0 and
     [v for at, v in m.writes if at == SYSCTRL] == [0])

# A rate change: the earlier start left PLL_NPU running at the other profile.
# The vendor order: output gate off, factors, lock enable off then on, LOCK,
# then the output gate on in the open stage.
# The vendor point for the modeled speed bin, code 4 (vf0400), is 852 MHz.
other = 0xf8104600 if a.rate == '492' else 0xf8305100
factors = 0x00305100 if a.rate == '492' else 0x00104600
m = Machine({**COLD, **left, PLL: other | (left[PLL] & 1 << 24), SYSCTRL: 2}, domain=8)
first, after_pass, second, after_open, third = boot(m)
pll_writes = [v & ~(1 << 24) for at, v in m.writes if at == PLL]
case('rate-change-from-the-other-profile-in-the-vendor-order',
     third == RESULT['A733_NPU_PLATFORM_READY'] and m.field('pll_adopted') == 0 and
     pll_writes == [(other & ~(1 << 28)) & ~(1 << 27), 0xe0000000 | factors, 0xc0000000 | factors,
                    0xe0000000 | factors, 0xe8000000 | factors] and
     not [w for w in m.writes if w[0] == GATES], pll_writes=[hex(v) for v in pll_writes])
if a.rate != '492':
    # 852 MHz only for a speed bin whose column lists 800 mV for it; the
    # vendor point of vf0300 is 1,008 MHz at 900 mV, which needs a DCDC2 write.
    m = Machine({**COLD, 0x0300624c: 0x300}, domain=0)
    first = m.step()
    case('852-refused-for-a-speed-bin-without-an-800-mV-point',
         first == RESULT['A733_NPU_PLATFORM_RETAINED'] and not m.writes, sid='0x300')
if a.rate == 'vendor':
    # vf0401's vendor point is 1,008 MHz at 960 mV, and vf0600 has no row.
    for sid in (0x140000, 0x600):
        m = Machine({**COLD, 0x0300624c: sid}, domain=0)
        first = m.step()
        case('vendor-point-refused-for-speed-bin-' + hex(sid),
             first == RESULT['A733_NPU_PLATFORM_RETAINED'] and not m.writes, sid=hex(sid))

# The convolution backend's register callbacks on the linked instructions.
# A prepared backend for the first catalogue model; the application register
# callback behind it is stopped at its entry and answers 1.
CONV, START = 0x43200000, 0x43100000
conv, model = L['npu_convolution'], L['supported_model']
models = im.address('models', 'drivers/npu/npu_convolution.c')
table = im.address('a733_npu_application_operations')
application_read = next(struct.unpack_from('<Q', d, table - b)[0] for b, d in im.parts if b <= table < b + len(d))
read_reg = im.address('read_reg', 'drivers/npu/npu_convolution.c')
for window in (0, 1):
    m = Machine(COLD)
    for name in ('model_bytes', 'input_bytes', 'output_bytes'):
        m.put(CONV + conv[name], m.get(models + model[name], 4))
    m.put(CONV + conv['prepared'], 1, 4)
    m.put(CONV + conv['model_index'], 0, 4)
    m.put(CONV + conv['startup'], START)
    m.put(START + L['a733_npu_startup']['window_open'], window, 4)
    reached = []
    m.u.hook_add(UC_HOOK_CODE, lambda u, at, size, x: (reached.append(at), m.returned(1)),
                 begin=application_read, end=application_read)
    m.done = False
    for i, v in enumerate((CONV, 0x20, 0x43300000)):
        m.u.reg_write(UC_ARM64_REG_X0 + i, v)
    m.u.reg_write(UC_ARM64_REG_SP, STACK)
    m.u.reg_write(UC_ARM64_REG_LR, SENTINEL)
    m.u.emu_start(read_reg, 0, count=100000)
    r = m.u.reg_read(UC_ARM64_REG_X0) & 0xffffffff
    case('convolution-register-read-' + ('allowed-when-the-window-is-open' if window else 'refused-before-ready'),
         m.done and r == window and len(reached) == window)

# The image's only RMR_EL3 write is in npu_finite_rom_return, and the EL3
# return path is that function's only caller.
def sites(match):
    return [base + i for base, data in im.parts for i in range(0, len(data) - 3, 4)
            if match(struct.unpack_from('<I', data, i)[0])]


def end_of(start):
    return min(v for vs in im.symbols.values() for v in vs if v > start)


rmr_writes = sites(lambda w: (w & 0xffffffe0) == 0xd51ec040)
rom = im.address('npu_finite_rom_return')
rom_end = end_of(rom)
exception = im.address('sd_single_core_exception')
branches_in = []
for base, data in im.parts:
    for i in range(0, len(data) - 3, 4):
        w = struct.unpack_from('<I', data, i)[0]
        if w >> 26 in (0x25, 0x05):
            target = base + i + ((((w & 0x3ffffff) ^ 0x2000000) - 0x2000000) << 2)
            if rom <= target < rom_end and not rom <= base + i < rom_end:
                branches_in.append(base + i)
literal_refs = [base + i for base, data in im.parts for i in range(0, len(data) - 7, 8)
                if struct.unpack_from('<Q', data, i)[0] == rom]
case('only-rmr-write-reached-only-from-the-el3-return-path',
     len(rmr_writes) == 1 and rom <= rmr_writes[0] < rom_end and len(branches_in) == 1 and
     exception <= branches_in[0] < end_of(exception) and not literal_refs,
     rmr_write=[hex(x) for x in rmr_writes], call_site=[hex(x) for x in branches_in])

# Shared SRAM audit of the image and of the finite launch regions.
segments = [(base, base + len(data)) for base, data in im.parts]
symbols = sorted({v for vs in im.symbols.values() for v in vs if SHARED[0] <= v < SHARED[1]})
smp = (root / 'tools/check-a733-smp.py').read_text()
regions = [(n, int(b, 16), int(s, 16) if s.startswith('0x') else int(s))
           for n, b, s in re.findall(r"\('(\w+)', (0x[0-9a-f]+), (0x[0-9a-f]+|\d+)\)",
                                     smp.split('REGIONS = ', 1)[1].split(']', 1)[0])]
case('no-image-segment-or-symbol-in-shared-sram',
     all(hi <= SHARED[0] or lo >= SHARED[1] for lo, hi in segments) and not symbols,
     segments=[[hex(lo), hex(hi)] for lo, hi in segments])
case('launcher-regions-below-shared-sram', len(regions) == 5 and all(b + s <= SHARED[0] for _, b, s in regions),
     regions={n: [hex(b), hex(b + s)] for n, b, s in regions})

out = {'schema': 'npu-sram-route-check-v2', 'physical_access': False,
       'image_sha256': hashlib.sha256(elf.read_bytes()).hexdigest(), 'cases': results,
       'scope': 'Linked sequencer, allowlist adapter and convolution register callbacks over modeled CCU, PCK600 '
                'and SYSCTRL memory; the scheduler, counter, register lock, domain check and power service are '
                'modeled. It does not run the controller or the device, and it does not show what the NPU reads '
                'from its SRAM under either routing.'}
a.output.write_text(json.dumps(out, indent=1) + '\n')
print(f'PASS {len(results)} SRAM-routing cases')
