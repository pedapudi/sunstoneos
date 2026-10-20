#!/usr/bin/env python3
"""Pinned A733 encoder instructions in isolated Unicorn; modeled ownership/MMIO only."""
import argparse,hashlib,json,struct,gzip,pickle
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_WRITE,UC_HOOK_MEM_READ,UC_HOOK_MEM_INVALID
from unicorn.arm64_const import *
REGS=[UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_X4,UC_ARM64_REG_X5,UC_ARM64_REG_X6,UC_ARM64_REG_X7]
RAM=0x10000000;OPS=RAM+0x2000;VECTX=RAM+0x3000;LEVEL=RAM+0x4000;CONFIG=RAM+0x5000;FRAME=RAM+0x6000;VALUE=RAM+0x7000;HEAP=RAM+0x100000;INPUT=0x10800000;STOP=0x130ff000;STACK=0x130fe000;STUB=0x13000000;MMIO=0x01c10000
PINS={'libvenc_base.so':'5282b1cd4f69014bb11b3dba9e13daa734de2407c39f4dec3566b7af534b599d','libvenc_codec.so':'b39fcc6e9c0b7deb58cfd4263c93825135c662fcc770b869d78caa5af91097cb','libVE.so':'3af49a5eb24f6832518a858274fb206d60e490985bff02f5d5be36d053f65be4'}
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for n in ['libraries','fixtures','output']:p.add_argument('--'+n,type=Path,required=True)
 p.add_argument('--router-only',action='store_true')
 p.add_argument('--repair-encoder-context',action='store_true')
 p.add_argument('--resume',type=Path)
 p.add_argument('--case',choices=['finish','no-finish','stall','mb-timeout','hw-timeout','wait-error'],default='finish')
 a=p.parse_args();a.output.mkdir(exist_ok=False);model_identity=hashlib.sha256(Path(__file__).read_bytes()).hexdigest();resume_identity=None if a.resume is None else dict(path=str(a.resume.resolve()),sha256=hashlib.sha256(a.resume.read_bytes()).hexdigest());input_identity={str(path.resolve()):hashlib.sha256(path.read_bytes()).hexdigest() for path in sorted(a.fixtures.iterdir()) if path.is_file()};u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
 for start,n in [(0,0x100000),(0x1000000,0x100000),(0x2000000,0x100000),(RAM,0x2000000),(0x13000000,0x100000),(MMIO,0x1000)]:u.mem_map(start,n)
 u.reg_write(UC_ARM64_REG_CPACR_EL1,3<<20)
 images=[];symbols={};stubs={};stubnames={};allocations=[];events=[];trace=[];phase=['setup'];heap=[HEAP];ctx=[0];ring=[0];trigger=[False];waits=[0]
 def put(address,value,fmt='Q'):u.mem_write(address,struct.pack('<'+fmt,value))
 def get(address,fmt='Q'):return struct.unpack('<'+fmt,u.mem_read(address,struct.calcsize(fmt)))[0]
 def stub(name):
  if name not in stubs:stubs[name]=STUB+16*len(stubs);stubnames[stubs[name]]=name
  return stubs[name]
 names=['libvenc_codec.so','libvenc_base.so','libVE.so']
 for i,name in enumerate(names):
  path=a.libraries/name;digest=hashlib.sha256(path.read_bytes()).hexdigest();assert name not in PINS or digest==PINS[name];f=path.open('rb');elf=ELFFile(f);base=i*0x1000000;images.append((name,base,elf,f,digest))
  for segment in elf.iter_segments():
   if segment['p_type']=='PT_LOAD':u.mem_write(base+segment['p_vaddr'],segment.data())
  for s in elf.get_section_by_name('.symtab').iter_symbols():
   if s['st_value'] and s['st_shndx']!='SHN_UNDEF':symbols.setdefault(s.name,base+s['st_value'])
 model={'cdc_malloc','cdc_calloc','cdc_free','malloc','calloc','free','memset','memcpy','memmove','memcmp','printf','puts','pthread_mutex_init','pthread_mutex_lock','pthread_mutex_unlock','pthread_mutex_destroy','__EncAdapterMemPalloc','__EncAdapterMemNoCachePalloc','__EncAdapterMemPfree','__EncAdapterMemFlushCache','__EncAdapterMemGetPhysicAddress'}
 for name,base,elf,f,digest in images:
  for section_name in ['.rela.dyn','.rela.plt']:
   section=elf.get_section_by_name(section_name)
   if section is None:continue
   table=elf.get_section(section['sh_link'])
   for rel in section.iter_relocations():
    s=table.get_symbol(rel['r_info_sym']);n=s.name;t=rel['r_info_type']
    if t==1027:value=base+rel['r_addend']
    elif n=='GLOBAL_LOG_LEVEL':value=LEVEL
    elif n in model:value=stub(n)
    elif n in symbols:value=symbols[n]+rel['r_addend']
    else:value=stub(n)
    put(base+rel['r_offset'],value)
 for n in model:
  if n in symbols:stubnames[symbols[n]]=n
 def alloc(n,kind):
  assert 0<n<=0x800000;start=(heap[0]+4095)&~4095;heap[0]=start+((n+4095)&~4095);assert heap[0]<INPUT;u.mem_write(start,bytes(n));allocations.append(dict(address=start,bytes=n,kind=kind,phase=phase[0]));return start
 def ret(value=0):u.reg_write(REGS[0],value&0xffffffffffffffff);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
 def hook(m,pc,size,unused):
  if pc==STOP:m.emu_stop();return
  name=stubnames.get(pc)
  if name is None:return
  v=[m.reg_read(r) for r in REGS];events.append(dict(name=name,args=v[:4],phase=phase[0]))
  if name in ['malloc','calloc','cdc_malloc','cdc_calloc']:ret(alloc(v[0]*(v[1] if name in ['calloc','cdc_calloc'] else 1),name))
  elif name in ['__EncAdapterMemPalloc','__EncAdapterMemNoCachePalloc']:ret(alloc(v[1],name))
  elif name=='memset':assert v[2]<=0x800000;m.mem_write(v[0],bytes([v[1]&255])*v[2]);ret(v[0])
  elif name in ['memcpy','memmove']:assert v[2]<=0x800000;m.mem_write(v[0],bytes(m.mem_read(v[1],v[2])));ret(v[0])
  elif name=='memcmp':ret(int(bytes(m.mem_read(v[0],v[2]))!=bytes(m.mem_read(v[1],v[2]))))
  elif name=='__EncAdapterMemGetPhysicAddress':ret(v[1])
  elif name in ['free','cdc_free','printf','puts','__EncAdapterMemPfree','__EncAdapterMemFlushCache'] or name.startswith('pthread_mutex_'):ret(0)
  elif name=='MODEL_WAIT':
   waits[0]+=1;assert trigger[0]
   if a.case=='wait-error':ret(-1);return
   start=get(MMIO+0xb88,'I');end=start+256;assert end<=get(MMIO+0xb8c,'I');out=(get(MMIO+0xb80,'I')<<8)+start//8;assert any(x['address']<=out and out+32<=x['address']+x['bytes'] for x in allocations if x['kind']=='__EncAdapterMemPalloc');m.mem_write(out,bytes([0x55])*32);put(MMIO+0xb1c,{'finish':1,'no-finish':0,'stall':2,'mb-timeout':4,'hw-timeout':8}[a.case],'I');put(MMIO+0xb90,end,'I');ret(0)
  elif name in ['MODEL_RESET','MODEL_ENABLE','MODEL_DISABLE','MODEL_SET_VE_MODE']:ret(0)
  else:raise RuntimeError(('unmodeled external boundary',name,hex(pc),v[:4]))
 def write(m,access,address,size,value,unused):
  trace.append(dict(phase=phase[0],pc=m.reg_read(UC_ARM64_REG_PC),offset=address-MMIO,width=size,value=value))
  if address==MMIO+0xb18 and value==0x10008:trigger[0]=True
 def read(m,access,address,size,value,unused):
  if address==MMIO+0xb1c and not trigger[0]:put(address,0,'I')
 def invalid(m,access,address,size,value,unused):raise RuntimeError(('unmapped',access,hex(address),hex(m.reg_read(UC_ARM64_REG_PC))))
 for pc in sorted(set(stubnames)|{STOP}):u.hook_add(UC_HOOK_CODE,hook,begin=pc,end=pc)
 # Additional modeled operation-table callbacks are allocated after relocation.
 for n in ['MODEL_WAIT','MODEL_RESET','MODEL_ENABLE','MODEL_DISABLE','MODEL_SET_VE_MODE']:
  pc=stub(n);u.hook_add(UC_HOOK_CODE,hook,begin=pc,end=pc)
 u.hook_add(UC_HOOK_MEM_WRITE,write,begin=MMIO,end=MMIO+0xfff);u.hook_add(UC_HOOK_MEM_READ,read,begin=MMIO,end=MMIO+0xfff);u.hook_add(UC_HOOK_MEM_INVALID,invalid)
 put(LEVEL,7,'I');u.mem_write(CONFIG,(a.fixtures/'config.bin').read_bytes());u.mem_write(FRAME,(a.fixtures/'frame.bin').read_bytes());pixels=(a.fixtures/'input.nv12').read_bytes();assert len(pixels)==460800;u.mem_write(INPUT,pixels)
 # Reuse the checked split-map router context, explicitly encoder not decoder.
 put(VECTX+0x74,3,'I');put(VECTX+0x48,MMIO);put(VECTX+0x98,0x0003331000021320);put(VECTX,0,'I');put(VECTX+4,1,'I')
 for off,name in [(0x20,'MODEL_RESET'),(0x28,'MODEL_WAIT'),(0x40,'VeGetGroupRegAddr'),(0x80,'MODEL_ENABLE'),(0x88,'MODEL_DISABLE'),(0xf0,'MODEL_SET_VE_MODE')]:put(OPS+off,symbols[name] if name in symbols else stubs[name])
 def call(name,*values):
  phase[0]=name
  for reg,value in zip(REGS,values):u.reg_write(reg,value)
  u.reg_write(UC_ARM64_REG_SP,STACK);u.reg_write(UC_ARM64_REG_LR,STOP);u.emu_start(symbols[name],STOP,count=2000000);assert u.reg_read(UC_ARM64_REG_PC)==STOP,(name,hex(u.reg_read(UC_ARM64_REG_PC)));return u.reg_read(REGS[0])
 if a.router_only:
  routes=[call('VeGetGroupRegAddr',VECTX,group,0) for group in [0,12,13]];assert routes==[MMIO+0x800,MMIO+0xa00,MMIO+0xb00]
  (a.output/'result.json').write_text(json.dumps(dict(passed=True,router_only=True,libraries=PINS,inputs=input_identity,model_sha256=model_identity,decoder_flag=0,encoder_flag=1,map=3,routes=routes,physical_access=False,scope='Actual pinned group-router instructions only; context flags derived from VideoEncCreate and VeInitialize. No reset/power/codec execution.'),indent=2)+'\n');print('PASS encoder TOP/ENCPP/ENC routing');return
 def save(stage):
  record=dict(stage=stage,model_sha256=model_identity,stub_map=stubnames,inputs=input_identity,libraries=PINS,context=ctx[0],heap=heap[0],allocations=allocations,events=events,mmio=trace,memory=[(at,bytes(u.mem_read(at,n))) for at,n in [(0,0x100000),(0x1000000,0x100000),(0x2000000,0x100000),(RAM,0x2000000),(0x13000000,0x100000),(MMIO,0x1000)]])
  (a.output/(stage+'.pkl.gz')).write_bytes(gzip.compress(pickle.dumps(record,protocol=4),compresslevel=1))
 stage=''
 if a.resume:
  saved=pickle.loads(gzip.decompress(a.resume.read_bytes()));assert saved['inputs']==input_identity and saved['libraries']==PINS
  for at,data in saved['memory']:u.mem_write(at,data)
  resume_identity['stage']=saved['stage'];assert 'stub_map' not in saved or saved['stub_map']==stubnames
  stage=saved['stage'];ctx[0]=saved['context'];heap[0]=saved['heap'];allocations[:]=saved['allocations'];events[:]=saved['events'];trace[:]=saved['mmio'];assert stage in ['opened','configured','initialized']
 if a.repair_encoder_context:
  assert stage=='initialized' and get(VECTX,'I')==0 and get(VECTX+4,'I')==0 and get(ctx[0]+0x270)==0
  # Exact creator/init-derived fixture repair: creator nEncoderFlag=1, then
  # actual group0 router result that JpegInit stores at ctx+0x270. No replay.
  put(VECTX+4,1,'I');top=call('VeGetGroupRegAddr',VECTX,0,0);assert top==MMIO+0x800;put(ctx[0]+0x270,top);save('initialized')
  (a.output/'result.json').write_text(json.dumps(dict(passed=True,context_repair_only=True,resume=resume_identity,model_sha256=model_identity,inputs=input_identity,libraries=PINS,changes=[dict(address=VECTX+4,before=0,after=1,source='VideoEncCreate0x2a5c..64;VeInitialize0x531c..20'),dict(address=ctx[0]+0x270,before=0,after=top,source='Actual VeGetGroupRegAddr;JpegInit0x10e58')],physical_access=False,scope='Fixture flags and stored TOP pointer corrected from actual creator/router instructions. Existing initialized codec/ENCPP state preserved; no encoder pipeline or device replay.'),indent=2)+'\n');print('PASS corrected source-derived encoder context');return
 try:
  if not stage:ctx[0]=call('JpegEncOpen',CONFIG,0x21320);assert ctx[0];save('opened')
  if stage not in ['configured','initialized']:
   put(VALUE,75,'I');assert call('JpegSetParameter',ctx[0],0x200,VALUE)==0
   # Explicit one-MiB configured ring rather than the library's 8-MiB default.
   put(ctx[0]+0xd08,1048576,'I');save('configured')
  if stage!='initialized':assert call('JpegInit',ctx[0],CONFIG)==0;save('initialized')
  result=call('JpegEncMainFrame',ctx[0],FRAME);assert (result&0xffffffff)==(0xffffffff if a.case=='wait-error' else 0) and trigger[0] and waits[0]==1,(result,trigger[0],waits[0]);save('completed')
  routes=[get(ctx[0]+off) for off in [0x270,0x278,0x280]];assert routes[1:]==[MMIO+0xa00,MMIO+0xb00]
  manager=get(ctx[0]+0x268);descriptor=get(manager+0x60);n=get(descriptor+4,'I');base=get(ctx[0]+0x298);header=get(ctx[0]+0x14,'I');assert 0<header<1048576 and n<=1048576
  (a.output/'cpu-header.bin').write_bytes(bytes(u.mem_read(base,header)))
  if a.case!='wait-error':assert n==header+32+2;raw=bytes(u.mem_read(base,n));assert raw[:2]==b'\xff\xd8' and raw[-2:]==b'\xff\xd9';(a.output/'modeled-entropy-framing.bin').write_bytes(raw)
  summary=dict(passed=True,resume=resume_identity,model_sha256=model_identity,stub_map=stubnames,top_context_valid=bool(get(VECTX+4,'I')),case=a.case,routes=routes,header_bytes=header,frame_bytes=n,library_accepts_modeled_status=a.case!='wait-error',modeled_entropy_bytes=32,libraries={name:digest for name,base,elf,f,digest in images},inputs=input_identity,result=result,waits=waits[0],context=ctx[0],allocations=allocations,events=events,mmio=trace,physical_access=False,scope='Foreign instructions only in isolated Unicorn. libc/memory/thread operations and wait/MMIO modeled; no hardware codec arithmetic or transaction-drain proof.')
 except Exception as ex:
  summary=dict(passed=False,resume=resume_identity,model_sha256=model_identity,stub_map=stubnames,libraries={name:digest for name,base,elf,f,digest in images},inputs=input_identity,error=repr(ex),pc=hex(u.reg_read(UC_ARM64_REG_PC)),context=ctx[0],allocations=allocations,events=events,mmio=trace,physical_access=False)
 (a.output/'result.json').write_text(json.dumps(summary,indent=2)+'\n')
 for name,base,elf,f,digest in images:f.close()
 if not summary['passed']:raise RuntimeError(summary['error'])
 print('PASS instruction path',result,len(trace),len(allocations))
if __name__=='__main__':main()
