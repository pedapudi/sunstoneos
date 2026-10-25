#!/usr/bin/env python3
"""New session seal/native ownership checks against the already linked image."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_SP,UC_ARM64_REG_LR
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def run(image,session,out):
 out.mkdir(parents=True,exist_ok=False);fixture=module('sd0_binding_machine',ROOT/'checks/storage-read-native.py');raw=session.read_bytes();e=fixture.sd0_image(image);m=fixture.Machine(e,raw,'binding')
 # ABI/nominal producer validation precedes mutation cases.
 prep=module('sd0_binding_producer',ROOT/'apps/storage-read/prepare.py');prep.embedded_reference(image.read_bytes(),raw)
 assert m.call('storage_read_session_valid',m.base)==1
 values=[0x4153373352444331,2,224,*struct.unpack_from('<2Q',raw,24),struct.unpack_from('<Q',raw,32)[0]^((1<<64)-1)]
 armed=23999900;end=armed+1440000000
 contract=struct.pack('<6Q',*values)+raw[48:80]+struct.pack('<14Q',armed,armed+24000000,end,end,struct.unpack_from('<Q',raw,40)[0],0,0,0,0,0,0,0,0x41f00000,864)+hashlib.sha256(raw).digest()
 assert len(contract)==224;m.cpu.mem_write(0x50020000,contract)
 assert m.call('a733_smp_contract_valid',0x50020000,24000000)==1
 changed=bytearray(raw);changed[760]^=1;m.cpu.mem_write(0x41f00000,bytes(changed));assert m.call('a733_smp_contract_valid',0x50020000,24000000)==0;m.cpu.mem_write(0x41f00000,raw)
 assert m.call('storage_read_session_prepare',0x50020000)==1
 m.cpu.mem_write(0x41f00000,bytes(changed));sealed=m.call('storage_read_admitted_session');assert bytes(m.cpu.mem_read(sealed,864))==raw
 m.cpu.mem_write(m.board+e.layouts['board_application']['admitted_session'],raw[:592]);assert m.call('board_application_pins',0x50030000)==1
 assert m.read(0x50030000+5*4)==0x7f and not m.trace
 # Actual native constructor/context and read-only write boundary.
 m.call('storage_read_task',0);assert m.read(m.platform+m.p['initialized'])==1 and not m.trace
 assert m.call('a733_mmc_platform_scopes',m.base+m.layout['config'],0x50031000)==1
 op=e.layouts['a733_mmc_platform_ops'];write=m.read(m.native+m.n['operations']+op['write'],8)
 assert write in e.symbols['write32'];m.held=True
 def invoke(address,value):
  m.finished=False;m.cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);m.cpu.reg_write(UC_ARM64_REG_LR,0x50fff000)
  for i,v in enumerate((m.native,address,value)):m.cpu.reg_write(UC_ARM64_REG_X0+i,v)
  m.cpu.emu_start(write,0,count=100000);assert m.finished;return m.cpu.reg_read(UC_ARM64_REG_X0)
 assert invoke(0x04020200,0x12345678)==0 and invoke(0x04020018,0x80006758)==0 and not m.trace
 m.held=False
 # Metadata retain rejects another actual current task, without MMIO/deadline.
 m.write(m.native+m.n['owner_task'],2,8);assert m.call('a733_mmc_platform_retain',m.platform)==5 and not m.read(m.platform+m.p['fault'])
 m.write(m.native+m.n['owner_task'],1,8);assert m.call('a733_mmc_platform_retain',m.platform)==4 and m.read(m.platform+m.p['fault'])==1 and not m.trace
 (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),full_session_seal=True,postseal_mutation_isolated=True,pf_authority=127,native_unbound_fifo_cmd24_refused=True,native_current_owner_retain=True,layout=e.layouts['storage_read_session'],scope='Actual linked admission/seal and native task-context/write guards; RTOS return values, translations and provider authority modeled.'),indent=2)+'\n')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('image','session','output'):p.add_argument('--'+n,type=Path,required=True)
 a=p.parse_args();run(a.image,a.session,a.output)
