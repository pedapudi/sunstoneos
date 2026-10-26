#!/usr/bin/env python3
"""Check derived-image identity and helper updates with actual producers and files."""
import argparse,copy,hashlib,importlib.util,json,os,shutil,struct
from pathlib import Path
from types import SimpleNamespace
ROOT=Path(__file__).resolve().parents[1]
def module(name):
    spec=importlib.util.spec_from_file_location(name.replace('-','_'),ROOT/'tools'/f'{name}.py')
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
p=argparse.ArgumentParser(description=__doc__)
for name in ('build','boot0','allocation','backup','packer','monitor-input','output'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();a.output.mkdir();v=module('prepare-card-ddr');t=module('training-record');s=module('boot-slots');u=module('storage-update');package=module('prepare-microsd-boot')
original=a.boot0.read_bytes();base=json.loads((a.build/'manifest.json').read_text());blob=(a.build/'boot0-card-restore.bin').read_bytes();helper=(a.build/'card-ddr-helper.bin').read_bytes()
cases=[]
def refused(name,fn):
    try:fn()
    except (ValueError,OSError,KeyError):cases.append(name);return
    raise AssertionError('admitted '+name)
assert v.validate_variant(a.build/'manifest.json',original)[0]==blob;cases.append('actual-producer-variant')
for kind in ('policy','helper-crc','units','qualification','wrong-patch','changed-body','presence','helper-identity'):
    out=a.output/kind;out.mkdir();m=copy.deepcopy(base);data=bytearray(blob);h=bytearray(helper)
    if kind=='policy':m['experimental_unknown_temperature']=not m['experimental_unknown_temperature']
    if kind=='helper-crc':m['helper_crc32']^=1
    if kind=='units':m['units']['A']['first']+=8192
    if kind=='qualification':m['qualification']['physical_boot']=True
    if kind=='wrong-patch':data[0x17216]^=2 # actual init call offset
    if kind=='changed-body':data[0x1000]^=1
    if kind=='presence':data[0x6f4]=1
    if kind=='helper-identity':h[-1]^=1
    if kind in ('wrong-patch','changed-body','presence'):
        struct.pack_into('<I',data,12,v.checksum(data));m['files']['boot0-card-restore.bin']={'bytes':len(data),'sha256':v.sha(data)}
    (out/'boot0-card-restore.bin').write_bytes(data);(out/'card-ddr-helper.bin').write_bytes(h)
    (out/'manifest.json').write_text(json.dumps(m));refused(kind,lambda:v.validate_variant(out/'manifest.json',original))
allocation=json.loads(a.allocation.read_text());units=allocation['training_records']
image=a.output/'card.img';shutil.copyfile(a.backup,image)
card={'kind':1,'host':0,'cid':[0x1111,0x2222,0x3333,0x4444],'sectors':image.stat().st_size//512,'target':{'port':'model','sysfs':'model'}}
plan=a.build/'helper-plan/plan.json';converted=u.plan(plan,a.allocation,image,card,a.output/'sessions')
assert len(converted['sessions'])==2
before=image.read_bytes();expected=bytearray(before)
for session in converted['sessions']:
    data=(a.output/'sessions'/session['candidate']).read_bytes();offset=session['first']*512
    assert session['first']==units[session['name'][-1]]['first']+64
    assert data[:len(helper)]==helper and not any(data[len(helper):])
    expected[offset:offset+len(data)]=data
    with image.open('r+b') as f:f.seek(offset);f.write(data)
assert image.read_bytes()==bytes(expected);cases.append('helper-plan-exact-ranges')
resumed=u.plan(plan,a.allocation,image,card,a.output/'resume');assert not resumed['sessions'] and len(resumed['skipped'])==2;cases.append('same-plan-complete-resume')
for name,change in [('record-overlap',lambda p:p['writes'][0].update(first_sector=units['A']['first']+1)),
                    ('unit-crossing',lambda p:p['writes'][1].update(first_sector=units['B']['first']+8191)),
                    ('copy-missing',lambda p:p['writes'].pop()),
                    ('oversized',lambda p:p.update(helper_bytes=32769))]:
    out=a.output/name;shutil.copytree(plan.parent,out);value=json.loads((out/'plan.json').read_text());change(value);(out/'plan.json').write_text(json.dumps(value))
    refused(name,lambda:u.plan(out/'plan.json',a.allocation,image,card,a.output/(name+'-sessions')))
# A partial range needs a fresh backup/plan; original-or-complete-candidate only.
first=converted['sessions'][0];offset=first['first']*512
with image.open('r+b') as f:f.seek(offset);f.write(before[offset:offset+512])
refused('torn-helper-refuses-resume',lambda:u.plan(plan,a.allocation,image,card,a.output/'torn'))
assert hashlib.sha256(a.backup.read_bytes()).hexdigest()==allocation['backup']['sha256'];cases.append('preserved-backup')
bad=copy.deepcopy(allocation);bad['backup']['sha256']='00'*32;(a.output/'bad-allocation.json').write_text(json.dumps(bad))
refused('producer-backup-binding',lambda:v.prepare(SimpleNamespace(boot0=a.boot0,allocation=a.output/'bad-allocation.json',backup=a.backup,output=a.output/'bad-build')))
manifest=package.prepare(SimpleNamespace(input=a.monitor_input,boot0=a.boot0,packer=a.packer,restore_variant=a.build/'manifest.json',output=a.output/'packaged'))
assert manifest['restore_variant']['sha256']==v.sha(blob) and not any(manifest['qualification'].values())
writes=s.container_writes(a.output/'packaged.manifest.json');assert writes[0][2]==blob;cases.append('actual-optional-package-and-parser')
(a.output/'results.json').write_text(json.dumps({'cases':cases,'pass':True,'physical_access':False,
    'manifest_sha256':v.sha((a.build/'manifest.json').read_bytes()),'checker_sha256':v.sha(Path(__file__).read_bytes()),
    'backup_sha256':allocation['backup']['sha256']},indent=2)+'\n')
print('PASS',len(cases),'card restore host cases')
