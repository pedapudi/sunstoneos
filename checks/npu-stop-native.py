#!/usr/bin/env python3
"""The linked NPU allowlist adapter: local stop writes, PLL rules, the shared SRAM routing bit and PCK600 refusals, with modeled MMIO."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_SP, UC_ARM64_REG_LR
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--elf',type=Path,required=True)
p.add_argument('--layout-helper',type=Path,default=Path(__file__).resolve().parent/'storage-read-native.py')
p.add_argument('--evidence',type=Path,required=True)
a=p.parse_args()
spec=importlib.util.spec_from_file_location('layout',a.layout_helper)
layout=importlib.util.module_from_spec(spec);spec.loader.exec_module(layout)
image=layout.Image(a.elf,layout_names=())
cases=[]
# A case may preset other words: the routing bit changes only while the NPU
# bus and reset word and the module gate hold the NPU in reset.
for name,address,before,after,ok,*preset in [
    ('module-gate-off',0x02002b00,0x80200001,0x00200001,1),
    ('bus-gate-off',0x02002b04,0xa00f0041,0xa00f0040,1),
    ('AHB-reset-assert',0x02002b04,0xa00f0040,0xa00b0040,1),
    ('AXI-reset-assert',0x02002b04,0xa00b0040,0xa0090040,1),
    ('core-reset-assert',0x02002b04,0xa0090040,0xa0080040,1),
    ('SRAM-reset-refused',0x02002b04,0xa0080040,0xa0000040,0),
    ('shared-bits-refused',0x02002b04,0xa0080040,0xa0080000,0),
    ('live-divider-refused',0x02002b00,0x80200001,0x80200003,0),
    ('power-off-refused',0x07064000,0x80000008,0x80000000,0),
    ('domain-on-refused',0x07064000,0x80000000,0x80000008,0),
    ('delay-row-refused',0x07064170,0,0x1f1f1f,0),
    ('PLL-write-refused',0x020022a0,0xf8105100,0,0),
    ('PLL-gate-control-enable',0x02003920,0,0x10000,1),
    ('PLL-output-before-lock-refused',0x020022a0,0xe0305100,0xe8305100,0),
    ('PLL-factors-852-with-output-closed',0x020022a0,0xf0305100,0xe0104600,1),
    ('PLL-factors-492-with-output-closed',0x020022a0,0xf0104600,0xe0305100,1),
    ('PLL-factors-852-with-output-open-refused',0x020022a0,0xf8305100,0xe8104600,0),
    ('PLL-other-factors-refused',0x020022a0,0xf0305100,0xe0105100,0),
    ('SRAM-route-to-NPU',0x03000008,0x2,0x0,1),
    ('SRAM-route-to-CPU',0x03000008,0x0,0x2,1),
    ('SRAM-routing-other-bit-refused',0x03000008,0x2,0x3,0),
    ('SRAM-routing-refused-with-a-reset-released',0x03000008,0x2,0x0,0,{0x02002b04:0x00090000}),
    ('SRAM-routing-refused-with-the-module-gate-open',0x03000008,0x0,0x2,0,{0x02002b00:0x80000000}),
]:
    u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    u.mem_map(0x02000000,0x06000000);u.mem_map(0x40000000,0x04000000)
    for start,data in image.parts:
        if data:u.mem_write(start,data)
    u.mem_write(address,struct.pack('<I',before))
    for at,value in (preset[0] if preset else {}).items():u.mem_write(at,struct.pack('<I',value))
    writes=[]
    u.hook_add(UC_HOOK_MEM_WRITE,lambda cpu,access,at,size,value,cookie:writes.append((at,size,value)),begin=0x02000000,end=0x07ffffff)
    for i,value in enumerate((0,address,after)):u.reg_write(UC_ARM64_REG_X0+i,value)
    u.reg_write(UC_ARM64_REG_SP,0x43ffe000);u.reg_write(UC_ARM64_REG_LR,0x43fff000)
    u.emu_start(image.address('a733_npu_platform_native_write'),0x43fff000,count=10000)
    assert u.reg_read(UC_ARM64_REG_X0)==ok,name
    assert writes==([(address,4,after)] if ok else []),(name,writes)
    assert int.from_bytes(u.mem_read(address,4),'little')==(after if ok else before),name
    cases.append({'name':name,'passed':True})
a.evidence.mkdir(parents=True,exist_ok=True)
(a.evidence/'native-stop-policy.json').write_text(json.dumps({'elf_sha256':hashlib.sha256(a.elf.read_bytes()).hexdigest(),'cases':cases,'scope':'Actual native adapter instructions; MMIO memory modeled. No physical register access, resource admission or bus drain established.'},indent=2)+'\n')
print(len(cases),'native adapter cases passed')
