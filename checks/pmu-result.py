#!/usr/bin/env python3
"""Check PMU setup, chained snapshots and returning-result decoder delegation."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'checks'))
import harness
GATE_RUNS = [['--output', '{output}']]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build(out):
    (out / 'FreeRTOS.h').write_text('''#ifndef TEST_FREERTOS_H
#define TEST_FREERTOS_H
#include <stddef.h>
#include <stdint.h>
typedef uint64_t StackType_t;
typedef struct { uint64_t word; } StaticTask_t;
void *pvPortMalloc(size_t);
unsigned test_core(void);
#define portGET_CORE_ID() test_core()
#endif
''')
    (out / 'task.h').write_text('void vTaskDelay(unsigned);\n')
    (out / 'stubs.c').write_text('''#include "FreeRTOS.h"
#include "sunstone_app.h"
#include "application.h"
unsigned mock_core = 2;
unsigned test_core(void) { return mock_core; }
static struct ss_grant grant = {.main_core = 2};
const struct ss_grant *ss_grant(void) { return &grant; }
static struct ss_manifest manifest = {.output_limit = 64};
const struct ss_application ss_application = {.manifest = &manifest};
const size_t ss_result_capacity = 64;
void __real_vApplicationIRQHandler(uint64_t id) { (void)id; }
void __real_ss_activity_begin(void) {}
void __real_ss_activity_end(int success) { (void)success; }
int ss_live(void) { return 1; }
void ss_progress(void) {}
void vTaskDelay(unsigned ticks) { (void)ticks; }
void *pvPortMalloc(size_t n) { (void)n; return (void *)0x41500000; }
_Noreturn void ss_fail(unsigned detail) { (void)detail; for (;;) {} }
_Noreturn void __real_ss_finish(const void *data, size_t n) { (void)data; (void)n; for (;;) {} }
void *memcpy(void *d, const void *s, size_t n) { char *p=d; const char *q=s; while (n--) *p++=*q++; return d; }
''')
    (out / 'link.ld').write_text('''ENTRY(__wrap_ss_activity_begin)
SECTIONS { . = 0x41000000; .text : { *(.text*) } .rodata : { *(.rodata*) }
 . = 0x41200000; .data : { *(.data*) } .bss : { *(.bss*) *(COMMON) }
 /DISCARD/ : { *(.comment) *(.note*) } }
''')
    flags = ['clang-21', '--target=aarch64-none-elf', '-march=armv8-a', '-O2', '-g', '-ffreestanding',
             '-fno-builtin', '-fno-stack-protector', '-mstrict-align', '-mgeneral-regs-only',
             '-Wall', '-Wextra', '-Werror', '-DSUNSTONE_PMU_ACCOUNT=1', '-DSUNSTONE_PMU_RESULT=1',
             '-I' + str(out), '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'apps/os'),
             '-I' + str(ROOT / 'runtime/include')]
    objects = []
    for name, path in [('irq', ROOT / 'runtime/irq_count.c'), ('result', ROOT / 'runtime/pmu_result.c'), ('stubs', out / 'stubs.c')]:
        obj = out / (name + '.o')
        command = flags + ['-c', str(path), '-o', str(obj)]
        result = subprocess.run(command, capture_output=True, text=True)
        (out / (name + '.log')).write_text(result.stdout + result.stderr)
        result.check_returncode()
        objects.append(str(obj))
    elf = out / 'pmu-result.elf'
    result = subprocess.run([str(harness.LLD), '-T', str(out / 'link.ld'), *objects, '-o', str(elf)], capture_output=True, text=True)
    (out / 'link.log').write_text(result.stdout + result.stderr)
    result.check_returncode()
    return harness.Image(elf, layout_names=())


class Machine:
    def __init__(self, image, variant='normal'):
        self.u = harness.machine()
        self.u.mem_map(0x41000000, 0x800000)
        for address, data in image.parts:
            self.u.mem_write(address, data)
        self.image, self.variant = image, variant
        self.regs = dict(tpidr_el1=2, pmcr_el0=(6 << 11), pmceid0_el0=(1 << 3) | (1 << 8) | (1 << 23) | (1 << 30),
                         pmceid1_el0=(1 << 4) | (1 << 20), pmcntenset_el0=0, pmintenset_el1=0,
                         pmccfiltr_el0=0, pmuserenr_el0=0)
        self.samples = dict(pmevcntr5_el0=[2, 2, 3, 3], pmevcntr4_el0=[0xfffffff0, 0x20],
                            pmccntr_el0=[100, 1100], pmevcntr3_el0=[0xfffffffe, 3], cntpct_el0=[100, 124])
        if variant == 'carry':
            self.samples.update(pmevcntr5_el0=[1, 2, 2, 2, 2], pmevcntr4_el0=[0xfffffff0, 0x10, 0x50])
        if variant == 'unstable':
            self.samples.update(pmevcntr5_el0=[1, 2, 3], pmevcntr4_el0=[0xfffffff0, 0x10])
        if variant == 'few-counters':
            self.regs['pmcr_el0'] = 5 << 11
        if variant == 'no-chain':
            self.regs['pmceid0_el0'] &= ~(1 << 30)
        self.decoder, self.instructions = Cs(CS_ARCH_ARM64, CS_MODE_ARM), {}
        self.writes, self.failed, self.result, self.allocations, self.barriers = [], None, None, [], 0
        self.hooks = {image.address(n): n for n in ('ss_fail', '__real_ss_finish', 'pvPortMalloc')}
        self.u.hook_add(UC_HOOK_CODE, self.code)
        self.u.hook_add(UC_HOOK_MEM_WRITE, self.memory)

    def memory(self, u, access, address, size, value, cookie):
        ready = self.image.address('pmu_done', unit='runtime/irq_count.c')
        if ready <= address < ready + 32 and value == 1:
            assert self.barriers, 'readiness published before ISB'
            assert [self.regs.get(f'pmevtyper{i}_el0') for i in range(6)] == [3, 0x17, 0x24, 0x34, 8, 0x1e]
            assert self.regs.get('pmcntenset_el0', 0) & 0x8000003f == 0x8000003f

    def skip(self, address):
        self.u.reg_write(UC_ARM64_REG_PC, address + 4)

    def code(self, u, address, size, cookie):
        if address == 0x417ff000:
            u.emu_stop()
            return
        name = self.hooks.get(address)
        if name == 'ss_fail':
            self.failed = u.reg_read(UC_ARM64_REG_X0)
            u.emu_stop()
            return
        if name == '__real_ss_finish':
            data, length = (u.reg_read(UC_ARM64_REG_X0 + i) for i in range(2))
            self.result = bytes(u.mem_read(data, length))
            u.emu_stop()
            return
        if name == 'pvPortMalloc':
            self.allocations.append(u.reg_read(UC_ARM64_REG_X0))
            u.reg_write(UC_ARM64_REG_X0, 0 if self.variant == 'no-memory' else 0x41500000)
            u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_LR))
            return
        if address not in self.instructions:
            self.instructions[address] = next(self.decoder.disasm(bytes(u.mem_read(address, 4)), address))
        op = self.instructions[address]
        args = op.op_str.lower().split(', ')
        if op.mnemonic == 'mrs':
            dest, reg = args
            if reg in self.samples:
                assert self.samples[reg], reg
                value = self.samples[reg].pop(0)
            else:
                value = self.regs.get(reg, 0)
                if self.variant == 'readback' and reg == 'pmevtyper4_el0':
                    value ^= 1
            u.reg_write(UC_ARM64_REG_X0 + int(dest[1:]), value)
            self.skip(address)
        elif op.mnemonic == 'msr':
            reg, source = args
            value = 0 if source == 'xzr' else u.reg_read(UC_ARM64_REG_X0 + int(source[1:]))
            self.writes.append((reg, value))
            if reg == 'pmintenclr_el1':
                self.regs['pmintenset_el1'] &= ~value
            else:
                self.regs[reg] = value
            self.skip(address)
        elif op.mnemonic == 'isb':
            self.barriers += 1
            self.skip(address)

    def call(self, name, *args):
        self.u.reg_write(UC_ARM64_REG_SP, 0x41700000)
        self.u.reg_write(UC_ARM64_REG_LR, 0x417ff000)
        for i, value in enumerate(args):
            self.u.reg_write(UC_ARM64_REG_X0 + i, value)
        self.u.emu_start(self.image.address(name), 0, count=100000)
        return self.u.reg_read(UC_ARM64_REG_X0)


def native_cases(image):
    cases = []
    for variant, instructions in (('normal', 48), ('carry', 64)):
        m = Machine(image, variant)
        assert m.call('ulSunstonePmuReady', 2) == 0
        m.call('__wrap_vApplicationIRQHandler', 30)
        assert m.call('ulSunstonePmuReady', 2) == 1 and m.barriers
        writes = len(m.writes)
        m.call('__wrap_vApplicationIRQHandler', 30)
        assert len(m.writes) == writes, 'ready PMU reconfigured'
        assert [m.regs[f'pmevtyper{i}_el0'] for i in range(6)] == [3, 0x17, 0x24, 0x34, 8, 0x1e]
        m.call('__wrap_ss_activity_begin')
        m.u.mem_write(0x41510000, bytes(range(16)))
        m.call('__wrap_ss_finish', 0x41510000, 16)
        assert m.failed is None and m.allocations == [64]
        magic, core, cycles, retired, walks, ticks = struct.unpack_from('<6Q', m.result)
        assert (magic, core, cycles, retired, walks, ticks) == (0x313052554d505353, 2, 1000, instructions, 5, 24)
        assert m.result[48:] == bytes(range(16))
        cases.append(variant + '-snapshot-and-exact-output')
    for variant in ('few-counters', 'no-chain', 'readback'):
        m = Machine(image, variant)
        m.call('__wrap_vApplicationIRQHandler', 30)
        assert m.call('ulSunstonePmuReady', 2) == 0xffffffffffffffff
        writes = len(m.writes)
        m.call('__wrap_vApplicationIRQHandler', 30)
        assert len(m.writes) == writes
        m.call('__wrap_ss_activity_begin')
        assert m.failed == 0x7b02
        cases.append(variant + '-refused-before-snapshot')
    for variant, expected in (('unstable', 0x7b04), ('no-memory', 0x7b07), ('capacity', 0x7b06), ('null-data', 0x7b06)):
        m = Machine(image, variant)
        m.call('__wrap_vApplicationIRQHandler', 30)
        m.call('__wrap_ss_activity_begin')
        if m.failed is None:
            m.call('__wrap_ss_finish', 0 if variant == 'null-data' else 0x41510000, 17 if variant == 'capacity' else 16)
        assert m.failed == expected and m.result is None
        if variant in ('capacity', 'null-data'):
            assert not m.allocations
        cases.append(variant + '-refused')
    return cases


def decoder_cases(out):
    tool = load('pmu_result_tool', ROOT / 'tools/pmu-result.py')
    decoder = out / 'delegate.py'
    decoder.write_text('''def decode(payload):
    return {'bytes': len(payload), 'matches_reference': payload == b'correct'}
def failures(result):
    return []
''')
    frozen = out / 'frozen.py'
    tool.freeze(decoder, frozen)
    module = load('frozen_pmu_result', frozen)
    raw = struct.pack('<6Q', tool.MAGIC, 6, 1000, (1 << 32) + 15, 9, 24) + b'correct'
    result = module.decode(raw)
    assert result['pmu']['inst_retired'] == (1 << 32) + 15 and result['pmu']['dtlb_walk_mod32'] == 9
    assert result['bytes'] == 7 and not module.failures(result)
    assert module.failures(module.decode(raw[:-7] + b'wrong'))
    for offset in (0, 8, 16, 24, 40):
        bad = bytearray(raw)
        struct.pack_into('<Q', bad, offset, 0 if offset != 8 else 7)
        harness.refused(lambda: module.decode(bytes(bad)), 'PMU result', ValueError)
    harness.refused(lambda: module.decode(raw[:47]), 'prefix', ValueError)
    return 8


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    image = build(args.output)
    cases = native_cases(image)
    decoded = decoder_cases(args.output)
    harness.save(args.output / 'result.json', dict(passed=True, physical_access=False, native_cases=cases,
        decoder_cases=decoded, scope='Instruction execution with modeled PMU registers and FreeRTOS boundaries; no physical counter qualification.'))
    print('PASS PMU initialization, bounded chained snapshots, result bounds and frozen decoder')


if __name__ == '__main__':
    main()
