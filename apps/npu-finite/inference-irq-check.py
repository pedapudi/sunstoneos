#!/usr/bin/env python3
"""Check the linked finite runtime binding with modeled GIC and scheduler state."""
import argparse
import hashlib
import json
import sys
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PSTATE

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--image', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'checks'))
from harness import Image
image = Image(a.image, layout_names=())
results = []
for name, scheduler, enabled, expected_calls in (
    ('before-scheduler', 1, 0, 1),
    ('running-scheduler-refused', 2, 0, 0),
    ('enabled-source-refused', 1, 2, 0),
):
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    u.reg_write(UC_ARM64_REG_PSTATE, 5)  # EL1h: bind_ready reads CurrentEL.
    u.mem_map(0x03400000, 0x10000)
    u.mem_map(0x40000000, 0x04000000)
    for address, data in image.parts:
        if data:
            u.mem_write(address, data)
    u.mem_write(0x0340010c, enabled.to_bytes(4, 'little'))
    u.mem_write(0x03400460, (0xb0 << 8).to_bytes(4, 'little'))
    calls = []
    stopped = []
    def ret(value):
        u.reg_write(UC_ARM64_REG_X0, value)
        u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_LR))
    def code(cpu, address, size, cookie):
        if address == 0x43fff000:
            stopped.append(True)
            u.emu_stop()
        elif address == image.address('xTaskGetSchedulerState'):
            ret(scheduler)
        elif address == image.address('npu_runtime_bind_irq'):
            args = [u.reg_read(UC_ARM64_REG_X0 + i) for i in range(5)]
            calls.append(args)
            ret(1)
    u.hook_add(UC_HOOK_CODE, code)
    u.reg_write(UC_ARM64_REG_X0, 0x43800000)
    u.reg_write(UC_ARM64_REG_SP, 0x43ffe000)
    u.reg_write(UC_ARM64_REG_LR, 0x43fff000)
    u.emu_start(image.address('npu_finite_runtime_irq_bind'), 0x43fff000, count=10000)
    # emu_start may stop at its end address before executing the sentinel hook.
    returned = u.reg_read(UC_ARM64_REG_PC) == 0x43fff000
    result = u.reg_read(UC_ARM64_REG_X0)
    passed = returned and len(calls) == expected_calls and result == expected_calls
    if calls:
        passed = passed and calls[0][0] == 0x43800000 and calls[0][3:] == [1, 0]
    results.append(dict(name=name, passed=bool(passed), returned=returned,
                        result=result, bind_arguments=calls))
report = dict(image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),
              physical_access=False, cases=results, passed=all(r['passed'] for r in results),
              scope='Actual finite IRQ binding instructions; GIC registers, scheduler and runtime bind callback modeled. Does not establish interrupt delivery.')
a.output.parent.mkdir(parents=True, exist_ok=True)
with a.output.open('x') as stream:
    json.dump(report, stream, indent=2)
    stream.write('\n')
print(json.dumps(report, indent=2))
raise SystemExit(0 if report['passed'] else 1)
