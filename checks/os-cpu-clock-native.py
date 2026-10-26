#!/usr/bin/env python3
"""Exercise actual compiled OS CPU register access policy and mux completion barrier.

Use a CPU-clock-enabled configured OS build. Register memory and CPU identity
are modeled; the OS adapter, clock allowlist and report decoder are actual code.
This check establishes neither physical MMIO access nor clock qualification.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex

from harness import Image, LLD, Steps
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_MEM_READ, UC_HOOK_INTR
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_LR, UC_ARM64_REG_PC

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--resume', action='store_true')
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=a.resume)
steps = Steps(a.output, 'commands.json')
u = next(u for u in json.loads((a.build / 'compile_commands.json').read_text())
         if u['file'].endswith('/apps/os/cpu.c'))
original = shlex.split(u['command'])
old_root = str(Path(u['file']).parents[2])
flags = [s.replace(old_root, str(ROOT)) for s in original[:original.index('-o')]]
assert '-DSUNSTONE_A733_OS_CPU_CLOCK=1' in flags and '-DA733_A76_ADMITTED=1' in flags
units = ('checks/os-cpu-clock-native.c', 'drivers/clock/a733_cpu_clock.c', 'runtime/memory.c')
objects = []
for i, unit in enumerate(units):
    obj = a.output / f'{i}.o'; objects.append(obj)
    inputs = (unit, 'apps/os/cpu.c', 'include/a733_cpu_clock.h', 'include/a733_mmio.h')
    stamp = hashlib.sha256(b''.join((ROOT / f).read_bytes() for f in inputs)).hexdigest()[:12]
    steps.command(f'compile-{i}-{stamp}', [*flags, '-c', ROOT / unit, '-o', obj])
elf = a.output / 'adapter.elf'
stamp = hashlib.sha256(b''.join(p.read_bytes() for p in objects)).hexdigest()[:12]
steps.command('link-' + stamp, [LLD, '-Ttext=0x41000000', '--entry=os_cpu_adapter_check', '--gc-sections', *objects, '-o', elf])
image = Image(elf)
m = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
m.mem_map(0x200000, 0x10000)
m.mem_map(0x41000000, 0x400000)
for addr, data in image.parts: m.mem_write(addr, data)
m.mem_map(0x42000000, 0x10000)
registers = {0x07090160: 0x10008002, 0x0300624c: 0x00000496,
             0x08871000: 0xf8802600, 0x0887101c: 0x03010000,
             0x08872000: 0xf8002700, 0x0887201c: 0x03000000,
             0x08873000: 0xf8001e00, 0x0887301c: 0x03000000}
for page in sorted({x & ~4095 for x in registers}): m.mem_map(page, 4096)
for addr, value in registers.items(): m.mem_write(addr, value.to_bytes(4, 'little'))
events = []
stop = 0x4200fff0


def hook(cpu, address, size, unused):
    if address == stop: cpu.emu_stop()
    elif int.from_bytes(cpu.mem_read(address, size), 'little') == 0xd5033f9f:
        events.append(('dsb-sy',))


def access(cpu, operation, address, size, value, unused):
    if address < 0x10000000:
        events.append(('read' if operation == UC_MEM_READ else 'write', address, size, value))


m.hook_add(UC_HOOK_CODE, hook)
m.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, access)


def call(mode, addr=0, value=0):
    events.clear()
    m.reg_write(UC_ARM64_REG_SP, 0x4200f000)
    m.reg_write(UC_ARM64_REG_LR, stop)
    for r, v in ((UC_ARM64_REG_X0, mode), (UC_ARM64_REG_X1, addr), (UC_ARM64_REG_X2, value)):
        m.reg_write(r, v)
    m.emu_start(image.address('os_cpu_adapter_check'), stop, count=100000)
    assert m.reg_read(UC_ARM64_REG_PC) == stop
    return m.reg_read(UC_ARM64_REG_X0)


cases = []
assert call(4) == 1 and len(events) == 8 and all(e[0] == 'read' and e[2] == 4 for e in events)
cases.append('actual-os-admission-report')
# CPU-B left at 2002 MHz by an earlier image of the lifetime: the FEL ROM
# return does not reset it, and the admission refuses it.
m.mem_write(0x08872000, (0xf8004d00).to_bytes(4, 'little'))
assert call(4) == 0
m.mem_write(0x08872000, registers[0x08872000].to_bytes(4, 'little'))
cases.append('admission-refuses-cpu-b-above-boot0-rate')
reads = [*registers, *[base + offset for base in (0x08871000, 0x08872000, 0x08873000) for offset in (4, 8, 0x18)]]
for addr in reads:
    assert call(0, addr) == 1 and len(events) == 1 and events[0][:3] == ('read', addr, 4)
cases.append('all-seventeen-allowed-reads')
for addr in (0x08871000, 0x08872000, 0x08873000, 0x0887101c, 0x0887201c, 0x0887301c):
    assert call(1, addr, registers[addr]) == 1
    expected = [('write', addr, 4, registers[addr])]
    if addr & 0xfff == 0x1c: expected.append(('dsb-sy',))
    assert events == expected, (hex(addr), events)
cases.append('cluster-and-dsu-writes-and-mux-dsb')
for addr in (0x08870000, 0x0887001c, 0x08871004, 0x08871008, 0x08871018, 0x08873018, 0x0300624c, 0x07090160):
    assert call(1, addr, 0) == 0 and not events
for mode, addr in ((0, 0x08870000), (0, 0x08871002), (1, 0x08871002), (2, 0x08871000), (3, 0x08871000)):
    assert call(mode, addr, 0) == 0 and not events
cases.append('out-of-policy-and-wrong-owner-refusal')
# Drive the actual owner through both inspections and both-cluster requests.
# The driver boundary models completion; its grant decoder/DSU table are real.
owner_obj = a.output / 'owner.o'
stamp = hashlib.sha256((ROOT / 'checks/os-cpu-clock-owner.c').read_bytes() +
                       (ROOT / 'apps/os/cpu.c').read_bytes()).hexdigest()[:12]
steps.command('compile-owner-' + stamp, [*flags, '-c', ROOT / 'checks/os-cpu-clock-owner.c', '-o', owner_obj])
policy_obj = a.output / 'policy.o'
steps.command('compile-owner-policy-' + hashlib.sha256((ROOT / 'drivers/thermal/a733_thermal.c').read_bytes()).hexdigest()[:12],
              [*flags, '-c', ROOT / 'drivers/thermal/a733_thermal.c', '-o', policy_obj])
owner_elf = a.output / 'owner.elf'
steps.command('link-owner-' + hashlib.sha256(owner_obj.read_bytes() + policy_obj.read_bytes()).hexdigest()[:12],
              [LLD, '-Ttext=0x41000000', '--entry=os_cpu_owner_check', '--gc-sections',
               owner_obj, policy_obj, *objects[1:], '-o', owner_elf])
owner_image = Image(owner_elf)
for mode, name in enumerate(('both-clusters-big-little-dsu', 'no-grant-read-only', 'retained-request-not-ready')):
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    cpu.mem_map(0x41000000, 0x400000)
    cpu.mem_map(0x200000, 0x10000)
    for address, data in owner_image.parts: cpu.mem_write(address, data)
    cpu.mem_map(0x42000000, 0x10000)
    cpu.reg_write(UC_ARM64_REG_SP, 0x4200f000);cpu.reg_write(UC_ARM64_REG_X0, mode)
    cpu.hook_add(UC_HOOK_INTR, lambda machine, interrupt, unused: machine.emu_stop())
    cpu.emu_start(owner_image.address('os_cpu_owner_check'), 0, count=100000)
    status = int.from_bytes(cpu.mem_read(owner_image.address('os_check_result'), 4), 'little')
    assert status == 1, (name, status)
    cases.append(name)
result = dict(status='PASS', cases=cases, elf_sha256=hashlib.sha256(elf.read_bytes()).hexdigest(),
    inputs={f: hashlib.sha256((ROOT / f).read_bytes()).hexdigest() for f in
            (*units, 'checks/os-cpu-clock-owner.c', 'checks/os-cpu-clock-native.py', 'apps/os/cpu.c', 'include/a733_cpu_clock.h', 'include/a733_mmio.h')},
    owner_elf_sha256=hashlib.sha256(owner_elf.read_bytes()).hexdigest(),
    limits='Actual AArch64 adapters with modeled MMIO storage; actual request owner with modeled driver completion. No physical clock, rail or thermal qualification.')
(a.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
print('PASS actual OS CPU adapter: report, CPU-B above boot0 refused, seventeen reads, six writes, mux DSB, refusals')
