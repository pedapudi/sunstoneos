#!/usr/bin/env python3
from pathlib import Path
import argparse,json,hashlib,struct
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_HOOK_CODE
from unicorn.arm_const import *
parser=argparse.ArgumentParser(description='Check the pinned boot0 monitor-only parser and handoff without hardware.')
parser.add_argument('--boot0',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
OUT=args.output
OUT.mkdir(parents=True,exist_ok=True)
blob=args.boot0
b=blob.read_bytes();digest=hashlib.sha256(b).hexdigest()
assert digest=='829ea8e3b65d71a02f2d0e094bc073de3a10869f948f62db49dbf023d07fabe3'
uc=Uc(UC_ARCH_ARM,UC_MODE_THUMB)
for addr,size in [(0x40000,0x50000),(0x90000,0x1000),(0x42e00000,0x10000),(0x47000000,0x10000),(0x08001000,0x1000)]:uc.mem_map(addr,size)
uc.mem_write(0x47000,b)
# A synthetic plaintext container is already in RAM. This does not execute
# storage, checksum admission, DRAM training, clock changes or the payload.
package=bytearray(2048);package[:14]=b'sunxi-package\0'
struct.pack_into('<I',package,0x20,1)
package[0x40:0x48]=b'monitor\0'
struct.pack_into('<6I',package,0x80,1024,512,0,3,0x46000000,0)
payload=bytearray([0x5a]*512);payload[4:12]=b'monitor\0';struct.pack_into('<I',payload,0x2c,0x47000000)
package[1024:1536]=payload;uc.mem_write(0x42e00000,bytes(package))
dram=bytes(range(128));identity=bytes(range(0xa0,0xb0))
uc.mem_write(0x831f8,dram);uc.mem_write(0x470f0,b'BOOTDATA');uc.mem_write(0x83c20,struct.pack('<I',12288));uc.mem_write(0x89100,identity)
sp=0x88000;outputs=0x89000
for i,reg in enumerate((UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)):uc.reg_write(reg,outputs+4*i)
uc.mem_write(sp,struct.pack('<3I',outputs+16,outputs+20,outputs+24));uc.reg_write(UC_ARM_REG_SP,sp);uc.reg_write(UC_ARM_REG_LR,0x90001)
boundaries=[];phase='parser';rmr_before=0x100;rmr_after=None;stopped=False

def hook(u,address,size,data):
 global rmr_after,stopped
 if address==0x90000:
  stopped=True;u.emu_stop();return
 if address in (0x49066,0x49074,0x4993c):
  boundaries.append(hex(address));u.reg_write(UC_ARM_REG_R0,0);u.reg_write(UC_ARM_REG_PC,u.reg_read(UC_ARM_REG_LR));return
 if address==0x4af58:
  boundaries.append(hex(address));u.reg_write(UC_ARM_REG_R0,0x89100);u.reg_write(UC_ARM_REG_PC,u.reg_read(UC_ARM_REG_LR));return
 if address==0x48ef0:
  boundaries.append('RMR read');u.reg_write(UC_ARM_REG_R2,rmr_before);u.reg_write(UC_ARM_REG_PC,address+4|1);return
 if address==0x48efc:
  rmr_after=u.reg_read(UC_ARM_REG_R2);boundaries.append('RMR write');u.reg_write(UC_ARM_REG_PC,address+4|1);return
 if address==0x48f06:
  stopped=True;u.emu_stop()
uc.hook_add(UC_HOOK_CODE,hook)
uc.emu_start(0x5dc81,0,count=200000)
assert stopped,'parser did not return'
assert uc.reg_read(UC_ARM_REG_R0)==0
pointers=struct.unpack('<7I',uc.mem_read(outputs,28));assert pointers==(0,0,0x47000000,0,0,0,0),pointers
expected=bytearray(payload);expected[0x30:0xb0]=dram;expected[0x2c:0x30]=struct.pack('<I',12288);expected[0x24:0x2c]=b'BOOTDATA'
assert bytes(uc.mem_read(0x47000000,512))==expected
# Start at the actual main tail after boot0's MMU/cache cleanup. The cleanup
# is not executed here; its inherited control state remains a physical question.
uc.mem_write(sp+0x1c,struct.pack('<4I',0,0,0x47000000,0));uc.reg_write(UC_ARM_REG_SP,sp)
stopped=False;phase='tail';uc.emu_start(0x5e36b,0,count=20000)
assert stopped and rmr_after==(rmr_before|3)
assert struct.unpack('<2I',uc.mem_read(0x08001004,8))==(0x47000000,0)
expected[0x10:0x14]=bytes(4);expected[0x18:0x1c]=bytes(4);expected[0xb8:0xc8]=identity
assert bytes(uc.mem_read(0x47000000,512))==expected
result={'passed':True,'boot0_sha256':digest,'case':'actual plaintext parser and monitor-only main tail','load_address_source':'payload+0x2c; deliberately different TOC run_addr ignored','monitor_load':0x47000000,'zero_component_pointers':['u-boot','optee','other executable'],'parser_entry':'0x5dc80','main_tail_entry':'0x5e36a','reset_function':'0x48ee0','monitor_header_mutations':{'0x10':'u-boot entry=0','0x18':'optee entry=0','0x24':'8 bytes boot header data','0x2c':'DRAM MiB overwrites prior load address','0x30':'128 DRAM parameter bytes','0xb8':'16 identity bytes'},'rvbar_written':[0x47000000,0],'rmr_modeled_before':rmr_before,'rmr_observed_request':rmr_after,'modeled_boundaries':boundaries,'physical_access':False,'limitations':['Synthetic container already in RAM; media loading, package authentication/checksum and DRAM initialization not executed.','Cache/MMU cleanup precedes selected main tail and is not executed. RMR read/write and identity source modeled; no actual warm reset, EL3 entry or payload execution.','No SCP item is supplied; this case establishes no independent firmware load by the selected parser path, not whole-boot remote-core reset state.','Source-backed monitor-only handoff feasibility; not a bootable SunstoneOS artifact.']}
(OUT/'boot0-monitor-handoff.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS actual boot0 monitor-only parser and reset request; physical boot remains unqualified')
