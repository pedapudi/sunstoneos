#!/usr/bin/env python3
"""Execute the native maintenance-watchdog admission and reload comparison.

Usage: audit-security-watchdog-rtc.py --object maintenance_watchdog.c.obj --output DIR

The compiled platform object runs under Unicorn with modeled watchdog, CCU and
RTC words. FreeRTOS, translation, counter and register-mutex calls are stubs.
The check covers only the RTC control comparison: calendar write-busy bits 8:7
may change during admission or a reload, while any other control bit change
still revokes the lease. No timer restart, reset or physical timing claim.
"""
import argparse
import json
import struct
import subprocess
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_SP, UC_ARM64_REG_LR,
                                 UC_ARM64_REG_PC, UC_ARM64_REG_PSTATE)

LLVM = Path('/usr/lib/llvm-21/bin')
LLD = Path.home() / '.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld'
STUBS = r'''
#include <stdint.h>
struct aarch64_translation {uint64_t physical;unsigned attributes,shareability;};
volatile uint64_t ullPortInterruptNestings[8],ullCriticalNestings[8];
volatile long scheduler_state;volatile uint64_t clock_us;volatile unsigned retained,entered;
long xTaskGetSchedulerState(void){return scheduler_state;}
void *xTaskGetCurrentTaskHandle(void){return (void *)0x4000f000;}
int aarch64_device_translate(uintptr_t a,int w,struct aarch64_translation *t)
{(void)w;t->physical=a;t->attributes=0;t->shareability=2;return 1;}
int a733_counter_clock_now(void *c,uint64_t *out){(void)c;*out=clock_us+=1;return 1;}
int a733_platform_lock_enter(void *l,int lock,uint64_t d){(void)l;(void)lock;(void)d;entered=1;return 1;}
void a733_platform_lock_leave(void *l,int lock){(void)l;(void)lock;entered=0;}
int a733_platform_lock_retain(void *l,int lock){(void)l;(void)lock;retained=1;return 1;}
'''
CODE, DATA, STOP = 0x40000000, 0x40100000, 0x40200000
CONTROL = 0x00004011


def build(obj, out):
    (out / 'stubs.c').write_text(STUBS)
    subprocess.check_call([LLVM / 'clang', '--target=aarch64-none-elf', '-O1', '-ffreestanding',
                           '-mgeneral-regs-only', '-c', out / 'stubs.c', '-o', out / 'stubs.o'])
    subprocess.check_call([LLD, '-static', '--no-dynamic-linker', '-e', '0',
                           f'--section-start=.text={CODE:#x}', f'-Tdata={DATA:#x}', f'-Tbss={DATA + 0x8000:#x}',
                           obj, out / 'stubs.o', '-o', out / 'harness.elf'])
    subprocess.check_call([LLVM / 'llvm-objcopy', '-O', 'binary', '--only-section=.text',
                           out / 'harness.elf', out / 'text.bin'])
    symbols = {}
    for line in subprocess.check_output([LLVM / 'llvm-nm', out / 'harness.elf'], text=True).splitlines():
        parts = line.split()
        if len(parts) == 3:
            symbols[parts[2]] = int(parts[0], 16)
    return (out / 'text.bin').read_bytes(), symbols


class Machine:
    def __init__(self, text, symbols, kick=None):
        self.symbols = symbols
        self.kick = kick
        self.u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.u.mem_map(CODE, 0x00300000)
        self.u.mem_write(CODE, text)
        for page in (0x02050000, 0x03000000, 0x07050000, 0x07090000):
            self.u.mem_map(page, 0x1000)
        self.u.reg_write(UC_ARM64_REG_PSTATE, 5)
        words = {0x02050010: 1, 0x02050014: 0x41, 0x02050018: 2, 0x03000024: 0,
                 0x070501c0: 0xfa50392f, 0x070501c4: 0x20, 0x070501d4: 0,
                 0x07090108: 0x5aa5a55a, 0x07090000: CONTROL, 0x07090060: 0x1f, 0x07090160: 0x10000003}
        for address, value in words.items():
            self.put(address, value)
        self.kicks = 0
        # Emulate MRS CurrentEL as EL1; Unicorn models the other registers.
        for at in range(CODE, CODE + len(text), 4):
            if (int.from_bytes(text[at - CODE:at - CODE + 4], 'little') & 0xffffffe0) == 0xd5384240:
                self.u.hook_add(UC_HOOK_CODE, self.current_el, begin=at, end=at)
        self.u.hook_add(UC_HOOK_MEM_WRITE, self.write, begin=0x02050000, end=0x02050fff)

    def put(self, address, value, size=4):
        self.u.mem_write(address, int(value).to_bytes(size, 'little'))

    def get(self, address, size=4):
        return int.from_bytes(self.u.mem_read(address, size), 'little')

    def current_el(self, u, at, size, unused):
        word = int.from_bytes(u.mem_read(at, 4), 'little')
        u.reg_write(UC_ARM64_REG_X0 + (word & 31), 4)
        u.reg_write(UC_ARM64_REG_PC, at + 4)

    def write(self, u, access, address, size, value, unused):
        if address == 0x0205000c:
            assert value == 0x14af
            self.kicks += 1
            if self.kick:
                self.put(0x07090000, self.kick)

    def call(self, name, *args):
        self.u.reg_write(UC_ARM64_REG_SP, DATA + 0x20000)
        self.u.reg_write(UC_ARM64_REG_LR, STOP)
        for i, value in enumerate(args):
            self.u.reg_write(UC_ARM64_REG_X0 + i, value)
        self.u.emu_start(self.symbols[name], STOP, count=200000)
        assert self.u.reg_read(UC_ARM64_REG_PC) == STOP, name
        return self.u.reg_read(UC_ARM64_REG_X0) & 0xffffffff

    def window(self, control=CONTROL):
        at = DATA + 0x10000
        self.u.mem_write(at, struct.pack('<QQIII', 10_000_000, 1000, control, 0x1f, 0x10000003))
        return at

    def admit(self, control=CONTROL):
        self.put(self.symbols['scheduler_state'], 1, 8)
        return self.call('a733_watchdog_hw_admit', self.window(control))

    def reload(self):
        self.put(self.symbols['scheduler_state'], 2, 8)
        state, before, after = DATA + 0x11000, DATA + 0x11008, DATA + 0x11010
        self.put(state, 1)
        ok = self.call('a733_watchdog_hw_reload', self.window(), DATA + 0x12000, DATA + 0x13000,
                       0x4000f000, state, 5_000_000, before, after)
        return ok, self.get(state), self.get(self.symbols['retained'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--object', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    a = parser.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    text, symbols = build(a.object.resolve(), a.output)
    cases = {}
    for busy in (0, 0x80, 0x100, 0x180):
        m = Machine(text, symbols)
        m.put(0x07090000, CONTROL | busy)
        assert m.admit() == 1, busy
        assert Machine(text, symbols).admit(CONTROL | busy) == 1, busy
    cases['admission ignores busy bits 8:7 in either word'] = True
    m = Machine(text, symbols)
    m.put(0x07090000, CONTROL ^ 1)
    assert m.admit() == 0
    m = Machine(text, symbols)
    m.put(0x07090000, CONTROL | 0x200)
    assert m.admit() == 0
    cases['admission refuses source and other control bit changes'] = True
    for busy in (0x80, 0x100):
        m = Machine(text, symbols, kick=CONTROL | busy)
        assert m.reload() == (1, 1, 0) and m.kicks == 1, busy
    cases['calendar busy during reload keeps the lease'] = True
    m = Machine(text, symbols, kick=CONTROL ^ 1)
    ok, state, retained = m.reload()
    assert ok == 0 and state & 4 and retained == 1 and m.kicks == 1
    cases['source bit change after the kick revokes and retains the mutex'] = True
    (a.output / 'result.json').write_text(json.dumps(cases, indent=2) + '\n')
    print(f'PASS {len(cases)} native watchdog RTC comparison cases; MMIO, RTOS and timing modeled')


if __name__ == '__main__':
    main()
