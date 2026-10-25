#!/usr/bin/env python3
"""Check the SRAM-routing variant of the finite NPU image on its linked instructions.

  sram-route-check.py --evidence DIR --output RESULT.json

DIR holds the variant's image.elf from apps/npu-finite/build-sram-route.py.
Unicorn runs the linked wrapper functions of sram-route.c and sram-route-el3.c;
the functions they redirect (the startup initializer, service activation, local
stop, the native register callbacks, the ROM return and a733_smp_stop) are
stopped at their entry and answered by the check. SYSCTRL 0x03000008 is
modeled, including a word that ignores writes. The check also confirms that
the image's call sites and its native-callback loads reach the wrappers, and
that no loadable segment, symbol or host SRAM region of the finite launch
lies in the shared SRAM at 0x00074000-0x000f3fff. No device access.
"""
import argparse, hashlib, importlib.util, json, re, struct, sys
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR

p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('--evidence', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'checks'))
import harness  # noqa: E402

elf = a.evidence / 'image.elf'
im = harness.Image(elf, layout_names=('a733_npu_startup',))
SYSCTRL, SHARED = 0x03000008, (0x00074000, 0x000f4000)
SENTINEL, STACK = 0x43fff000, 0x43ffe000


def enum(header, names):
    text = (root / header).read_text()
    for body in re.findall(r'enum[^{]*\{([^}]*)\}', text):
        members = [m.strip().split('=')[0].strip() for m in body.split(',') if m.strip()]
        if all(n in members for n in names):
            return {n: members.index(n) for n in names}
    raise AssertionError((header, names))


STAGE = enum('include/a733_npu_startup.h', ('A733_NPU_STARTUP_FINAL', 'A733_NPU_STARTUP_READY',
                                            'A733_NPU_STARTUP_STOPPED'))
PLATFORM = enum('include/a733_npu_platform.h', ('A733_NPU_PLATFORM_READY', 'A733_NPU_PLATFORM_BUSY',
                                               'A733_NPU_PLATFORM_RETAINED'))
SERVICE = enum('include/npu_service.h', ('NPU_SERVICE_OK', 'NPU_SERVICE_DENIED', 'NPU_SERVICE_FAULT'))
stage_offset = im.layouts['a733_npu_startup']['stage']
wrap = {n: im.address('__wrap_' + n) for n in ('a733_npu_startup_init', 'npu_service_activate',
                                               'npu_convolution_stop_poll', 'npu_finite_rom_return')}
real = {n: im.address(n) for n in ('a733_npu_startup_init', 'npu_service_activate', 'npu_convolution_stop_poll',
                                   'npu_finite_rom_return', 'a733_smp_stop')}
startup_ptr = im.address('sram_route_startup', 'sram-route.c')
routed_flag = im.address('sram_route_npu', 'sram-route.c')
ops_wrap, ops_real = im.address('__wrap_a733_npu_native_operations'), im.address('a733_npu_native_operations')


def loaded(at, n):
    for base, data in im.parts:
        if base <= at and at + n <= base + len(data):
            return data[at - base:at - base + n]
    raise AssertionError(hex(at))


wrapped_read, wrapped_write = struct.unpack('<2Q', loaded(ops_wrap, 16))
real_read, real_write = struct.unpack('<2Q', loaded(ops_real, 16))
STARTUP = 0x43a00000
STUBS = {**{real[n]: n for n in real}, real_read: 'real_read', real_write: 'real_write'}


class Machine:
    def __init__(self, sysctrl=0x2, stuck=False, real_returns=None):
        self.u = u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        u.mem_map(0x02000000, 0x02000000)
        u.mem_map(0x40000000, 0x04000000)
        for at, data in im.parts:
            if data:
                u.mem_write(at, data)
        self.sysctrl, self.stuck = sysctrl, stuck
        self.writes, self.reads, self.calls, self.window, self.steps = [], 0, [], [], 0
        self.returns = real_returns or {}
        u.hook_add(UC_HOOK_CODE, self.code)
        u.hook_add(UC_HOOK_MEM_READ, self.read, begin=SYSCTRL, end=SYSCTRL + 3)
        u.hook_add(UC_HOOK_MEM_WRITE, self.write, begin=SYSCTRL, end=SYSCTRL + 3)
        u.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, self.npu, begin=0x03600000, end=0x03600fff)
        u.mem_write(SYSCTRL, struct.pack('<I', sysctrl))

    def read(self, u, access, at, size, value, unused):
        self.reads += 1
        u.mem_write(SYSCTRL, struct.pack('<I', self.sysctrl))

    def write(self, u, access, at, size, value, unused):
        self.writes.append(value)
        if not self.stuck:
            self.sysctrl = value

    def npu(self, u, access, at, size, value, unused):
        self.window.append(at)

    def code(self, u, at, size, unused):
        name = STUBS.get(at)
        if name is None:
            self.steps += 1
        if name is None:
            return
        self.calls.append(name)
        if name in ('npu_finite_rom_return', 'a733_smp_stop'):
            u.emu_stop()
            return
        u.reg_write(UC_ARM64_REG_X0, self.returns.get(name, 1))
        u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_LR))

    def put(self, at, value, n=8):
        self.u.mem_write(at, int(value).to_bytes(n, 'little'))

    def get(self, at, n=8):
        return int.from_bytes(self.u.mem_read(at, n), 'little')

    def call(self, at, *args):
        for i, v in enumerate(args):
            self.u.reg_write(UC_ARM64_REG_X0 + i, v)
        self.u.reg_write(UC_ARM64_REG_SP, STACK)
        self.u.reg_write(UC_ARM64_REG_LR, SENTINEL)
        self.u.emu_start(at, SENTINEL, count=100000)
        return self.u.reg_read(UC_ARM64_REG_X0) & 0xffffffff

    def prepared(self, stage, routed=0):
        self.put(startup_ptr, STARTUP)
        self.put(STARTUP + stage_offset, stage, 4)
        self.put(routed_flag, routed, 4)


results = []


def case(name, ok, **facts):
    assert ok, (name, facts)
    results.append({'case': name, 'passed': True, **facts})


m = Machine()
r = m.call(wrapped_read, 0x43b00000, 0x20, 0x43b00100)
case('register-read-refused-before-startup-init', r == 0 and 'real_read' not in m.calls and not m.window)

m = Machine()
first = m.call(wrap['a733_npu_startup_init'], STARTUP, 1, 2, 0, 0)
recorded = m.get(startup_ptr)
second = m.call(wrap['a733_npu_startup_init'], STARTUP + 0x1000, 1, 2, 0, 0)
case('startup-init-records-one-sequencer', first == 1 and recorded == STARTUP and second == 0 and
     m.calls.count('a733_npu_startup_init') == 1 and m.get(startup_ptr) == STARTUP)

m = Machine()
m.prepared(STAGE['A733_NPU_STARTUP_FINAL'])
r = m.call(wrap['npu_service_activate'], 0x43c00000)
case('activation-before-ready-denied', r == SERVICE['NPU_SERVICE_DENIED'] and not m.reads and not m.writes and
     'npu_service_activate' not in m.calls)

m = Machine(sysctrl=0x0)
m.prepared(STAGE['A733_NPU_STARTUP_READY'])
r = m.call(wrap['npu_service_activate'], 0x43c00000)
case('activation-refuses-entry-not-routed-to-cpu', r == SERVICE['NPU_SERVICE_FAULT'] and not m.writes and
     m.get(routed_flag, 4) == 0 and 'npu_service_activate' not in m.calls)

m = Machine(sysctrl=0x2, stuck=True)
m.prepared(STAGE['A733_NPU_STARTUP_READY'])
r = m.call(wrap['npu_service_activate'], 0x43c00000)
case('activation-refuses-failed-readback', r == SERVICE['NPU_SERVICE_FAULT'] and m.writes == [0x0] and
     m.get(routed_flag, 4) == 0 and 'npu_service_activate' not in m.calls)

m = Machine(sysctrl=0x6, real_returns={'npu_service_activate': SERVICE['NPU_SERVICE_OK']})
m.prepared(STAGE['A733_NPU_STARTUP_READY'])
r = m.call(wrap['npu_service_activate'], 0x43c00000)
routed_writes = list(m.writes)
again = m.call(wrap['npu_service_activate'], 0x43c00000)
case('activation-routes-sram-to-npu-once-then-activates', r == SERVICE['NPU_SERVICE_OK'] and
     routed_writes == [0x4] and m.writes == [0x4] and m.sysctrl == 0x4 and m.get(routed_flag, 4) == 1 and
     m.calls.count('npu_service_activate') == 2 and again == SERVICE['NPU_SERVICE_OK'],
     sysctrl_entry='0x6', sysctrl_after='0x4', other_bits_preserved=True)

m.steps = 0
r = m.call(wrapped_read, 0x43b00000, 0x20, 0x43b00100)
read_steps = m.steps
w = m.call(wrapped_write, 0x43b00000, 0x0, 0x70900)
case('register-access-allowed-when-routed-and-ready', r == 1 and w == 1 and
     m.calls.count('real_read') == 1 and m.calls.count('real_write') == 1,
     guard_instructions_per_read=read_steps)

for stage in ('A733_NPU_STARTUP_FINAL', 'A733_NPU_STARTUP_STOPPED'):
    m = Machine(sysctrl=0x0)
    m.prepared(STAGE[stage], routed=1)
    r = m.call(wrapped_read, 0x43b00000, 0x20, 0x43b00100)
    w = m.call(wrapped_write, 0x43b00000, 0x0, 0x70900)
    case('register-access-refused-at-' + stage.split('_')[-1].lower(), r == 0 and w == 0 and
         'real_read' not in m.calls and 'real_write' not in m.calls and not m.window)

m = Machine(sysctrl=0x2)
m.prepared(STAGE['A733_NPU_STARTUP_READY'], routed=0)
r = m.call(wrapped_read, 0x43b00000, 0x20, 0x43b00100)
case('register-access-refused-when-ready-but-not-routed', r == 0 and 'real_read' not in m.calls)

for result in ('A733_NPU_PLATFORM_BUSY', 'A733_NPU_PLATFORM_RETAINED'):
    m = Machine(sysctrl=0x0, real_returns={'npu_convolution_stop_poll': PLATFORM[result]})
    m.prepared(STAGE['A733_NPU_STARTUP_READY'], routed=1)
    r = m.call(wrap['npu_convolution_stop_poll'], 0x43c00000)
    case('stop-' + result.split('_')[-1].lower() + '-passes-through-without-restore',
         r == PLATFORM[result] and not m.writes and m.get(routed_flag, 4) == 1)

m = Machine(sysctrl=0x4, real_returns={'npu_convolution_stop_poll': PLATFORM['A733_NPU_PLATFORM_READY']})
m.prepared(STAGE['A733_NPU_STARTUP_STOPPED'], routed=1)
r = m.call(wrap['npu_convolution_stop_poll'], 0x43c00000)
case('stop-ready-restores-cpu-routing', r == PLATFORM['A733_NPU_PLATFORM_READY'] and m.writes == [0x6] and
     m.sysctrl == 0x6 and m.get(routed_flag, 4) == 0, sysctrl_after='0x6')

m = Machine(sysctrl=0x4, stuck=True, real_returns={'npu_convolution_stop_poll': PLATFORM['A733_NPU_PLATFORM_READY']})
m.prepared(STAGE['A733_NPU_STARTUP_STOPPED'], routed=1)
r = m.call(wrap['npu_convolution_stop_poll'], 0x43c00000)
case('stop-failed-restore-retains', r == PLATFORM['A733_NPU_PLATFORM_RETAINED'] and m.writes == [0x6] and
     m.get(routed_flag, 4) == 0)

m = Machine(sysctrl=0x4, real_returns={'npu_convolution_stop_poll': PLATFORM['A733_NPU_PLATFORM_READY']})
m.prepared(STAGE['A733_NPU_STARTUP_STOPPED'], routed=0)
r = m.call(wrap['npu_convolution_stop_poll'], 0x43c00000)
case('stop-without-routing-writes-nothing', r == PLATFORM['A733_NPU_PLATFORM_READY'] and not m.writes)

for name, entry, stuck, writes, final in (('el3-return-restores-npu-routing', 0x4, False, [0x6], 'npu_finite_rom_return'),
                                          ('el3-return-keeps-cpu-routing', 0x2, False, [], 'npu_finite_rom_return'),
                                          ('el3-return-parks-on-failed-restore', 0x4, True, [0x6], 'a733_smp_stop')):
    m = Machine(sysctrl=entry, stuck=stuck)
    m.u.reg_write(UC_ARM64_REG_SP, STACK)
    m.u.reg_write(UC_ARM64_REG_LR, SENTINEL)
    m.u.emu_start(wrap['npu_finite_rom_return'], SENTINEL, count=10000)
    stop = (m.u.reg_read(UC_ARM64_REG_X0), m.u.reg_read(UC_ARM64_REG_X0 + 1)) if final == 'a733_smp_stop' else None
    case(name, m.writes == writes and m.calls == [final] and
         (stop is None or stop == (7, SYSCTRL)), sysctrl_entry=hex(entry))

# Linked redirection: every call of the five names from the unchanged objects
# reaches a wrapper, and the unchanged objects load the native callbacks from
# the wrapper table.
text = b''.join(d for base, d in im.parts if base == 0x41000000)
calls = {}
for i in range(0, len(text) - 3, 4):
    word = struct.unpack_from('<I', text, i)[0]
    if word >> 26 in (0x25, 0x05):
        target = 0x41000000 + i + ((((word & 0x3ffffff) ^ 0x2000000) - 0x2000000) << 2)
        calls.setdefault(target, []).append(0x41000000 + i)
inside = lambda at, name: any(lo <= at < lo + 0x200 for lo in [im.address('__wrap_' + name)])
for name in ('a733_npu_startup_init', 'npu_service_activate', 'npu_convolution_stop_poll', 'npu_finite_rom_return'):
    sites = calls.get(real[name], [])
    case('only-the-wrapper-reaches-' + name, sites and all(inside(s, name) for s in sites),
         sites=[hex(s) for s in sites])
case('native-callbacks-load-the-wrapper-table', wrapped_read != real_read and wrapped_write != real_write and
     ops_wrap != ops_real, wrapper_table=hex(ops_wrap), native_table=hex(ops_real))

# Shared SRAM audit of the image and of the finite launch regions.
segments = [(base, base + len(data)) for base, data in im.parts]
symbols = sorted({v for vs in im.symbols.values() for v in vs if SHARED[0] <= v < SHARED[1]})
smp = (root / 'tools/check-a733-smp.py').read_text()
regions = [(n, int(b, 16), int(s, 16) if s.startswith('0x') else int(s))
           for n, b, s in re.findall(r"\('(\w+)', (0x[0-9a-f]+), (0x[0-9a-f]+|\d+)\)", smp.split('REGIONS = ', 1)[1].split(']', 1)[0])]
case('no-image-segment-or-symbol-in-shared-sram',
     all(hi <= SHARED[0] or lo >= SHARED[1] for lo, hi in segments) and not symbols,
     segments=[[hex(lo), hex(hi)] for lo, hi in segments])
case('launcher-regions-below-shared-sram', len(regions) == 5 and all(b + s <= SHARED[0] for _, b, s in regions),
     regions={n: [hex(b), hex(b + s)] for n, b, s in regions})

out = {'schema': 'npu-sram-route-check-v1', 'physical_access': False,
       'image_sha256': hashlib.sha256(elf.read_bytes()).hexdigest(), 'cases': results,
       'scope': 'Linked wrapper instructions with modeled SYSCTRL and stubbed redirected functions; it does not '
                'run startup, the controller or the device, and it does not show what the NPU reads from its '
                'SRAM under either routing.'}
a.output.write_text(json.dumps(out, indent=1) + '\n')
print(f'PASS {len(results)} SRAM-routing cases')
