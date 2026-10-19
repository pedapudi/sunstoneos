#!/usr/bin/env python3
"""Actual fixed allocator/tables/pool publication and paced chunked NPU client.

Translation/cache effects, hardware service, numerical results and transport are
modeled. Saved SDK outputs test copying, not native inference. Each USB batch
executes the linked task's eight polls and then a1ms delay before client progress.
"""
import argparse,ctypes,hashlib,importlib.util,json,struct
from pathlib import Path
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
graph=module('large_graph_base',ROOT/'checks/npu-graph-native.py')
host=module('large_graph_host',ROOT/'apps/npu-runtime-large/host.py')
class Image(graph.runtime.base.Image):
 def __init__(self,path):
  # Private types in separate assembler units can share names. Retain only
  # unambiguous layouts; every layout this check consumes must remain unique.
  from elftools.elf.elffile import ELFFile
  self.symbols={};self.functions={};self.layouts={};ambiguous=set()
  with path.open('rb') as stream:
   elf=ELFFile(stream);self.parts=[(int(p['p_vaddr']),p.data()) for p in elf.iter_segments() if p['p_type']=='PT_LOAD']
   for symbol in elf.get_section_by_name('.symtab').iter_symbols():self.symbols.setdefault(symbol.name,set()).add(int(symbol['st_value']))
   for cu in elf.get_dwarf_info().iter_CUs():
    unit=cu.get_top_DIE().attributes['DW_AT_name'].value.decode()
    for die in cu.iter_DIEs():
     if 'DW_AT_name' not in die.attributes:continue
     name=die.attributes['DW_AT_name'].value.decode()
     if die.tag=='DW_TAG_subprogram' and 'DW_AT_low_pc' in die.attributes:self.functions.setdefault((unit,name),set()).add(int(die.attributes['DW_AT_low_pc'].value))
     if die.tag!='DW_TAG_structure_type':continue
     fields={d.attributes['DW_AT_name'].value.decode():int(d.attributes['DW_AT_data_member_location'].value) for d in die.iter_children() if d.tag=='DW_TAG_member' and 'DW_AT_name' in d.attributes and 'DW_AT_data_member_location' in d.attributes}
     if fields:
      if name in self.layouts and self.layouts[name]!=fields:ambiguous.add(name)
      self.layouts[name]=fields
  for name in ambiguous:del self.layouts[name]
class Machine(graph.Machine):
 def __init__(self,image,oracles):
  self.trace=[];self.returns={};self.polls=0;self.delays=0;self.phase='constructor';self.cache=[];self.scheduler=1;self.task=1
  super().__init__(image,oracles);self.max_tx=512
  self.traced={image.address(n):n for n in ('nbg_model_open','nbg_sh_nt_requirements','a733_npu_native_init') if image.symbols.get(n)}
  for name in ('aarch64_device_cache','xTaskGetCurrentTaskHandle','npu_service_send'):
   self.hooks[image.address(name)]=name
  self.interesting=set(self.hooks)|set(self.traced)|{0x43fff000,image.address('a733_observe_privileged'),image.address('a733_observe_digest')}
  for base,data in image.parts:
   for offset in range(0,len(data)-3,4):
    word=int.from_bytes(data[offset:offset+4],'little')
    if word&0xffffffe0 in (0xd53be000,0xd53be020):self.interesting.add(base+offset)
 def call(self,name,*args):
  self.u.reg_write(UC_ARM64_REG_SP,0x43fd0000);self.u.reg_write(UC_ARM64_REG_LR,0x43fff000)
  for i,value in enumerate(args):self.u.reg_write(UC_ARM64_REG_X0+i,value)
  self.u.emu_start(self.image.address(name),0,count=30000000)
  assert self.u.reg_read(UC_ARM64_REG_PC)==0x43fff000, ('Instruction bound',name,hex(self.u.reg_read(UC_ARM64_REG_PC)))
  return self.u.reg_read(UC_ARM64_REG_X0)
 def code(self,cpu,at,size,unused):
  if at not in self.interesting:return
  a=[self.u.reg_read(UC_ARM64_REG_X0+i) for i in range(4)];name=self.hooks.get(at)
  if at in self.returns:self.trace.append((self.returns.pop(at),a[0]))
  if at in self.traced:self.returns[self.u.reg_read(UC_ARM64_REG_LR)]=self.traced[at]
  if at==self.image.address('a733_observe_privileged'):self.polls+=1
  if name=='xTaskGetSchedulerState':self.ret(self.scheduler)
  elif name=='xTaskGetCurrentTaskHandle':self.ret(self.task)
  elif name=='aarch64_device_cache':self.cache.append(a[:3]);self.ret(1)
  elif name=='npu_service_send':self.ret(0)  # Direct wrapper check queues no hardware work.
  elif name=='vTaskDelay':
   assert a[0]==1;self.delays+=1;self.ret();self.u.emu_stop()
  elif name=='tud_vendor_n_read':
   count=min(a[2],len(self.incoming),512);self.u.mem_write(a[1],bytes(self.incoming[:count]));del self.incoming[:count];self.ret(count)
  elif name=='digest':
   assert 0<a[1]<=1048576
   self.u.mem_write(a[2],hashlib.sha256(self.data(a[0],a[1])).digest());self.ret()
  elif at==self.image.address('a733_observe_digest'):
   self.u.mem_write(a[2],hashlib.sha256(self.data(a[0],a[1])).digest());self.ret()
  else:super().code(cpu,at,size,unused)
 def usb_batch(self,ack=True):
  before=len(self.tx);polls=self.polls;delays=self.delays
  self.u.reg_write(UC_ARM64_REG_SP,0x43fd0000);self.u.reg_write(UC_ARM64_REG_LR,0x43fff000)
  entry=self.image.address('usb_task',unit='/apps/npu-runtime-large/usb.c')
  self.u.emu_start(entry,0,count=3000000)
  assert self.polls-polls==8 and self.delays-delays==1
  self.ticks+=24000
  if ack:
   for raw in self.tx[before:]:self.call('tud_vendor_tx_cb',0,len(raw))
 def exchange(self,packet,clients=True):
  self.incoming.extend(packet);before=len(self.tx)
  for _ in range(256):
   self.usb_batch()
   if clients:self.client()
   raw=b''.join(self.tx[before:])
   if len(raw)==1040:return raw
  raise AssertionError('Large request did not finish within bounded USB batches')

def direct_header(m,packet):
 m.u.mem_write(0x43e00000,packet[:64]);return m.call('npu_runtime_header',0x43e00000)
def check_constructor(a,out,image,inputs,outputs,bad=False):
 m=Machine(image,dict(zip(inputs,outputs)))
 # Locate the private static app type from its actual DWARF variable.
 from elftools.elf.elffile import ELFFile
 with a.image.open('rb') as f:
  e=ELFFile(f);matches=[]
  for cu in e.get_dwarf_info().iter_CUs():
   if not cu.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith('/apps/npu-convolution/native.c'):continue
   for die in cu.iter_DIEs():
    if die.tag=='DW_TAG_variable' and die.attributes.get('DW_AT_name') and die.attributes['DW_AT_name'].value==b'app' and 'DW_AT_location' in die.attributes:
     typ=die.get_DIE_from_attribute('DW_AT_type');fields={d.attributes['DW_AT_name'].value.decode():d.attributes['DW_AT_data_member_location'].value for d in typ.iter_children() if d.tag=='DW_TAG_member'};matches.append(fields['reserved'])
  assert len(matches)==1
 reserved=image.address('app')+matches[0]
 if bad:
  model=image.address('a733_npu_application_model');m.u.mem_write(model+3000,bytes([m.get(model+3000,1)^1]))
 result=m.call('a733_npu_reserved_prepare',reserved,1,image.address('a733_npu_application_model'),3976,0x03600000,0x43fff000,0)
 rl=image.layouts['a733_npu_reserved'];cl=image.layouts['npu_convolution'];nl=image.layouts['a733_npu_native'];ml=image.layouts['a733_npu_memory'];el=image.layouts['device_memory_extent']
 c=reserved+rl['convolution'];native=c+cl['native'];memory=native+nl['memory'];image_at=memory+ml['image'];bl=image.layouts['a733_npu_buffer'];stride=32
 if bad:
  assert result==0 and m.get(c+cl['failed'],4)==1 and m.get(native+nl['initialized'],4)==0
  return dict(result='pass',model_mutation_refused=True,hardware_access=False)
 assert result==c and m.get(reserved+rl['admitted'],4)==1 and m.get(c+cl['prepared'],4)==1, dict(result=hex(result),expected=hex(c),admitted=m.get(reserved+rl['admitted'],4),prepared=m.get(c+cl['prepared'],4),failed=m.get(c+cl['failed'],4),pc=hex(m.u.reg_read(UC_ARM64_REG_PC)),translations=m.translations,model_index=m.get(c+cl['model_index'],4),input_bytes=m.get(c+cl['input_bytes'],4),native_initialized=m.get(native+nl['initialized'],4),trace=m.trace)
 extent=reserved+rl['extent'];assert m.get(extent+el['cpu'])==0x41600000 and m.get(extent+el['bytes'])==2916352
 assert m.translations>1024
 cpus=(0x41600000,0,0x41702000,0x41782000,0x41802000,0x41843000,0x41602000,0x41601000)
 devices=(0xe0000000,0,0xe0105000,0xe0186000,0xe0207000,0xe0249000,0xe0004000,0xe0002000)
 sizes=(4096,0,524288,524288,266240,524288,1048576,4096)
 roles=(0,0,2,0,1,3,1,0)
 tables=0x418c3000
 assert m.get(memory+ml['arena'])==tables and m.get(memory+ml['capacity'])==20480
 assert m.get(memory+ml['count'])==7
 for i in range(8):
  assert m.get(c+cl['buffers']+i*stride+bl['cpu'])==cpus[i]
  if not sizes[i]:continue
  root=m.get(tables+(devices[i]>>24)*4,4);assert root&3==1
  for offset in range(0,sizes[i],4096):
   at=(root&~4095)+(((devices[i]+offset)&0xffffff)>>12)*4
   assert m.get(at,4)==cpus[i]+offset+3+(4 if roles[i]==1 else 0)
 assert m.get(tables+0x41*4,4)==2  # CPU staging is absent from NPU roots.
 assert m.get(tables+4096+4,4)==2  # Device guard after first descriptor page.
 for slot,ref in ((0,0),(7,1),(6,2),(2,3),(3,4),(4,5)):
  expected=(a.assembly/f'buffer-{ref}.bin').read_bytes();assert m.data(cpus[slot],sizes[slot])==expected
 # Direct actual wrapper: with a mocked rejected queue, no job is admitted.
 # Still prove full per-job pool clear/input copy and real memory-policy cache ops.
 m.hooks.pop(image.address('npu_convolution_send'));m.scheduler=2
 m.put(c+cl['bound'],1,4);m.put(c+cl['client'],m.task);m.put(c+cl['opened'],1,4);m.put(c+cl['ready'],1,4)
 command=c+cl['commands']+image.layouts['a733_npu_command']['token']+8
 for index,data in enumerate(inputs):
  m.u.mem_write(cpus[6],bytes([0xa5+index])*sizes[6]);m.u.mem_write(0x43010000,data)
  assert m.call('npu_convolution_send',c,2,0x43010000,len(data),5000000,5,0x43e00100)==0
  assert m.data(cpus[6],sizes[6])==bytes(sizes[6]) and m.data(cpus[3],len(data))==data
  m.cache=[]
  assert m.call('a733_npu_memory_publish',memory,image_at,command,0)==1
  assert [1,cpus[6],sizes[6]] in m.cache and [1,cpus[4],sizes[4]] in m.cache
  assert [0,cpus[7],sizes[7]] in m.cache
  assert m.call('a733_npu_memory_finish',memory,command,0)==1
  assert [2,cpus[6],sizes[6]] in m.cache and [2,cpus[4],sizes[4]] in m.cache
 (out/'tables.bin').write_bytes(m.data(tables,20480))
 return dict(result='pass',actual_allocator=True,parent_bytes=2916352,mapped_payload_pages=707,translation_checks=m.translations,pool_clear_publication_finish_jobs=2,shader_clean=True,physical_cache_effects_modeled=True)

def check_runtime(a,out,image,inputs,outputs):
 m=Machine(image,dict(zip(inputs,outputs)));m.scheduler=2;sequence=0;deadlines=[]
 for rid,(raw,expected) in enumerate(zip(inputs,outputs),1):
  begin=m.ticks;sequence+=1;value=host.decode(m.exchange(host.packet(sequence,host.BEGIN,m.prepared,rid,5000000,total=len(raw))),sequence,m.prepared,rid)
  assert value['state']==host.UPLOADING
  for offset in range(0,len(raw),512):
   sequence+=1;part=raw[offset:offset+512];value=host.decode(m.exchange(host.packet(sequence,host.UPLOAD,m.prepared,rid,data=part,offset=offset)),sequence,m.prepared,rid)
   assert value['received']==offset+len(part)
  sequence+=1;value=host.decode(m.exchange(host.packet(sequence,host.COMMIT,m.prepared,rid)),sequence,m.prepared,rid)
  for _ in range(100):
   if value['state']==host.DONE:break
   sequence+=1;value=host.decode(m.exchange(host.packet(sequence,host.READ,m.prepared,rid)),sequence,m.prepared,rid)
  assert value['state']==host.DONE
  data=bytearray();frames=[]
  while len(data)<len(expected):
   if not value['output'] or value['offset']!=len(data):
    sequence+=1;frame=m.exchange(host.packet(sequence,host.READ,m.prepared,rid,offset=len(data)));value=host.decode(frame,sequence,m.prepared,rid);frames.append(frame)
   assert value['state']==host.DONE and value['offset']==len(data)
   data.extend(value['output'])
  assert bytes(data)==expected and value['input_sha256']==hashlib.sha256(raw).hexdigest() and value['output_sha256']==hashlib.sha256(expected).hexdigest()
  (out/f'output-{rid}.bin').write_bytes(data)
  sequence+=1;m.exchange(host.packet(sequence,host.FINISH,m.prepared,rid));assert m.state()==host.IDLE
  deadlines.append(dict(request=rid,elapsed_ticks=m.ticks-begin,request_limit_ticks=5000000*24));assert m.ticks-begin<5000000*24
 sequence+=1;value=host.decode(m.exchange(host.packet(sequence,host.CLOSE,m.prepared,3,5000000)),sequence,m.prepared,3)
 for _ in range(100):
  if value['state']==host.CLOSED:break
  sequence+=1;value=host.decode(m.exchange(host.packet(sequence,host.READ,m.prepared,3)),sequence,m.prepared,3)
 assert value['state']==host.CLOSED and value['jobs']==2 and value['irq_completions']==3 and m.closed
 assert m.operations==[1,2,0,4,2,0,4,5] and m.inputs==inputs
 assert m.ticks<480000000+24000000
 return dict(result='pass',inputs=2,copied_outputs=2,healthy_close=True,usb_task_polls=m.polls,tick_hz=1000,polls_per_delay=8,request_deadlines=deadlines,numerical_output_and_irq_modeled=True)

def check_negatives(a,out,image,inputs,outputs):
 results={}
 def fresh():return Machine(image,dict(zip(inputs,outputs)))
 def begin(m,rid=1):return direct_header(m,host.packet(1,host.BEGIN,m.prepared,rid,5000000,total=522240))
 def refused(name,operation):
  m=fresh();operation(m);assert m.state()==host.FAILED and not m.operations;results[name]=True
 refused('stale-epoch',lambda m:direct_header(m,host.packet(1,host.BEGIN,{**m.prepared,'epoch':6},1,5000000,total=522240)))
 refused('wrong-shape',lambda m:direct_header(m,host.packet(1,host.BEGIN,m.prepared,1,5000000,total=522239)))
 refused('offset-gap',lambda m:(begin(m),direct_header(m,host.packet(2,host.UPLOAD,m.prepared,1,data=b'x',offset=1))))
 refused('commit-before-complete',lambda m:(begin(m),direct_header(m,host.packet(2,host.COMMIT,m.prepared,1))))
 refused('oversize-chunk',lambda m:(begin(m),direct_header(m,host.packet(2,host.UPLOAD,m.prepared,1,data=bytes(513)))))
 def late(m):
  begin(m);direct_header(m,host.packet(2,host.UPLOAD,m.prepared,1,data=b'x'));m.ticks=m.get(m.slot+m.layout['deadline']);m.u.mem_write(0x43e00000,b'x');m.call('npu_runtime_payload',0x43e00000,1)
 refused('late-incomplete-upload',late)
 def done():
  m=fresh();begin(m)
  for field,value in [('state',host.DONE),('start',m.ticks),('end',m.ticks+24),('job',2),('jobs',1),('irq_count',2)]:m.put(m.slot+m.layout[field],value,4 if field in ('state','jobs','irq_count') else 8)
  m.u.mem_write(image.address('npu_runtime_output'),outputs[0]);return m
 m=done();direct_header(m,host.packet(2,host.FINISH,m.prepared,1));assert m.state()==host.FAILED;results['early-finish']=True
 m=done();direct_header(m,host.packet(2,host.CLOSE,m.prepared,2,5000000));assert m.state()==host.FAILED;results['close-before-finish']=True
 m=done();assert direct_header(m,host.packet(2,host.READ,m.prepared,1))==0
 m.call('npu_runtime_frame',0x43e00000,0x43e01000,0x43e01008,0x43e01010)
 assert m.get(0x43e00000+792)==0 and m.get(0x43e01000,4)==host.DONE
 m.call('npu_runtime_ack',host.DONE,1,0);m.call('npu_runtime_frame',0x43e00000,0x43e01000,0x43e01008,0x43e01010)
 assert m.get(0x43e00000+792)==512 and m.get(0x43e01000,4)==0
 results['copied-chunk-held-until-full-callback']=True
 direct_header(m,host.packet(3,host.READ,m.prepared,1,offset=0));assert m.state()==host.FAILED;results['replayed-output-offset']=True
 m=done()
 for offset in range(0,262144,512):
  direct_header(m,host.packet(2,host.READ,m.prepared,1,offset=offset));m.call('npu_runtime_ack',host.DONE,1,offset)
 direct_header(m,host.packet(3,host.FINISH,m.prepared,1));m.call('npu_runtime_frame',0x43e00000,0x43e01000,0x43e01008,0x43e01010)
 assert m.state()==host.DONE and m.get(0x43e01000,4)==0x100
 m.call('npu_runtime_ack',0x100,1,0);assert m.state()==host.IDLE
 direct_header(m,host.packet(4,host.BEGIN,m.prepared,1,5000000,total=522240));assert m.state()==host.FAILED
 results['finish-full-callback-releases-and-stale-id-refuses']=True
 m=done();direct_header(m,host.packet(2,host.READ,m.prepared,1));m.call('npu_runtime_frame',0x43e00000,0x43e01000,0x43e01008,0x43e01010);m.ticks=m.get(m.slot+m.layout['deadline']);m.call('npu_runtime_ack',host.DONE,1,0);assert m.state()==host.FAILED
 results['late-output-callback-retains']=True
 return dict(result='pass',cases=results,service_and_transport_modeled=True)

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('image','assembly','output'):p.add_argument('--'+n,type=Path,required=True)
 p.add_argument('--input',type=Path,action='append',required=True);p.add_argument('--reference',type=Path,action='append',required=True)
 p.add_argument('--case',choices=('constructor','bad-model','runtime','negative'),required=True);a=p.parse_args()
 inputs=[q.read_bytes() for q in a.input];outputs=[q.read_bytes() for q in a.reference];assert len(inputs)==len(outputs)==2
 a.output.mkdir(parents=True,exist_ok=False);image=Image(a.image)
 result=check_negatives(a,a.output,image,inputs,outputs) if a.case=='negative' else check_runtime(a,a.output,image,inputs,outputs) if a.case=='runtime' else check_constructor(a,a.output,image,inputs,outputs,a.case=='bad-model')
 result.update(image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),case=a.case,physical_access=False)
 (a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS large native',a.case)
if __name__=='__main__':main()
