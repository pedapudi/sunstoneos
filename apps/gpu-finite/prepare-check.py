#!/usr/bin/env python3
"""Check native GPU packaging and damaged input refusals without process records."""
import sys
import argparse,copy,hashlib,importlib.util,json,struct
from pathlib import Path
if not __debug__:raise SystemExit('GPU preparation checks require assertions enabled.')
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'checks'))
from harness import artifact_refused
spec=importlib.util.spec_from_file_location('finite_prepare_check',ROOT/'apps/gpu-finite/prepare.py')
prep=importlib.util.module_from_spec(spec);spec.loader.exec_module(prep)
def marker_cases(current,historical,e):
 """Refuse every malformed write-back span marker, keeping the historical span only for untyped images."""
 from elftools.elf.elffile import ELFFile
 def sha(data):return hashlib.sha256(data).hexdigest()
 def load(directory):
  path=directory/'build/return.elf';data=path.read_bytes();manifest={}
  with path.open('rb') as stream:
   elf=ELFFile(stream);symtab=elf.get_section_by_name('.symtab');entries=list(symtab.iter_symbols())
   loads=[(i,x) for i,x in enumerate(elf.iter_segments()) if x['p_type']=='PT_LOAD']
   layout={'names':{x.name:i for i,x in enumerate(entries)},'values':[int(x['st_value']) for x in entries],
    'symtab':(int(symtab['sh_offset']),int(symtab['sh_entsize'])),'loads':[(i,int(x['p_vaddr']),int(x['p_filesz']),int(x['p_offset'])) for i,x in loads],
    'phdr':(int(elf.header['e_phoff']),int(elf.header['e_phentsize']))}
  return data,manifest,layout
 def entry(layout,name):base,size=layout['symtab'];return base+layout['names'][name]*size
 def refused(label,data,manifest,edits,message):
  corrupt=bytearray(data)
  for fmt,at,value in edits:struct.pack_into(fmt,corrupt,at,value)
  bad=e/(label+'.elf');bad.write_bytes(corrupt)
  try:prep.image_profile(bad,dict(manifest,final_elf=sha(corrupt)))
  except ValueError as error:assert str(error)==message,(label,str(error))
  else:raise AssertionError(label+' accepted')
  cases[label+'-refused']=True
 cases={}
 data,manifest,layout=load(current);path=current/'build/return.elf'
 geometry=prep.image_profile(path,manifest)[3];assert (geometry['wb_start'],geometry['wb_end'])==(0x48000000,0x48088000);cases['current-marker-accepted']=True
 old=(historical/'build/return.elf').read_bytes();old_manifest={}
 span=prep.image_profile(historical/'build/return.elf',old_manifest)[3];assert (span['wb_start'],span['wb_end'])==(0x48000000,0x48086000);cases['historical-marker-absent-accepted']=True
 marker=entry(layout,'gpu_return_wb_geometry');address=layout['values'][layout['names']['gpu_return_wb_geometry']]
 seg,vaddr,filesz,base=next(x for x in layout['loads'] if x[1]<=address<x[1]+x[2]);offset=base+address-vaddr
 phdr=layout['phdr'][0]+seg*layout['phdr'][1];flags=struct.unpack_from('<I',data,phdr+4)[0]
 name=struct.unpack_from('<I',data,marker)[0];other=struct.unpack_from('<I',data,entry(layout,'last_tick'))[0]
 unbacked=layout['values'][layout['names']['__bss_start']];assert not any(v<=unbacked<v+f for _,v,f,_ in layout['loads'])
 for label,edits,message in [
  ('changed-span',[('<Q',offset+8,0x48089000)],'Unknown GPU WB geometry.'),
  ('changed-start',[('<Q',offset,0x48001000)],'Unknown GPU WB geometry.'),
  ('marker-size',[('<Q',marker+16,8)],'Malformed GPU WB geometry marker.'),
  ('marker-duplicate',[('<I',entry(layout,'last_tick'),name)],'Malformed GPU WB geometry marker.'),
  ('marker-writable',[('<I',phdr+4,flags|2)],'GPU WB geometry must be immutable and file-backed.'),
  ('marker-unbacked',[('<Q',marker+8,unbacked)],'GPU WB geometry must be immutable and file-backed.'),
  ('typed-marker-renamed',[('<I',marker,other)],'GPU WB geometry marker does not match the allocator.')]:
  refused(label,data,manifest,edits,message)
 (e/'marker-result.json').write_text(json.dumps({'passed':True,'physical_access':False,'image_sha256':sha(data),'historical_image_sha256':sha(old),'cases':cases,
  'scope':'Host preparation of the final ELF and of a frozen image built before the marker; each refusal changes one native ELF field.',
  'source_sha256':{str(x.relative_to(ROOT)):hashlib.sha256(x.read_bytes()).hexdigest() for x in [ROOT/'apps/gpu-finite/prepare.py',Path(__file__).resolve()]}},indent=2)+'\n')
 print('PASS GPU write-back span marker:',len(cases),'cases')
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--evidence',type=Path,required=True)
p.add_argument('--image-evidence',type=Path,default=Path.home()/'.cache/sunstoneos/evidence/gpu-returning-repeat-fix-20261019')
p.add_argument('--shared-helper',type=Path)
for flag in ('marker-only','producer-only','disposition-only','session-envelope-only'):p.add_argument('--'+flag,action='store_true')
p.add_argument('--historical-evidence',type=Path,default=Path.home()/'.cache/sunstoneos/evidence/gpu-returning-repeat-fix-20261019')
a=p.parse_args();e=prep.unique_directory(a.evidence)
if a.marker_only:
 marker_cases(a.image_evidence,a.historical_evidence,e)
 raise SystemExit(0)
image=a.image_evidence/'build/return.elf'
template=Path.home()/'.cache/sunstoneos/evidence/gpu-single-core-20261018/provider-valid-fixture/fixtures/session-template.bin'
raw=template.read_bytes();parts,address,build,g=prep.image_profile(image,{})
results={}
def refused(name,call):
 try:call()
 except (ValueError,KeyError):results[name]=True
 else:raise AssertionError(name)
for name,change in [('expanded',lambda b:b.extend(bytes(40))),('header',lambda b:struct.pack_into('<Q',b,0,0)),
                    ('owner-conflict',lambda b:b.__setitem__(slice(464,472),b[456:464])),
                    ('entry-zero',lambda b:b.__setitem__(slice(80,112),bytes(32))),
                    ('cold-master-zero',lambda b:struct.pack_into('<Q',b,192,0))]:
 bad=bytearray(raw);change(bad)
 refused(name,lambda:prep.validate_admission({},'',bad,build))
refused('invalid-disposition',lambda:prep.validate_admission({'ram_disposition':'unknown'},'',raw,build))
refused('invalid-reservation',lambda:prep.validate_admission({'reservation':0},'',raw,build))
clang=Path('/usr/lib/llvm-21/bin/clang');lld=Path.home()/'.cache/sunstoneos/host/usr/bin/ld.lld-21'
bundle=e/'bundle';prepared=prep.prepare(image,None,None,template,bundle,clang,lld)
assert not prepared['entry_evidence'] and 'host_sources' not in prepared
assert prepared['_frozen']['session.bin'][48:80]==build
results['package-without-manifest-admission-review-or-evidence']=True
archive=e/'archived-inputs';archive.mkdir()
for name in ('launch.c','build.log','manifest.json','session-template.bin'):
 path=bundle/name
 if not path.exists():path.write_text('Archived development input.\n')
 metadata=json.loads((bundle/'prepared.json').read_bytes())
 metadata['files'][name]=prep.sha(path.read_bytes())
 (bundle/'prepared.json').write_text(json.dumps(metadata))
 path.write_text('Changed archived development input.\n')
 accepted=prep.validate_bundle(bundle)
 assert name not in accepted['_frozen'] and name not in accepted['files']
 path.rename(archive/name);prep.validate_bundle(bundle)
 results['archived-'+name+'-edit-and-removal-accepted']=True

# Artifact destinations refuse reuse before modifying the original bundle.
artifact_refused(lambda:prep.prepare(image,None,None,template,bundle,clang,lld),bundle)
results['existing-output-refused-without-changes']=True
for name in ('session.bin','contract.bin','launch.bin','segment-0.bin'):
 path=bundle/name;original=path.read_bytes();path.write_bytes(original+b'x')
 refused('changed-'+name,lambda:prep.validate_bundle(bundle));path.write_bytes(original)
metadata=json.loads((bundle/'prepared.json').read_bytes())
for name,field,value in [('upload-address','segments',[{'name':'session.bin','address':0}]),('geometry','geometry',{}),('build-id','build_id','00'*32)]:
 changed=copy.deepcopy(metadata);changed[field]=value;(bundle/'prepared.json').write_text(json.dumps(changed))
 refused(name,lambda:prep.validate_bundle(bundle))
(bundle/'prepared.json').write_text(json.dumps(metadata));prep.validate_bundle(bundle)
prep.save(e/'result.json',{'passed':True,'physical_access':False,'cases':results,'scope':'Actual launcher build and packaging; no USB, prior probe, manifest, review, claim or evidence file.'})
print('PASS GPU packaging:',len(results),'cases')
