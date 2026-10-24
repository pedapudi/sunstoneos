#!/usr/bin/env python3
"""Exercise the real radio clock-write callback with modeled RTOS state and MMIO.

The actual AArch64 owner-context predicate and address/value filters execute.
No device is accessed. --baseline reproduces the old adapter's wide-clock refusal
using a preserved source file passed with --native-source.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_HOOK_CODE
from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
                                UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_PC,
                                UC_ARM64_REG_PSTATE)

ROOT = Path(__file__).resolve().parents[1]
MODULE = 0x02002d10
RETURN = 0x410f0000


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--sources', type=Path, default=Path.home()/'.cache/sunstoneos/sources')
    p.add_argument('--clang', default='/usr/bin/clang-21')
    p.add_argument('--lld', default=str(Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld'))
    p.add_argument('--native-source', type=Path, default=ROOT/'drivers/net/a733_radio_platform_native.c')
    p.add_argument('--baseline', action='store_true')
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    kernel = a.sources/'FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2'
    image = a.output/'radio-native-clock.elf'
    dep = a.output/'radio-native-clock.d'
    command = [a.clang, '--target=aarch64-none-elf', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
               '-ffreestanding', '-fno-builtin', '-ffunction-sections', '-fdata-sections',
               '-I'+str(ROOT/'include'), '-I'+str(ROOT/'runtime/include'),
               '-I'+str(kernel/'include'), '-I'+str(kernel/'portable/GCC/ARM_AARCH64_SRE'),
               '-DINCLUDE_xSemaphoreGetMutexHolder=1', '-DINCLUDE_xTaskGetSchedulerState=1',
               '-DINCLUDE_xTaskGetCurrentTaskHandle=1', '-DRADIO_NATIVE_SOURCE="'+str(a.native_source.resolve())+'"',
               '-MD', '-MF', str(dep), '-nostdlib', '--ld-path='+a.lld, '-Wl,--gc-sections',
               '-Wl,-e,radio_native_clock_write', '-Wl,-Ttext=0x41000000',
               str(ROOT/'checks/radio-native-clock.c'), '-o', str(image)]
    (a.output/'command.json').write_text(json.dumps(command, indent=2)+'\n')
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (a.output/'build.log').write_text(result.stdout)
    if result.returncode:
        raise SystemExit(result.stdout)
    with image.open('rb') as f:
        elf = ELFFile(f)
        segments = [(s['p_vaddr'], s.data()) for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
        symbols = {s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    cpu.mem_map(0x200000, 0x1000)
    cpu.mem_map(0x40ff0000, 0x200000)
    cpu.mem_map(0x50000000, 0x10000)
    cpu.mem_map(0x02002000, 0x2000)
    for address, data in segments:
        cpu.mem_write(address, data)
    accesses = []
    instructions = []
    cpu.hook_add(UC_HOOK_MEM_READ, lambda c,k,addr,size,v,u:
                 accesses.append(['read',addr,size]) if 0x02002000<=addr<0x02004000 else None)
    cpu.hook_add(UC_HOOK_MEM_WRITE, lambda c,k,addr,size,v,u:
                 accesses.append(['write',addr,size,v]) if 0x02002000<=addr<0x02004000 else None)
    cpu.hook_add(UC_HOOK_CODE, lambda c,addr,size,u: instructions.append(bytes(c.mem_read(addr,size)).hex()))
    cases = []

    def case(name, value, expected, old=0x8000001d, address=MODULE, flags=0, pstate=5):
        cpu.mem_write(address, old.to_bytes(4,'little'))
        accesses.clear()
        instructions.clear()
        for register, val in ((UC_ARM64_REG_X0,address),(UC_ARM64_REG_X1,value),(UC_ARM64_REG_X2,flags),
                              (UC_ARM64_REG_SP,0x5000f000),(UC_ARM64_REG_LR,RETURN),(UC_ARM64_REG_PSTATE,pstate)):
            cpu.reg_write(register,val)
        cpu.emu_start(symbols['radio_native_clock_write'], RETURN, count=2000)
        assert cpu.reg_read(UC_ARM64_REG_PC)==RETURN, name
        got = cpu.reg_read(UC_ARM64_REG_X0)&0xffffffff
        writes = [v for v in accesses if v[0]=='write']
        assert got==expected and len(writes)==expected, (name,got,accesses)
        assert int.from_bytes(cpu.mem_read(address,4),'little')==(value if expected else old), name
        if expected:
            assert accesses==[['read',address,4],['write',address,4,value]], (name,accesses)
        cases.append(dict(name=name,accepted=got,accesses=list(accesses),instructions=len(instructions),
                          dsb_sy=instructions.count('9f3f03d5')))

    # Exact production host_module sequence, with each accepted value fed into
    # the next step. Gate changes and a preserved non-clock bit are exercised.
    old = 0x8000001d | (1<<20)
    for name, value in [('gate-off',0x1d),('switch-parent',0x01000007),('gate-on',0x81000007)]:
        value |= 1<<20
        expected = int(not a.baseline or name=='gate-off')
        case(name,value,expected,old)
        if expected:
            old=value
    for value in (0x1d,0x8000001d,0x01000007,0x81000007):
        case('exact-'+hex(value),value,int(not a.baseline or (value&0x7fffffff)==0x1d))
    for value in (0,0x8000001c,0x81000008,0x82000007,0x81000107,0x89000007):
        case('unsupported-'+hex(value),value,0)
    case('unowned-bit-changed',0x8010001d,0)
    case('bus-gate-reset',0x10001,1,old=0,address=MODULE+12)
    case('bus-unowned-bit',0x10003,0,old=0,address=MODULE+12)
    case('parent-write',0xfe006300,0,old=0,address=0x020020a0)
    for flag, name in ((1,'wrong-task'),(2,'unowned-lock'),(4,'retained-lock'),(8,'interrupt'),
                       (16,'critical-section'),(32,'scheduler-stopped')):
        case(name,0x8000001d,0,flags=flag)
        assert not cases[-1]['accesses'], name
    case('masked-irq',0x8000001d,0,pstate=0x85)
    assert not cases[-1]['accesses']
    paths = {Path(v) for v in dep.read_text().replace('\\\n',' ').split(':',1)[1].split()}
    paths.update((Path(__file__),Path(a.clang).resolve(),Path(a.lld).resolve()))
    records = []
    (a.output/'inputs').mkdir()
    for path in sorted(paths):
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        (a.output/'inputs'/digest).write_bytes(data)
        records.append(dict(path=str(path),sha256=digest))
    (a.output/'inputs.json').write_text(json.dumps(records,indent=2)+'\n')
    (a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,
        baseline=a.baseline,image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),cases=cases),indent=2)+'\n')
    print(f'PASS {len(cases)} native radio clock policy cases; actual AArch64 callback, modeled RTOS/MMIO')


if __name__=='__main__':
    main()
