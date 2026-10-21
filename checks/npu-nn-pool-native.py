#!/usr/bin/env python3
"""Actual fixed complete-NN constructor and repeated chunked caller path.

NPU execution, numerical results, CPU translations/cache effects, scheduler and
USB transport are modeled. SDK outputs test copying, not native inference.
"""
import argparse,hashlib,importlib.util,json
from pathlib import Path
from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_X0,UC_ARM64_REG_PC
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('nn_pool_base',ROOT/'checks/npu-large-native.py');base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)
class Machine(base.Machine):
 def __init__(self,image,oracles):
  super().__init__(image,oracles)
  # The inherited fixture installs one global code hook. Use the same modeled
  # boundaries as address hooks so large CRC/copy loops execute without Python.
  handle,=self.u._callbacks
  self.u.hook_del(handle)
  for at in self.interesting:self.u.hook_add(UC_HOOK_CODE,self.code,begin=at,end=at)
 def call(self,name,*args):
  self.u.reg_write(UC_ARM64_REG_SP,0x43fd0000);self.u.reg_write(UC_ARM64_REG_LR,0x43fff000)
  for i,value in enumerate(args):self.u.reg_write(UC_ARM64_REG_X0+i,value)
  self.u.emu_start(self.image.address(*name) if isinstance(name,tuple) else self.image.address(name),0,count=240000000)
  assert self.u.reg_read(UC_ARM64_REG_PC)==0x43fff000,('Instruction bound',name,hex(self.u.reg_read(UC_ARM64_REG_PC)))
  return self.u.reg_read(UC_ARM64_REG_X0)
def constructor(a,out,image,inputs,outputs):
 m=Machine(image,dict(zip(inputs,outputs)))
 from elftools.elf.elffile import ELFFile
 with a.image.open('rb') as stream:
  elf=ELFFile(stream);offsets=[]
  for cu in elf.get_dwarf_info().iter_CUs():
   if not cu.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith('/apps/npu-convolution/native.c'):continue
   for die in cu.iter_DIEs():
    if die.tag=='DW_TAG_variable' and die.attributes.get('DW_AT_name') and die.attributes['DW_AT_name'].value==b'app' and 'DW_AT_location' in die.attributes:
     typ=die.get_DIE_from_attribute('DW_AT_type');offsets.extend(d.attributes['DW_AT_data_member_location'].value for d in typ.iter_children() if d.tag=='DW_TAG_member' and d.attributes['DW_AT_name'].value==b'reserved')
 assert len(offsets)==1
 reserved=image.address('app')+offsets[0];rl=image.layouts['a733_npu_reserved'];cl=image.layouts['npu_convolution'];nl=image.layouts['a733_npu_native'];ml=image.layouts['a733_npu_memory'];bl=image.layouts['a733_npu_buffer'];el=image.layouts['device_memory_extent']
 c=reserved+rl['convolution'];native=c+cl['native'];memory=native+nl['memory'];image_at=memory+ml['image']
 assert m.call('a733_npu_reserved_prepare',reserved,1,image.address('a733_npu_application_model'),964224,0x03600000,0x43fff000,0)==c
 assert m.get(reserved+rl['admitted'],4)==m.get(c+cl['prepared'],4)==1
 extent=reserved+rl['extent'];assert m.get(extent+el['cpu'])==0x41600000 and m.get(extent+el['bytes'])==3235840
 cpus=(0x41600000,0x41606000,0x417eb000,0x4186b000,0x41890000,0x41891000,0x416e0000,0)
 devices=(0xe0000000,0xe0007000,0xe01ee000,0xe026f000,0xe0295000,0xe0297000,0xe00e2000,0)
 sizes=(24576,892928,524288,151552,4096,524288,1093632,0);roles=(4,4,2,0,1,3,1,0)
 tables=0x41911000;assert m.get(memory+ml['arena'])==tables and m.get(memory+ml['capacity'])==20480 and m.get(memory+ml['count'])==7
 # Decode the actual constructed root, not a table address inferred from sums.
 root=m.get(tables+(devices[0]>>24)*4,4);second=root&~4095
 assert root==0x41912001 and second%4096==0 and second==tables+4096
 pages=0
 for i in range(8):
  assert m.get(c+cl['buffers']+i*32+bl['cpu'])==cpus[i]
  if not sizes[i]:continue
  for offset in range(0,sizes[i],4096):
   at=second+(((devices[i]+offset)&0xffffff)>>12)*4
   assert m.get(at,4)==cpus[i]+offset+3+(4 if roles[i]==1 else 0);pages+=1
  assert m.get(second+(((devices[i]+sizes[i])&0xffffff)>>12)*4,4)==2
 assert pages==785 and m.get(tables+0x41*4,4)==2
 for slot,ref in ((0,0),(1,1),(6,2),(2,3),(3,4),(4,5)):
  actual=m.data(cpus[slot],sizes[slot]);expected=(a.assembly/f'buffer-{ref}.bin').read_bytes();assert actual==expected,slot
  (out/f'buffer-{ref}.bin').write_bytes(actual)
 m.hooks.pop(image.address('npu_convolution_send'));m.scheduler=2
 for field,value in [('bound',1),('client',m.task),('opened',1),('ready',1)]:m.put(c+cl[field],value,8 if field=='client' else 4)
 command=c+cl['commands']+image.layouts['a733_npu_command']['token']+8
 # The bootstrap publication cleans the CONSTANT descriptors and coefficients;
 # a job publication maintains only the input, pool and output.
 boot=c+cl['commands'];m.cache=[]
 assert m.call('a733_npu_memory_publish',memory,image_at,boot,1)==1
 for i in (0,1):assert [0,cpus[i],sizes[i]] in m.cache
 assert m.call('a733_npu_memory_finish',memory,boot,0)==1
 for index,data in enumerate(inputs):
  m.u.mem_write(cpus[6],bytes([0xa5+index])*sizes[6]);m.u.mem_write(0x43010000,data)
  assert m.call('npu_convolution_send',c,2,0x43010000,len(data),5000000,5,0x43e00100)==0
  assert m.data(cpus[6],sizes[6])==bytes(sizes[6]) and m.data(cpus[3],len(data))==data
  m.cache=[];assert m.call('a733_npu_memory_publish',memory,image_at,command,0)==1
  assert sorted(m.cache)==sorted([[0,cpus[3],sizes[3]],[1,cpus[4],sizes[4]],[1,cpus[6],sizes[6]]]),m.cache
  assert m.call('a733_npu_memory_finish',memory,command,0)==1
  for i in (6,4):assert [2,cpus[i],sizes[i]] in m.cache
 # Coefficient integrity is rechecked at the actual wrapper publication boundary.
 old=m.get(cpus[1],1);m.u.mem_write(cpus[1],bytes([old^1]));m.u.mem_write(cpus[6],bytes([0x7b])*sizes[6])
 m.cache=[]
 assert m.call(('publish','/drivers/npu/npu_convolution.c'),c,image_at,command,0)==0
 assert not m.cache and m.get(c+cl['failed'],4)==1 and m.data(cpus[6],sizes[6])==bytes([0x7b])*sizes[6]
 (out/'tables.bin').write_bytes(m.data(tables,20480))
 return dict(result='pass',actual_allocator=True,parent_bytes=3235840,mapped_payload_pages=pages,actual_second_level=hex(second),table_alignment_bytes=4096,translation_checks=m.translations,pool_clear_and_cache_policy_jobs=2,coefficient_mutation_retained_before_cache_publication=True,hardware_effects_modeled=True)
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('image','assembly','output'):p.add_argument('--'+name,type=Path,required=True)
 p.add_argument('--input',type=Path,action='append',required=True);p.add_argument('--reference',type=Path,action='append',required=True);p.add_argument('--case',choices=('constructor','runtime'),required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
 inputs=[p.read_bytes() for p in a.input];outputs=[p.read_bytes() for p in a.reference];assert len(inputs)==len(outputs)==2 and inputs[0]!=inputs[1]
 image=base.Image(a.image);result=(constructor if a.case=='constructor' else base.check_runtime)(a,a.output,image,inputs,outputs);result.update(image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),case=a.case,physical_access=False);(a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS complete NN',a.case)
if __name__=='__main__':main()
