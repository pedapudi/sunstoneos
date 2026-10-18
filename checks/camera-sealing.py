#!/usr/bin/env python3
"""Only camera admission-to-consumption binding; providers and publication are modeled."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
ROOT=Path(__file__).resolve().parents[1]
def run(image,session,out):
 out.mkdir(parents=True,exist_ok=False)
 spec=importlib.util.spec_from_file_location('camera_seal_prepare',ROOT/'apps/camera/prepare.py');prep=importlib.util.module_from_spec(spec);spec.loader.exec_module(prep)
 raw=session.read_bytes();parts,entry,identity,layout=prep.image_data(image.read_bytes());profile=prep.session_profile(raw,layout,identity)
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
    if d.tag=='DW_TAG_structure_type':
     fields={c.attributes['DW_AT_name'].value.decode():int(c.attributes['DW_AT_data_member_location'].value) for c in d.iter_children() if c.tag=='DW_TAG_member' and 'DW_AT_name'in c.attributes and 'DW_AT_data_member_location'in c.attributes}
     if fields:layouts.setdefault(name,[]).append(fields)
 def address(name,suffix=None):
  values=symbols.get(name,set())-{0}
  if suffix:
   values=set().union(*(v for (u,n),v in functions.items() if n==name and u.endswith(suffix))) if any(n==name and u.endswith(suffix) for u,n in functions) else set()
  assert len(values)==1,(name,suffix,values);return next(iter(values))
 names={n:address(n) for n in ('app','a733_cached_runtime','camera_session_valid','camera_session_prepare','camera_admitted_session','a733_smp_contract_valid','a733_smp_counter','a733_smp_device_preparation_admitted','a733_native_usb_init','platform_panic')}
 names['a733_smp_diagnostic_start']=address('a733_smp_diagnostic_start','apps/camera/main.c')
 app_layout=layouts['camera_application'];assert all(v==app_layout[0] for v in app_layout);app_layout=app_layout[0]
 _,contract=prep.request_bytes(entry,identity,raw,profile);contract=bytearray(contract[:224]);struct.pack_into('<Q',contract,0,0x4153373343414333);struct.pack_into('<4Q',contract,80,100,24000100,1440000100,1440000100)
 # Complete nominal inputs use the actual linked build ID and producer-validated evidence hashes.
 assert raw[48:80]==identity
 results={}
 for variant in ('unsealed-refused','precopy-owner-refused','precopy-evidence-refused','publication-failed','sealed-copy-consumed'):
  cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.mem_map(0x40000000,0x4000000);cpu.mem_map(0x50000000,0x1000000)
  for a,data in segments:cpu.mem_write(a,data)
  cpu.mem_write(0x41f00000,raw);cpu.mem_write(0x50001000,bytes(contract));cpu.mem_write(names['a733_cached_runtime'],struct.pack('<3Q',profile['epoch'],24000100,1440000100))
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
   elif a==names['a733_native_usb_init']:
    providers.append(True);assert bytes(machine.mem_read(names['app']+app_layout['admitted'],len(raw)))==raw
    runtime=bytearray(raw)
    for field in ('deadline_us','camera.deadline_us','media.deadline_us','shared.pmic.startup_deadline','shared.usb.deadline_us'):
     item=layout['fields'][field];runtime[item['offset']:item['offset']+item['size']]=(1440000100//24).to_bytes(item['size'],'little')
    assert bytes(machine.mem_read(names['app']+app_layout['session'],len(raw)))==runtime
    finished.append(True);machine.emu_stop()
  publish=address('a733_cpu_coherence_publish');cpu.hook_add(UC_HOOK_CODE,hook)
  def call(name,arg=0,arg1=101):
   finished.clear();cpu.reg_write(UC_ARM64_REG_X0,arg);cpu.reg_write(UC_ARM64_REG_X1,arg1);cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);cpu.reg_write(UC_ARM64_REG_LR,0x50fff000);cpu.emu_start(names[name],0,count=1000000);assert finished,(variant,name);return cpu.reg_read(UC_ARM64_REG_X0)&0xffffffff
  assert call('a733_smp_contract_valid',0x50001000)==1
  if variant!='unsealed-refused':
   changed=bytearray(raw)
   if variant=='precopy-owner-refused':
    for field in ('camera.owner','media.owner'):
     item=layout['fields'][field];changed[item['offset']:item['offset']+item['size']]=(7).to_bytes(item['size'],'little')
   if variant=='precopy-evidence-refused':
    for field in ('evidence','camera.evidence'):changed[layout['fields'][field]['offset']]^=1
   cpu.mem_write(0x41f00000,bytes(changed));assert call('camera_session_valid',0x41f00000)==1
   sealed=call('camera_session_prepare',0x50001000);assert sealed==(variant=='sealed-copy-consumed')
   if not sealed:assert call('camera_admitted_session')==0 and call('camera_session_prepare',0x50001000)==0
   else:
    changed=bytearray(raw)
    for field in ('camera.owner','media.owner'):changed[layout['fields'][field]['offset']]^=1
    changed[layout['fields']['memory']['offset']]^=1;cpu.mem_write(0x41f00000,bytes(changed))
  call('a733_smp_diagnostic_start');assert bool(providers)==(variant=='sealed-copy-consumed') and bool(panics)!=(variant=='sealed-copy-consumed')
  results[variant]=dict(exit=0,provider_calls=len(providers),publications=publications)
 (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,cases=results,nominal_session_sha256=hashlib.sha256(raw).hexdigest(),image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),scope='Actual seal, copied hash, ready receipt and earliest consumer. Cache publication/provider readiness/clock are modeled; no camera/provider/pixel execution.'),indent=2)+'\n')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('image','session','output'):p.add_argument('--'+name,type=Path,required=True)
 a=p.parse_args();run(a.image,a.session,a.output)
