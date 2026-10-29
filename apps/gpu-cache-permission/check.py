#!/usr/bin/env python3
"""Linked CPU client and production-helper cases; architectural effects are modeled."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import sys
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'checks'))
import harness
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PSTATE, UC_ARM64_REG_CPACR_EL1
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
spec = importlib.util.spec_from_file_location('permission_host', Path(__file__).with_name('host.py'))
host = importlib.util.module_from_spec(spec); spec.loader.exec_module(host)

def main():
    p = argparse.ArgumentParser(description=__doc__); p.add_argument('--bundle', type=Path, required=True); p.add_argument('--output', type=Path, required=True)
    a = p.parse_args(); record = host.validate_bundle(a.bundle); image = harness.Image(a.bundle / 'image.elf', layout_names=())
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    cases = []
    for variant in ('normal', 'writable', 'el0-denied', 'wrong-attributes', 'tag-not-cleared', 'sum-corrupted-after-cache'):
        u = Uc(UC_ARCH_ARM64, UC_MODE_ARM); u.mem_map(0x40000000, 0x8000000)
        for address, data in image.parts:
            if data: u.mem_write(address, data)
        u.reg_write(UC_ARM64_REG_SP, 0x41700000); u.reg_write(UC_ARM64_REG_LR, 0x41600000)
        u.reg_write(UC_ARM64_REG_PSTATE, 5); u.reg_write(UC_ARM64_REG_CPACR_EL1, 3 << 20)
        state = {'task': None, 'exit': None, 'dc': [], 'barriers': 0, 'initializing': True, 'stores': 0, 'loads': 0, 'cache_entered': False}
        regs = {'currentel': 4, 'sctlr_el1': 0x1005, 'ctr_el0': 0x04040004, 'mair_el1': 0x44ff00,
                'tcr_el1': 0x200803519, 'daif': 0x3c0, 'par_el1': 0, 'cntpct_el0': 100}
        l2 = image.address('level2', unit='gpu-cache-permission/platform.c')
        hooks = {image.address(n): n for n in ('platform_init', 'xTaskCreateStatic', 'vTaskStartScheduler', 'platform_exit', 'aarch64_device_cache')}
        def ret(v=0):
            u.reg_write(UC_ARM64_REG_X0, v); u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_LR))
        def memory(cpu, access, address, size, value, cookie):
            if not 0x41800000 <= address < 0x41c00000: return
            if access == 17:  # UC_MEM_WRITE
                assert state['initializing'], 'EL1 wrote the output'
                state['stores'] += size
            else: state['loads'] += size
        def code(cpu, address, size, cookie):
            if address == 0x41600000: u.emu_stop(); return
            name = hooks.get(address)
            args = [u.reg_read(UC_ARM64_REG_X0 + i) for i in range(5)]
            if name == 'platform_init':
                for i in range(2):
                    desc = 0x41800000 + i * 0x200000 | 0x00600000000007c5
                    if variant == 'writable': desc &= ~128
                    if variant == 'wrong-attributes': desc ^= 4
                    u.mem_write(l2 + (12 + i) * 8, struct.pack('<Q', desc))
                if variant == 'tag-not-cleared': u.mem_write(0x41800000, struct.pack('<Q', 99))
                ret(); return
            if name == 'xTaskCreateStatic': state['task'] = args[0]; ret(0x41701000); return
            if name == 'vTaskStartScheduler':
                assert state['task']; u.reg_write(UC_ARM64_REG_PC, state['task']); return
            if name == 'platform_exit': state['exit'] = args[0]; u.emu_stop(); return
            if name == 'aarch64_device_cache':
                assert args[:3] == [1, 0x41800000, 0x400000]
                state['cache_entered'] = True
            insn = next(decoder.disasm(bytes(u.mem_read(address, 4)), address))
            op = insn.op_str.lower().split(', ')
            def reg(token):
                return UC_ARM64_REG_X0 + int(token[1:])
            if insn.mnemonic == 'mrs':
                assert op[1] in regs, op
                if op[1] == 'cntpct_el0': regs[op[1]] += 100
                u.reg_write(reg(op[0]), regs[op[1]]); u.reg_write(UC_ARM64_REG_PC, address + 4)
            elif insn.mnemonic == 'msr':
                if op[0] in regs: regs[op[0]] = u.reg_read(reg(op[1]))
                else: assert op[0] == 'daifset', op
                u.reg_write(UC_ARM64_REG_PC, address + 4)
            elif insn.mnemonic == 'at':
                va = u.reg_read(reg(op[1])); assert va in (0x41800000, 0x41a00000)
                denied = op[0].endswith('w') or (variant == 'el0-denied' and op[0] == 's1e0r')
                regs['par_el1'] = 1 if denied else (0xff << 56) | va | 0x180
                u.reg_write(UC_ARM64_REG_PC, address + 4)
            elif insn.mnemonic == 'dc':
                assert op[0] == 'civac' and state['cache_entered']
                state['dc'].append(u.reg_read(reg(op[1]))); u.reg_write(UC_ARM64_REG_PC, address + 4)
                if variant == 'sum-corrupted-after-cache' and len(state['dc']) == 65536:
                    u.mem_write(0x41810000, b'\x00' * 8)
            elif insn.mnemonic == 'dsb' and state['cache_entered']: state['barriers'] += 1
        u.hook_add(UC_HOOK_CODE, code); u.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, memory)
        u.emu_start(image.address('gpu_cache_permission_initialize'), 0, count=150000)
        assert state['stores'] == 65728
        state['initializing'] = False
        u.reg_write(UC_ARM64_REG_LR, 0x41600000)
        u.emu_start(image.address('kernel_main'), 0, count=1500000)
        assert state['exit'] is not None
        if variant in ('normal', 'sum-corrupted-after-cache'):
            assert state['dc'] == list(range(0x41800000, 0x41c00000, 64)) and state['barriers'] == 2, (variant, state['exit'], len(state['dc']), state['barriers'], bytes(u.mem_read(0x42000100, 512)))
        else: assert not state['cache_entered']
        assert state['exit'] == (0 if variant == 'normal' else 1), variant
        if variant == 'normal':
            assert state['loads'] == 131456
            length = struct.unpack('<Q', u.mem_read(0x42000018, 8))[0]
            actual_log = bytes(u.mem_read(0x42000100, length)).decode()
            host.validate_result({'magic': 0x534b4552, 'stage': 5, 'status': state['exit']}, actual_log)
        cases.append({'case': variant, 'status': state['exit'], 'initialized_bytes': state['stores'], 'read_bytes': state['loads'], 'civac': len(state['dc'])})
    good = {'magic': 0x534b4552, 'stage': 5, 'status': 0}
    log = 'GPU output cache ticks=0x0000000000000010\n' + host.EXPECTED + '\n'
    host.validate_result(good, log)
    for terminal, text in [({**good, 'status': 1}, log), (good, log + host.EXPECTED + '\n'), (good, log + 'FAIL exception\n'), (good, host.EXPECTED)]:
        harness.refused(lambda: host.validate_result(terminal, text), 'permission', ValueError)
    packaged = a.bundle / 'host/tools/check-a733-kernel.py'
    spec = importlib.util.spec_from_file_location('packaged_permission', packaged)
    packaged_host = importlib.util.module_from_spec(spec); spec.loader.exec_module(packaged_host)
    for args in (['--mode', 'timeout'], ['--controlled', 'ignored'], ['--contr', 'ignored'],
                 ['--ram-input-p'], ['--ram-in=ignored'], ['--ram-input', 'ignored']):
        harness.refused(lambda: packaged_host.main(['--image', str(a.bundle / 'probe.bin')] + args), 'permission host mode', ValueError)
    a.output.write_text(json.dumps({'passed': True, 'physical_access': False, 'scope': 'Linked client, initialization stores, real helper loop; system registers, AT, cache effects and scheduler modeled', 'image_sha256': record['image_sha256'], 'cases': cases}, indent=2) + '\n')
    print('6 linked cases, 4 capture refusals and 6 packaged mode refusals passed')
if __name__ == '__main__': main()
