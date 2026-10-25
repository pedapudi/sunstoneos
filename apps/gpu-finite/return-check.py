#!/usr/bin/env python3
"""Model only the new GPU-local admission and secure return boundaries."""
import argparse,hashlib,importlib.util,json,re,struct,subprocess
from pathlib import Path
from unicorn import UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import *
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
entry=module('gpu_return_entry',ROOT/'checks/gpu-single-core-entry.py');layout=module('gpu_return_layout',ROOT/'checks/storage-read-native.py')
LAYOUTS = ['gpu_application', 'a733_board_session', 'pvr_gpu_service', 'pvr_gpu_stop',
 'a733_gpu_backend', 'a733_gpu_platform', 'a733_gpu_memory', 'a733_gpu_reserved',
 'a733_gpu_compute_reserved', 'a733_gpu_compute_binding', 'a733_native_usb', 'a733_pmic',
 'device_memory_pool', 'device_memory_allocation', 'device_memory_lease', 'device_memory_extent', 'gpu_return_result']
class Machine(entry.EntryMachine):
 def __init__(self,image,raw,variant):
  if not hasattr(image,'allocation_size'):
   from elftools.elf.elffile import ELFFile
   with image.path.open('rb') as stream:
    sizes={d.attributes['DW_AT_byte_size'].value for cu in ELFFile(stream).get_dwarf_info().iter_CUs() for d in cu.iter_DIEs() if d.tag=='DW_TAG_structure_type' and d.attributes.get('DW_AT_name') and d.attributes['DW_AT_name'].value==b'device_memory_allocation' and 'DW_AT_byte_size' in d.attributes}
   assert len(sizes)==1;image.allocation_size=sizes.pop()
  super().__init__(image,raw,image.address('kernel_main'));self.variant=variant;self.cleanup=False;self.cleans=[];self.stop_reason=None;self.reset=False;self.register_writes=[];self.restoring=False;self.gpu_reads=[];self.dram={};self.dirty=set();self.setway=False;self.late_ns={};self.publication_security=[]
  self.sysregs.update(ctr_el0=0x04040000,icc_igrpen1_el3=2,daif=0x3c0,sctlr_el3=0,rmr_el3=0)
  self.events.update(a for a,op in image.ops.items() if op.startswith(('dc ','dc\t','tlbi ','tlbi\t')) or op=='wfi')
  self.events.add(image.address('a733_smp_stop'));self.events.add(image.address('clean_data_cache'))
  self.cpu.hook_add(UC_HOOK_MEM_READ,self.mmio_read,begin=0x02000000,end=0x08ffffff)
  self.cpu.hook_add(UC_HOOK_MEM_WRITE,self.private_write)
  self.cpu.hook_add(UC_HOOK_MEM_WRITE,self.mmio_write,begin=0x02000000,end=0x08ffffff)
  if variant.startswith('initial-gpu-'):
   self.write({'initial-gpu-enabled':0x03400108,'initial-gpu-pending':0x03400208,'initial-gpu-active':0x03400308}[variant],0x80000000)
 def private_write(self,cpu,access,address,size,value,unused):
  if self.cleanup and self.el==4:
   for line in range(address&~63,(address+size+63)&~63,64):
    if line in self.dram:self.dirty.add(line)
 def enter_monitor(self,status=0,kind=1,operation=0x733):
  # Security-tagged model: only NS EL1 CVAC publishes NS dirty bytes to DRAM.
  if self.variant=='late-stack-write':
   line=(self.e.address('__exception_stack_top')-512)&~63
   self.cpu.mem_write(line,b'LATESTACK'+bytes(55));self.dirty.add(line);self.late_witness=line
  self.late_ns={line:bytes(self.cpu.mem_read(line,64)) for line in self.dirty}
  for line,data in self.dram.items():self.cpu.mem_write(line,data)
  self.el=12;self.cpu.reg_write(UC_ARM64_REG_PSTATE,0x3cd)
  self.cpu.reg_write(UC_ARM64_REG_SP,self.e.address('__monitor_stack_top'))
  for i,v in enumerate((operation,status,kind)):self.cpu.reg_write(UC_ARM64_REG_X0+i,v)
  self.cpu.reg_write(UC_ARM64_REG_PC,self.e.address('sd_single_core_exception'))
 def mmio_read(self,cpu,access,address,size,value,unused):
  if not self.cleanup:return
  self.gpu_reads.append(address)
  if self.variant=='restore-timeout' and self.restoring and address==0x03400000:
   self.write(address,self.read(address,4)|0x80000000);self.time+=48000000
 def mmio_write(self,cpu,access,address,size,value,unused):
  if not self.cleanup:return
  self.writes.append([address,size,value])
  if address==0x03400000:self.restoring=True
  # Model GIC write-one commands in their actual register banks.
  if 0x03400180<=address<0x03400200:self.write(address-0x80,self.read(address-0x80,4)&~value)
  if 0x03400100<=address<0x03400180:self.write(address,self.read(address,4)|value)
  if address==0x03470180:self.write(0x03470100,self.read(0x03470100,4)&~value)
 def hook(self,cpu,address,size,unused):
  text=self.e.ops.get(address,'');word=int.from_bytes(cpu.mem_read(address,4),'little')
  if address==self.e.address('a733_smp_stop'):
   self.stop_reason=cpu.reg_read(UC_ARM64_REG_X0);self.parked=True;cpu.emu_stop();return
  if self.cleanup:
   if address==self.e.address('clean_data_cache'):
    assert self.el==12 and not self.sysregs['sctlr_el1']&0x1005
    for line,data in self.late_ns.items():self.dram[line]=data;cpu.mem_write(line,data)
    self.setway=True;self.dirty.clear();self.ret();return
   if text.startswith('smc'):
    assert self.el==4 and not self.sysregs['sctlr_el1']&0x1005
    self.enter_monitor(cpu.reg_read(UC_ARM64_REG_X1));return
   if word&0xffffffe0==0xd53be020:
    if self.variant=='clock-regression' and len(self.cleans)==3:self.time-=100
    if self.variant=='clean-timeout' and len(self.cleans)==3:self.time+=48000000
    cpu.reg_write(UC_ARM64_REG_X0+(word&31),self.time);self.time+=24;cpu.reg_write(UC_ARM64_REG_PC,address+4);return
   read=re.fullmatch(r'mrs\s+x(\d+),\s*(\S+)',text,re.I)
   if read:
    key=read[2].lower();value=self.el if key=='currentel' else self.sysregs[key]
    cpu.reg_write(UC_ARM64_REG_X0+int(read[1]),value);cpu.reg_write(UC_ARM64_REG_PC,address+4);return
   write=re.fullmatch(r'msr\s+(\S+),\s*x(\d+|zr)',text,re.I)
   if write:
    key=write[1].lower();value=0 if write[2].lower()=='zr' else cpu.reg_read(UC_ARM64_REG_X0+int(write[2]));self.sysregs[key]=value;self.register_writes.append([key,value])
    cpu.reg_write(UC_ARM64_REG_PC,address+4)
    if key=='rmr_el3':assert value==2;self.reset=True;cpu.emu_stop()
    return
   op=re.fullmatch(r'dc\s+(cvac|civac),\s*x(\d+)',text,re.I)
   if op:
    line=cpu.reg_read(UC_ARM64_REG_X0+int(op[2]));self.cleans.append(line)
    ns=self.el==4 and self.variant!='secure-only-maintenance'
    self.publication_security.append('NS' if ns else 'Secure')
    if ns and not(self.variant=='missing-receipt-line' and line==self.e.address('gpu_return_receipt')&~63):
     assert not self.translate(line,False)&1
     self.dram[line]=bytes(cpu.mem_read(line,64));self.dirty.discard(line)
    cpu.reg_write(UC_ARM64_REG_PC,address+4);return
   if text.startswith('tlbi ' ) or text.startswith('tlbi\t'):cpu.reg_write(UC_ARM64_REG_PC,address+4);return
   assert text!='eret','EL1 resumed during return cleanup'
  return super().hook(cpu,address,size,unused)
 def call(self,name,*args):
  self.finished=False;self.parked=False
  self.cpu.reg_write(UC_ARM64_REG_SP,self.e.address('__exception_stack_top'))
  self.cpu.reg_write(UC_ARM64_REG_LR,0x43fff000)
  for i,v in enumerate(args):self.cpu.reg_write(UC_ARM64_REG_X0+i,v)
  self.cpu.emu_start(self.e.address(name),0,count=10000000)
  assert self.finished or self.parked
  return self.cpu.reg_read(UC_ARM64_REG_X0)
 def prepare(self):
  self.cpu.emu_start(self.e.address('sd_single_core_entry'),0,count=10000000)
  if self.variant.startswith('initial-gpu-'):assert self.parked and self.stop_reason==4;return
  assert self.finished and not self.parked
  self.call('platform_init')
  lo=self.e.address('__gpu_retained_start');hi=self.e.address('__gpu_retained_end')
  sealed=self.e.address('sealed_session');assert lo<=sealed<sealed+592<=hi
  assert bytes(self.cpu.mem_read(sealed,592))==self.raw
  # Device execution/FreeRTOS scheduling are the accepted modeled boundary.
  # These post-CLOSE records are not physical admission or GPU output evidence.
  app=self.e.address('app');g=self.e.layouts['gpu_application'];s=app+g['service'];f=self.e.layouts['pvr_gpu_service']
  def put(base,typ,key,value,n=4):self.write(base+self.e.layouts[typ][key],value,n)
  for key,value in [('phase',5),('owner',8),('irq_masked',1)]:put(s,'pvr_gpu_service',key,value)
  put(s+f['stop'],'pvr_gpu_stop','phase',10);put(s+f['stop'],'pvr_gpu_stop','isolation_observed',1)
  put(app+g['backend'],'a733_gpu_backend','phase',5);put(app+g['power'],'a733_gpu_platform','phase',6)
  firmware=app+g['firmware'];compute=app+g['compute'];fl=self.e.layouts['a733_gpu_reserved'];cl=self.e.layouts['a733_gpu_compute_reserved']
  put(firmware+fl['memory'],'a733_gpu_memory','phase',1)
  self.cpu.mem_write(app+g['session'],self.raw);bl=self.e.layouts['a733_board_session'];c=bytes(self.cpu.mem_read(self.e.address('gpu_admission'),288));startup,diag=struct.unpack_from('<2Q',c,88)
  self.write(app+g['session']+(bl['reload_after_ticks']-8),startup-24000000,8);self.write(app+g['session']+bl['deadline_us'],diag//24,8)
  self.write(app+g['result'],1);self.write(self.e.address('pmic_parked'),1)
  pmic=app+g['shared']+self.e.layouts['a733_native_usb']['pmic'];put(pmic,'a733_pmic','active',8)
  self.write(app+g['compute_sequence'],1);self.write(app+g['compute_sequence']+4,2)
  self.output=[((17+(i+1)*3)^(0x9e3779b9+(i<<16)))&0xffffffff for i in range(32)]*2;self.cpu.mem_write(app+g['compute_words'],struct.pack('<64I',*self.output))
  terminal='GPU_STAGE=STOPPED\nGPU_COMPUTE_WORDS=\n'
  for n in range(2):terminal+='GPU_SEQUENCE=0x%016x\n'%(n+1)+''.join('0x%016x\n'%v for v in self.output[n*32:n*32+32])
  terminal+='PASS GPU two copied results, CLOSE and parked providers\n'
  log=self.e.address('__gpu_platform_log');self.cpu.mem_write(log+256,terminal.encode()+b'\0');self.write(log+24,len(terminal),8)
  self.write(0x03400108,0);self.write(0x03400208,0);self.write(0x03400308,0);self.write(0x02002b20,0);self.write(0x02002b24,0);self.write(0x07065000,0);self.write(0x07065008,0)
  # Construct each allocator ledger and lease with the actual portable producer.
  wb=self.e.address('boot_pool');nc=self.e.address('uncached_pool')
  if 'device_memory_pool_init_typed' in self.e.symbols:
   assert self.call('device_memory_pool_init_typed',wb,0x48000000,0x48000000,0x30000000,1,0)==1
   assert self.call('device_memory_pool_init_typed',nc,0x78000000,0x78000000,0x1000000,0,1)==1
  else:
   # Historical frozen images expose the earlier untyped producer only.
   assert self.call('device_memory_pool_init',wb,0x48000000,0x48000000,0x30000000)==1
   assert self.call('device_memory_pool_init',nc,0x78000000,0x78000000,0x1000000)==1
  put(firmware,'a733_gpu_reserved','wb_pool',wb,8);put(firmware,'a733_gpu_reserved','nc_pool',nc,8)
  binding=compute+cl['binding']
  for key,value in [('session',compute),('firmware',firmware),('service',s),('wb',wb),('nc',nc),('owner',10)]:put(binding,'a733_gpu_compute_binding',key,value,8)
  for pool,owner,pages,obj,typ,lease,extent in [(wb,9,34,firmware,'a733_gpu_reserved','wb_lease','wb_extent'),(nc,9,45,firmware,'a733_gpu_reserved','nc_lease','nc_extent'),(wb,10,88,compute,'a733_gpu_compute_reserved','payload_lease','payload_extent'),(nc,10,6,compute,'a733_gpu_compute_reserved','firmware_lease','firmware_extent'),(wb,10,12,compute,'a733_gpu_compute_reserved','table_lease','table_extent')]:
   fields=self.e.layouts[typ];assert self.call('device_memory_acquire',pool,owner,pages*4096,obj+fields[lease],obj+fields[extent])==1
   token=obj+fields[lease];slot=self.read(token+self.e.layouts['device_memory_lease']['slot'],4);allocation=pool+self.e.layouts['device_memory_pool']['allocations']+slot*self.e.allocation_size;put(allocation,'device_memory_allocation','state',2)
  self.produced_extents={name:list(struct.unpack('<3Q',self.cpu.mem_read(base+self.e.layouts[typ][name],24)))
      for base,typ,names in [(firmware,'a733_gpu_reserved',['wb_extent','nc_extent']),(compute,'a733_gpu_compute_reserved',['payload_extent','table_extent','firmware_extent'])] for name in names}
  self.wb_end=max(self.produced_extents[n][0]+self.produced_extents[n][2] for n in ['wb_extent','payload_extent','table_extent'])
  assert self.call('gpu_return_allocations')==1, self.produced_extents
  if self.variant=='extra-allocation':assert self.call('device_memory_acquire',wb,99,4096,0x43001000,0x43001040)==1
  if self.variant=='lease-generation':self.write(compute+cl['table_lease']+self.e.layouts['device_memory_lease']['generation'],99,8)
  if self.variant=='changed-extent':self.write(compute+cl['table_extent']+self.e.layouts['device_memory_extent']['bytes'],13*4096,8)
  if self.variant=='changed-geometry':self.write(wb+self.e.layouts['device_memory_pool']['backing']+self.e.layouts['device_memory_extent']['bytes'],0x200000,8)
  if self.variant=='not-stopped':put(s,'pvr_gpu_service','phase',4)
  if self.variant=='pmic-unparked':self.write(self.e.address('pmic_parked'),0)
  if self.variant=='changed-session':self.write(app+g['session']+bl['lease'],99,8)
  if self.variant=='gpu-gate-on':self.write(0x02002b20,0x80000000)
  if self.variant=='gpu-domain-on':self.write(0x07065008,8)
  if self.variant=='core-policy-on':self.write(0x07066000,8)
  if self.variant=='core-status-on':self.write(0x07066008,8)
  if self.variant=='core-dynamic':self.write(0x07066000,0x100)
  if self.variant=='pending-irq':self.write(0x03400208,0x80000000)
  if self.variant=='cache-geometry':self.sysregs['ctr_el0']=0x03040000
  assert self.sysregs['tcr_el1']==entry.shared.TCR,self.sysregs;self.cleanup=True;self.el=4;self.sysregs['spsr_el3']=0x85 if self.variant=='wrong-caller-profile' else 0x84;self.sysregs.update(sctlr_el1=0x30d01805,sctlr_el3=0,scr_el3=0x505,esr_el3=0x17<<26)
  lo=self.e.address('__gpu_private_start');hi=(self.e.address('__gpu_private_end')+63)&~63
  for line in list(range(lo,hi,64))+list(range(0x48000000,self.wb_end,64)):
   self.dram[line]=bytes(64);self.dirty.add(line)
 def finish(self):
  self.finished=False;self.parked=False;self.stop_at=None
  self.cpu.reg_write(UC_ARM64_REG_PSTATE,0x84)
  self.cpu.reg_write(UC_ARM64_REG_SP,self.e.address('__exception_stack_top'))
  self.cpu.reg_write(UC_ARM64_REG_LR,0x43fff000);self.cpu.reg_write(UC_ARM64_REG_X0,1 if self.variant=='nonzero-exit' else 0)
  if self.variant in ('unexpected-smc','secure-timeout'):
   self.enter_monitor(0,2 if self.variant=='secure-timeout' else 1,0x734 if self.variant=='unexpected-smc' else 0x733)
   target=self.e.address('sd_single_core_exception')
  else:target=self.e.address('platform_exit')
  self.cpu.emu_start(target,0,count=6000000)
  assert self.reset or self.parked,(self.variant,hex(self.cpu.reg_read(UC_ARM64_REG_PC)))
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True);p.add_argument('--cases',nargs='+');a=p.parse_args();e=a.evidence.resolve();path=e/'build/return.elf';image=layout.Image(path,layout_names=LAYOUTS);image.path=path;image.ops={}
 from elftools.elf.elffile import ELFFile
 for line in subprocess.check_output(['/usr/lib/llvm-21/bin/llvm-objdump','-d','--no-show-raw-insn',str(path)],text=True).splitlines():
  match=re.match(r'^\s*([0-9a-f]+):\s+(.+)$',line)
  if match:image.ops[int(match[1],16)]=match[2].strip()
 raw=bytearray((Path.home()/'.cache/sunstoneos/evidence/gpu-single-core-20261018/provider-valid-fixture/fixtures/session-template.bin').read_bytes());raw[48:80]=bytes.fromhex(json.loads((e/'manifest.json').read_text())['build_id'])
 names=['normal','initial-gpu-enabled','initial-gpu-pending','initial-gpu-active','not-stopped','pmic-unparked','extra-allocation','lease-generation','changed-extent','changed-geometry','changed-session','gpu-gate-on','gpu-domain-on','core-policy-on','core-status-on','core-dynamic','pending-irq','cache-geometry','wrong-caller-profile','clock-regression','clean-timeout','restore-timeout','nonzero-exit','unexpected-smc','secure-timeout','secure-only-maintenance','missing-receipt-line','late-stack-write'];cases={}
 if a.cases and (e/'return-result.json').exists():
  prior=json.loads((e/'return-result.json').read_text());assert prior['elf_sha256']==hashlib.sha256(path.read_bytes()).hexdigest();cases=prior['cases']
 for variant in names:
  if a.cases and variant not in a.cases:continue
  m=Machine(image,bytes(raw),variant);m.prepare()
  if not variant.startswith('initial-gpu-'):
   (e/(variant+'-producers.json')).write_text(json.dumps({'elf_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'extents':m.produced_extents,'pool_types':'actual DMA32/write-back WB and NONE/noncacheable NC producers'},indent=2)+'\n')
   m.finish()
  if variant in ('normal','late-stack-write'):
   assert m.reset and not m.parked and m.read(0x08001004,4)==0x20 and m.read(0x08001008,4)==0
   assert not m.sysregs['sctlr_el1']&0x1005
   for n in ['cntps_ctl_el1','cntp_ctl_el0','cntv_ctl_el0','icc_igrpen0_el1','icc_igrpen1_el3']:assert m.sysregs[n]==0,n
   lo=image.address('__gpu_private_start');hi=(image.address('__gpu_private_end')+63)&~63;expected=list(range(lo,hi,64))+list(range(0x48000000,m.wb_end,64));assert m.cleans==expected;
   if variant=='late-stack-write':assert m.setway and m.dram[m.late_witness].startswith(b'LATESTACK') and not m.dirty
   assert m.setway and all(x=='NS' for x in m.publication_security)
   if variant=='normal':
    for name,address,size in [('monitor',0x42000000,128),('platform',image.address('__gpu_platform_log'),16384),('receipt',image.address('gpu_return_receipt'),104),('packet',0x42000100,320)]:
     (e/('normal-'+name+'.bin')).write_bytes(bytes(m.cpu.mem_read(address,size)))
   if variant=='normal':
    prepare=module('gpu_current_return_prepare',ROOT/'apps/gpu-finite/prepare.py');parser=module('gpu_current_return_report',ROOT/'apps/gpu-finite/report.py')
    manifest=json.loads((e/'manifest.json').read_text());geometry=prepare.image_profile(path,manifest)[3]
    assert geometry['wb_end']==m.wb_end and geometry['wb_start']==m.produced_extents['wb_extent'][0]
    raw_reports=[(e/('normal-'+n+'.bin')).read_bytes() for n in ('monitor','platform','receipt','packet')]
    receipt=struct.unpack('<13Q',raw_reports[2]);prepared={'epoch':receipt[1],'generation':receipt[2],'lease':receipt[3],'geometry':geometry}
    assert parser.validate_reports(*raw_reports,prepared)['functional_success']
    # The receipt carries the native span, so the historical span must not validate it.
    try:parser.validate_reports(*raw_reports,dict(prepared,geometry=dict(geometry,wb_end=0x48086000)))
    except ValueError as error:assert str(error)=='GPU receipt ownership/publication extent differs.'
    else:raise AssertionError('Historical span accepted a current receipt')
    # Corrupt the immutable producer metadata while rebinding the file hash;
    # rejection must come from the marker, not a stale manifest digest.
    with path.open('rb') as stream:
     elf=ELFFile(stream);symtab=elf.get_section_by_name('.symtab');entries=list(symtab.iter_symbols())
     index=next(i for i,x in enumerate(entries) if x.name=='gpu_return_wb_geometry');address=int(entries[index]['st_value'])
     other=next(i for i,x in enumerate(entries) if x.name=='last_tick')
     loads=[(i,x) for i,x in enumerate(elf.iter_segments()) if x['p_type']=='PT_LOAD']
     seg,segment=next((i,x) for i,x in loads if int(x['p_vaddr'])<=address<int(x['p_vaddr'])+int(x['p_filesz']))
     offset=int(segment['p_offset'])+address-int(segment['p_vaddr'])
     entry=lambda i:int(symtab['sh_offset'])+i*int(symtab['sh_entsize'])
     phdr=int(elf.header['e_phoff'])+seg*int(elf.header['e_phentsize'])
     unbacked=image.address('__bss_start');assert not any(int(x['p_vaddr'])<=unbacked<int(x['p_vaddr'])+int(x['p_filesz']) for _,x in loads)
    data=path.read_bytes();name=struct.unpack_from('<I',data,entry(index))[0];flags=struct.unpack_from('<I',data,phdr+4)[0]
    malformed=[('changed-span',[('<Q',offset+8,m.wb_end+4096)],'Unknown GPU WB geometry.'),
     ('changed-start',[('<Q',offset,0x48001000)],'Unknown GPU WB geometry.'),
     ('marker-size',[('<Q',entry(index)+16,8)],'Malformed GPU WB geometry marker.'),
     ('marker-duplicate',[('<I',entry(other),name)],'Malformed GPU WB geometry marker.'),
     ('marker-writable',[('<I',phdr+4,flags|2)],'GPU WB geometry must be immutable and file-backed.'),
     ('marker-unbacked',[('<Q',entry(index)+8,unbacked)],'GPU WB geometry must be immutable and file-backed.')]
    for label,edits,message in malformed:
     corrupt=bytearray(data)
     for fmt,at,value in edits:struct.pack_into(fmt,corrupt,at,value)
     bad=e/(label+'.elf');bad.write_bytes(corrupt)
     try:prepare.image_profile(bad,dict(manifest,final_elf=hashlib.sha256(corrupt).hexdigest()))
     except ValueError as error:assert str(error)==message,(label,str(error))
     else:raise AssertionError(label+' accepted')
    # A frozen image built before the marker keeps its historical span.
    old=Path.home()/'.cache/sunstoneos/evidence/gpu-returning-repeat-fix-20261019'
    historical=prepare.image_profile(old/'build/return.elf',json.loads((old/'manifest.json').read_text()))[3]
    assert (historical['wb_start'],historical['wb_end'])==(0x48000000,0x48086000)
    (e/'host-geometry.json').write_text(json.dumps({'image_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'passed':True,'geometry':geometry,'cases':['actual-writer-profile-report','historical-span-refused-for-current-receipt']+[x[0]+'-refused' for x in malformed]+['historical-marker-absent'],'historical_image_sha256':hashlib.sha256((old/'build/return.elf').read_bytes()).hexdigest(),'scope':'Actual native monitor/receipt/packet/final log header through the host parser; malformed markers are single-field edits of the final ELF with a rebound manifest digest. GPU arithmetic and terminal log modeled. No physical access.'},indent=2)+'\n')
   report=image.layouts['gpu_return_result'];assert m.read(0x42000100)==0x3154455255504741;assert list(struct.unpack('<64I',m.cpu.mem_read(0x42000100+report['words'],256)))==m.output
  else:assert m.parked and not m.reset and not any(x[0] in (0x08001004,0x08001008) for x in m.writes),variant
  if variant in ['not-stopped','pmic-unparked','extra-allocation','lease-generation','changed-extent','changed-geometry','gpu-gate-on','gpu-domain-on','core-policy-on','core-status-on','core-dynamic','pending-irq','cache-geometry','nonzero-exit']:assert not any(0x48000000<=x<0x78000000 for x in m.cleans),variant
  cases[variant]={'passed':True,'reset_requested':m.reset,'retained_status':m.stop_reason,'clean_lines':len(m.cleans),'clean_wb_device_lines':sum(0x48000000<=x<0x78000000 for x in m.cleans),'mmio_reads':sorted(set(m.gpu_reads)),'register_writes':m.register_writes,'publication_security_spaces':sorted(set(m.publication_security)),'final_setway_executed':m.setway,'post_range_clean_ns_dirty_lines':len(m.late_ns)}
  (e/'return-result.json').write_text(json.dumps({'passed':True,'elf_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'cases':cases,'physical_access':False,'scope':'Actual admission/session sealing, retained placement/BSS clearing, allocator producers, success-only exception routing, ownership/extent/session/device-off gates, cache-address loops, GIC restore and RMR instruction path. GPU completion state, mappings, security-tagged NS dirty-cache/DRAM publication, timers/GIC/system registers and reset outcome modeled. Accepted eight device cases not replayed; no physical GPU or ROM return evidence.'},indent=2)+'\n')
  print('PASS GPU returning handoff:',variant)
if __name__=='__main__':main()
