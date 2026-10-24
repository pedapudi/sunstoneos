#!/usr/bin/env python3
"""Observe the actual selected SRE task frame without running a device workload."""
import argparse,hashlib,importlib.util,json
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_SP,UC_ARM64_REG_LR
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True);a=p.parse_args();e=a.evidence
r=Path(__file__).resolve().parents[2];spec=importlib.util.spec_from_file_location('layout',r/'checks/storage-read-native.py');layout=importlib.util.module_from_spec(spec);spec.loader.exec_module(layout);im=layout.Image(e/'image.elf',layout_names=())
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x40000000,0x04000000)
for at,data in im.parts:
 if data:u.mem_write(at,data)
stack,entry,arg=0x43010000,0x41001234,0x43020000
for i,value in enumerate((stack,entry,arg)):u.reg_write(UC_ARM64_REG_X0+i,value)
u.reg_write(UC_ARM64_REG_SP,0x43ffe000);u.reg_write(UC_ARM64_REG_LR,0x43fff000);u.emu_start(im.address('pxPortInitialiseStack'),0x43fff000,count=100000)
pstate=int.from_bytes(u.mem_read(stack-33*8,8),'little');pc=int.from_bytes(u.mem_read(stack-34*8,8),'little')
assert pstate==4 and pc==entry
assert 'a733_smp_exit_privileged' not in im.symbols and 'a733_cached_smc' not in im.symbols
(e/'task-profile-check.json').write_text(json.dumps({'elf_sha256':hashlib.sha256((e/'image.elf').read_bytes()).hexdigest(),'initial_task_pstate':pstate,'initial_task_el':1,'initial_task_stack_selector':0,'cached_SVC119_exit_absent':True,'scope':'Actual linked SRE stack-constructor instructions; does not execute scheduling or qualify physical task context. SP_EL0 selection denotes EL1t here.'},indent=2)+'\n');print('PASS actual SRE task PSTATE: EL1t, SP_EL0; cached SVC119 exit absent')
