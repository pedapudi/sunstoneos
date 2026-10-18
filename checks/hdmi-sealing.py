#!/usr/bin/env python3
"""Only hdmi admission-to-consumption binding; providers and publication are modeled."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
ROOT=Path(__file__).resolve().parents[1]
def run(image,out):
 out.mkdir(parents=True,exist_ok=False)
 spec=importlib.util.spec_from_file_location('hdmi_seal_prepare',ROOT/'apps/hdmi/prepare.py');prep=importlib.util.module_from_spec(spec);spec.loader.exec_module(prep)
 image_bytes=image.read_bytes();parts,entry,identity,session_bytes=prep.image_data(image_bytes)
 def resolve(d):
  while d.tag in ('DW_TAG_typedef','DW_TAG_const_type','DW_TAG_volatile_type','DW_TAG_restrict_type'):d=d.get_DIE_from_attribute('DW_AT_type')
  return d
 def sizeof(d):
  d=resolve(d)
  if 'DW_AT_byte_size'in d.attributes:return int(d.attributes['DW_AT_byte_size'].value)
  assert d.tag=='DW_TAG_array_type';n=1
  for item in d.iter_children():
   if item.tag=='DW_TAG_subrange_type':n*=int(item.attributes['DW_AT_count'].value) if 'DW_AT_count'in item.attributes else int(item.attributes['DW_AT_upper_bound'].value)+1
  return n*sizeof(d.get_DIE_from_attribute('DW_AT_type'))
 def walk(d,prefix='',base=0):
  fields={}
  for child in resolve(d).iter_children():
   if child.tag!='DW_TAG_member':continue
   name=prefix+child.attributes['DW_AT_name'].value.decode();t=resolve(child.get_DIE_from_attribute('DW_AT_type'));offset=base+int(child.attributes['DW_AT_data_member_location'].value)
   assert t.tag!='DW_TAG_pointer_type';fields[name]=dict(offset=offset,size=sizeof(t))
   if t.tag=='DW_TAG_structure_type':fields.update(walk(t,name+'.',offset))
  return fields
 session_layouts=[]
 with image.open('rb') as f:
  elf=ELFFile(f);symbols={};functions={};layouts={}
  for s in elf.get_section_by_name('.symtab').iter_symbols():symbols.setdefault(s.name,set()).add(int(s['st_value']))
  segments=[(int(s['p_vaddr']),bytes(s.data())) for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
  for cu in elf.get_dwarf_info().iter_CUs():
   unit=cu.get_top_DIE().attributes['DW_AT_name'].value.decode()
   for d in cu.iter_DIEs():
    n=d.attributes.get('DW_AT_name')
    if not n:continue
    name=n.value.decode()
    if d.tag=='DW_TAG_subprogram' and 'DW_AT_low_pc' in d.attributes:functions.setdefault((unit,name),set()).add(int(d.attributes['DW_AT_low_pc'].value))
    if d.tag=='DW_TAG_structure_type' and name=='hdmi_session' and 'DW_AT_byte_size'in d.attributes:
     value=dict(size=sizeof(d),fields=walk(d))
     if value not in session_layouts:session_layouts.append(value)
    if d.tag=='DW_TAG_structure_type':
     fields={c.attributes['DW_AT_name'].value.decode():int(c.attributes['DW_AT_data_member_location'].value) for c in d.iter_children() if c.tag=='DW_TAG_member' and 'DW_AT_name'in c.attributes and 'DW_AT_data_member_location'in c.attributes}
     if fields:layouts.setdefault(name,[]).append(fields)
 def address(name,suffix=None):
  values=symbols.get(name,set())-{0}
  if suffix:
   values=set().union(*(v for (u,n),v in functions.items() if n==name and u.endswith(suffix))) if any(n==name and u.endswith(suffix) for u,n in functions) else set()
  assert len(values)==1,(name,suffix,values);return next(iter(values))
 names={n:address(n) for n in ('app','a733_cached_runtime','hdmi_session_valid','hdmi_session_prepare','hdmi_admitted_session','a733_smp_contract_valid','a733_smp_counter','a733_smp_device_preparation_admitted','a733_counter_clock_init','platform_panic')}
 names['a733_smp_diagnostic_start']=address('a733_smp_diagnostic_start','apps/hdmi/main.c')
 app_layout=layouts['hdmi_application'];assert all(v==app_layout[0] for v in app_layout);app_layout=app_layout[0]
 assert len(session_layouts)==1;layout=session_layouts[0];assert layout['size']==session_bytes
 mode,offset=prep.framebuffer_metadata(image_bytes);assert mode==1 and offset==layout['fields']['framebuffer']['offset']
 raw=bytearray(session_bytes)
 def put(field,value):
  item=layout['fields'][field];value=value if isinstance(value,bytes) else value.to_bytes(item['size'],'little');assert len(value)==item['size'];raw[item['offset']:item['offset']+item['size']]=value
 for field,value in dict(magic=prep.SESSION_MAGIC,version=1,bytes=session_bytes,epoch=31,generation=37,lease=41,supervisor=43,owner=47,pck_client=3).items():put(field,value)
 for field,value in {'request.mode':4,'request.range':0,'request.source':1,'request.initial.id':53,'request.initial.generation':59,'pmic.owner':61,'pmic.identity':37,'framebuffer.authority':67}.items():put(field,value)
 put('request.initial.words',struct.pack('<8I',8,0xa0,0x00110001,0x10000000,0x76543210,0,0,0x001f0040))
 put('framebuffer.fabric',struct.pack('<3I',0,0x80000000,0x80000000));put('framebuffer.fabric_hz',struct.pack('<3I',24000000,24000000,24000000));put('framebuffer.iommu_gates',struct.pack('<2I',0x10007,0x10007));put('image',identity)
 record=dict(schema='a733-hdmi-supervised-admission-v1',image_sha256=hashlib.sha256(image_bytes).hexdigest(),epoch=31,generation=37,lease=41,supervisor=43,framebuffer='720p60-rgb565-retained-v1',framebuffer_authority=67)
 for field,kind in [('analog','analog'),('power','power'),('supervision','supervision'),('framebuffer.isolation','isolation'),('framebuffer.memory','memory')]:
  data=json.dumps(dict(schema='hdmi-binding-model-v1',kind=kind,image_sha256=record['image_sha256'],physical_access=False,purpose='Software fixture only; no physical authority.'),sort_keys=True).encode();filename=kind+'.json';(out/filename).write_bytes(data);record[kind]=filename;put(field,hashlib.sha256(data).digest())
 admission=json.dumps(record,sort_keys=True).encode();(out/'admission.json').write_bytes(admission);put('evidence',hashlib.sha256(admission).digest());raw=bytes(raw);(out/'nominal-session.bin').write_bytes(raw)
 accepted=prep.validate(image,out/'nominal-session.bin',out/'admission.json');assert accepted[3]==raw and accepted[2]==identity
 profile=dict(epoch=31,generation=37,lease=41)
 _,contract=prep.request_bytes(entry,identity,raw,31,37,41,43);contract=bytearray(contract[:224]);struct.pack_into('<Q',contract,0,0x4153373348444331);struct.pack_into('<4Q',contract,80,100,24000100,480000100,480000100)
 assert raw[48:80]==identity
 results={}
 for variant in ('unsealed-refused','precopy-owner-refused','precopy-evidence-refused','publication-failed','sealed-copy-consumed'):
  cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.mem_map(0x40000000,0x4000000);cpu.mem_map(0x50000000,0x1000000)
  for a,data in segments:cpu.mem_write(a,data)
  cpu.mem_write(0x41f00000,raw);cpu.mem_write(0x50001000,bytes(contract));cpu.mem_write(names['a733_cached_runtime'],struct.pack('<3Q',profile['epoch'],24000100,480000100))
  finished=[];providers=[];panics=[];publications=[]
  def returned(value):cpu.reg_write(UC_ARM64_REG_X0,value&((1<<64)-1));cpu.reg_write(UC_ARM64_REG_PC,cpu.reg_read(UC_ARM64_REG_LR))
  def hook(machine,a,size,unused):
   if a==0x50fff000:finished.append(True);machine.emu_stop()
   elif a==names['a733_smp_counter']:returned(101)
   elif a==names['a733_smp_device_preparation_admitted']:returned(1)
   elif a==publish:
    first,n,d=(machine.reg_read(x) for x in (UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2));assert n>=len(raw)+8 and d==24000100
    assert bytes(machine.mem_read(first,len(raw)))==raw and bytes(machine.mem_read(first+len(raw),8))==struct.pack('<2I',1,1)
    publications.append(n);returned(0 if variant=='publication-failed' else 1)
   elif a==names['platform_panic']:panics.append(True);finished.append(True);machine.emu_stop()
   elif a==names['a733_counter_clock_init']:
    providers.append(True);assert bytes(machine.mem_read(names['app']+app_layout['session'],len(raw)))==raw
    finished.append(True);machine.emu_stop()
  publish=address('a733_cpu_coherence_publish');cpu.hook_add(UC_HOOK_CODE,hook)
  def call(name,arg=0,arg1=101):
   finished.clear();cpu.reg_write(UC_ARM64_REG_X0,arg);cpu.reg_write(UC_ARM64_REG_X1,arg1);cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);cpu.reg_write(UC_ARM64_REG_LR,0x50fff000);cpu.emu_start(names[name],0,count=1000000);assert finished,(variant,name);return cpu.reg_read(UC_ARM64_REG_X0)&0xffffffff
  assert call('a733_smp_contract_valid',0x50001000)==1
  if variant!='unsealed-refused':
   changed=bytearray(raw)
   if variant=='precopy-owner-refused':
    for field in ('owner',):
     item=layout['fields'][field];changed[item['offset']:item['offset']+item['size']]=(49).to_bytes(item['size'],'little')
   if variant=='precopy-evidence-refused':
    for field in ('analog',):changed[layout['fields'][field]['offset']]^=1
   if variant in ('precopy-owner-refused','precopy-evidence-refused'):assert bytes(changed)!=raw
   cpu.mem_write(0x41f00000,bytes(changed));assert call('hdmi_session_valid',0x41f00000)==1
   sealed=call('hdmi_session_prepare',0x50001000);assert sealed==(variant=='sealed-copy-consumed')
   if not sealed:assert call('hdmi_admitted_session')==0 and call('hdmi_session_prepare',0x50001000)==0
   else:
    changed=bytearray(raw)
    for field in ('owner',):changed[layout['fields'][field]['offset']]^=1
    changed[layout['fields']['framebuffer.memory']['offset']]^=1;cpu.mem_write(0x41f00000,bytes(changed))
  call('a733_smp_diagnostic_start');assert bool(providers)==(variant=='sealed-copy-consumed') and bool(panics)!=(variant=='sealed-copy-consumed')
  results[variant]=dict(exit=0,provider_calls=len(providers),publications=publications)
 (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,cases=results,nominal_session_sha256=hashlib.sha256(raw).hexdigest(),image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),scope='Actual seal, copied hash, ready receipt and earliest consumer. Cache publication/provider readiness/clock are modeled; no display provider or output execution.'),indent=2)+'\n')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('image','output'):p.add_argument('--'+name,type=Path,required=True)
 a=p.parse_args();run(a.image,a.output)
