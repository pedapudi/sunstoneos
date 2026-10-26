#!/usr/bin/env python3
"""Check paired record selection and backup-first plans on ordinary files.

Records come from actual preserved captures. Card geometry and storage sessions
come from the real producers; all sector interruptions are host models.
"""
import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import struct

ROOT=Path(__file__).resolve().parents[1]
def module(name,file):
    spec=importlib.util.spec_from_file_location(name,ROOT/'tools'/file)
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result

def refused(action,text):
    try:action()
    except (ValueError,KeyError) as error:assert text in str(error),(text,str(error));return
    raise AssertionError('accepted '+text)

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--restore-set',type=Path,required=True)
p.add_argument('--boot0',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
t=module('training','training-record.py');s=t.boot_slots();u=module('storage','storage-update.py')
analysis=t.restore_set(a.restore_set)
def actual(session,port):
    path=next(Path(c['path']) for c in analysis['captures'] if f'board-session-20261025-{session}/' in c['path']
              and c['port']==port and c['path'].endswith('capture-1'))
    return t.build(t.from_capture(path,a.restore_set,a.boot0,1))
one,two,foreign=actual(27,'3-3'),actual(25,'3-3'),actual(35,'3-1')
assert one!=two
for name,data in [('reference',one),('next-reference',two),('other-board',foreign)]: (a.output/(name+'.bin')).write_bytes(data)
image=a.output/'card.img'
with image.open('xb') as stream:
    stream.truncate(131072*512)
    mbr=bytearray(512);struct.pack_into('<4xB3xII',mbr,446,0x83,81920,49152);mbr[510:512]=b'\x55\xaa';stream.write(mbr)
backup=a.output/'backup.img';shutil.copyfile(image,backup);backup.chmod(0o444)
allocation=a.output/'allocation.json'
def allocate(output,*extra):
    errors=io.StringIO()
    with contextlib.redirect_stdout(io.StringIO()),contextlib.redirect_stderr(errors):
        try:
            return s.main(['allocate','--backup',str(backup),'--slot-a','40960:8192','--slot-b','49152:8192',
                           '--output',str(output),*extra])
        except SystemExit as error:
            raise ValueError(errors.getvalue()) from error
allocate(allocation,'--training-record-a','57344:8192','--training-record-b','65536:8192')
allocated=json.loads(allocation.read_text());units=allocated['training_records']
assert allocated['backup']['sha256']==hashlib.sha256(image.read_bytes()).hexdigest()
assert allocated['backup']['bytes']==image.stat().st_size
cases=[]
for name,args,message in [
    ('one-copy',['--training-record-a','57344:8192'],'both training-record'),
    ('overlap',['--training-record-a','57344:8192','--training-record-b','57344:8192'],'overlaps'),
    ('slot-overlap',['--training-record-a','49152:8192','--training-record-b','65536:8192'],'overlaps'),
    ('unaligned',['--training-record-a','57345:8192','--training-record-b','65536:8192'],'one allocation unit'),
    ('partition',['--training-record-a','81920:8192','--training-record-b','65536:8192'],'Partition'),
    ('capacity',['--training-record-a','131072:8192','--training-record-b','65536:8192'],'one allocation unit'),
    ('legacy-mixed',['--training-record','73728:8192','--training-record-a','57344:8192','--training-record-b','65536:8192'],'both training-record')]:
    refused(lambda:allocate(a.output/(name+'.json'),*args),message);cases.append(name)

card={'kind':1,'host':0,'cid':[0x1111,0x2222,0x3333,0x4444],'sectors':131072,'target':{'port':'model','sysfs':'model'}}
def select(reference=one):
    raw,owned,fd,n=t.paired_image(s,allocation,image)
    try:return t.select_copies(s,fd,owned,analysis,reference)
    finally:os.close(fd)
def plan(label,reference=two):
    return t.plan_standby(s,allocation,image,reference,analysis,a.output/label)
def converted(label,dest):return u.plan(a.output/label/'plan.json',allocation,image,card,a.output/dest)
def put(copy,record):
    with image.open('r+b') as stream:
        stream.seek(units[copy]['first']*512);stream.write(bytes(0x400000))
        if record:
            stream.seek(units[copy]['first']*512);stream.write(record[:128])
            stream.seek((units[copy]['first']+1)*512);stream.write(record[128:])
def generation(record,value):
    parsed=t.parse(record);parsed['generation']=value;return t.build(parsed)
def unit_bytes(copy):
    with image.open('rb') as stream:stream.seek(units[copy]['first']*512);return stream.read(0x400000)
def apply_file(label,write,count=None):
    data=(a.output/label/write['file']).read_bytes()
    with image.open('r+b') as stream:
        stream.seek(write['first_sector']*512);stream.write(data if count is None else data[:count])

assert select()['action']=='full-training'
initial=plan('install',one);assert (initial['record_copy'],initial['generation'])==('A',1)
first_sessions=converted('install','install-sessions')
assert [x['region'] for x in first_sessions['sessions']]==['training-record-A']*2
assert [x['backup_sha256'] for x in first_sessions['sessions']]==[x['previous_sha256'] for x in initial['writes']]
apply_file('install',initial['writes'][0]);assert select()['selected'] is None
resume=converted('install','install-resume');assert [x['name'] for x in resume['skipped']]==['record-A-payload']
apply_file('install',initial['writes'][1]);assert select()['selected']=='A'
cases+=['empty-full-training','install-A','completed-range-resume']

update=plan('update');assert (update['record_copy'],update['generation'])==('B',2)
second_sessions=converted('update','update-sessions');assert [x['region'] for x in second_sessions['sessions']]==['training-record-B']*2
active=unit_bytes('A');sector_cases=0
for write in update['writes']:
    data=(a.output/'update'/write['file']).read_bytes()
    for offset in range(0,len(data),512):
        with image.open('r+b') as stream:stream.seek(write['first_sector']*512+offset);stream.write(data[offset:offset+512])
        selected=select()
        assert selected['selected']==('B' if write['name'].endswith('header') else 'A')
        assert unit_bytes('A')==active
        sector_cases+=1
assert select()['copies']['B']['generation']==2
two_copy_read_bytes=sum(g['blocks']*512 for c in select()['copies'].values() for g in c['guards'])
assert two_copy_read_bytes==28672
refused(lambda:plan('repeat',two),'already holds')
assert not converted('update','update-complete')['sessions']
cases+=['standby-B-generation-two','sector-interruptions-preserve-active','completed-plan-skips','repeat-refused']

# Two valid generations may hold the same captured values. Seed that admitted
# state so the next update changes both the standby payload and its header.
put('A',generation(two,1))
third=plan('third',one);assert third['record_copy']=='A' and third['generation']==3
original_a=unit_bytes('A');active_b=unit_bytes('B')
payload=(a.output/'third'/third['writes'][0]['file']).read_bytes()
differences=[i for i,b in enumerate(payload) if b!=original_a[512+i]]
for cut in sorted({differences[0]+1,(differences[0]+differences[-1])//2,differences[-1]}):
    with image.open('r+b') as stream:stream.seek(units['A']['first']*512);stream.write(original_a)
    apply_file('third',third['writes'][0],cut)
    assert select()['selected']=='B' and unit_bytes('B')==active_b
    refused(lambda:converted('third',f'torn-payload-{cut}'),'differs from the boot-slot plan')
for cut in (1,24,64,124,128,256,511):
    with image.open('r+b') as stream:stream.seek(units['A']['first']*512);stream.write(original_a)
    apply_file('third',third['writes'][0]);apply_file('third',third['writes'][1],cut)
    chosen=select()
    assert chosen['selected']==('A' if cut>=128 else 'B')
    assert unit_bytes('B')==active_b
    if cut>=128:assert chosen['copies']['A']['generation']==3
    elif unit_bytes('A')[:512]!=original_a[:512]:
        refused(lambda:converted('third',f'torn-header-{cut}'),'differs from the boot-slot plan')
    else:
        assert len(converted('third',f'unchanged-header-{cut}')['sessions'])==1
cases+=['torn-payload-refuses-resume','torn-header-selects-only-valid','source-copy-preserved']

# Tie selection is deterministic; a payload already present in standby is skipped.
put('A',generation(one,5));put('B',generation(two,5));assert select()['selected']=='A'
tie=plan('tie');assert tie['record_copy']=='B' and tie['generation']==6
assert [x['name'] for x in converted('tie','tie-sessions')['sessions']]==['record-B-header']
put('A',generation(one,0xffffffffffffffff));put('B',None)
refused(lambda:plan('exhausted'),'exhausted')
put('A',foreign);assert select()['action']=='full-training'
refused(lambda:plan('foreign'),'another board')
cases+=['tie-A','same-payload-header-only','generation-exhaustion','wrong-board-full-training-and-update-refusal']

# A blank non-target can acquire a new record without touching target ranges.
put('A',None);put('B',None);plan('blank-guard',one)
put('B',generation(two,9));refused(lambda:converted('blank-guard','blank-stale'),'Non-target')
# A CRC-invalid payload can become valid while its header stays unchanged.
put('A',None);bad=bytearray(generation(two,9));bad[200]^=1;put('B',bytes(bad))
invalid=plan('invalid-guard',one);assert len(invalid['guards'])==2
put('B',generation(two,9));refused(lambda:converted('invalid-guard','invalid-stale'),'Non-target')
# An invalid length needs only a header guard; becoming valid changes that header.
put('A',None);bad=bytearray(generation(two,9));struct.pack_into('<I',bad,16,0xffffffff);struct.pack_into('<I',bad,124,t.crc(bad[:124]));put('B',bytes(bad))
short=plan('length-guard',one);assert len(short['guards'])==1
put('B',generation(two,9));refused(lambda:converted('length-guard','length-stale'),'Non-target')
cases+=['blank-nontarget-guard','invalid-payload-nontarget-guard','invalid-length-header-guard']

put('A',one);put('B',None);safe=plan('guard')
for name,edit,message in [
    ('missing-guard',lambda x:x.update(guards=[]),'Non-target'),
    ('order',lambda x:x['writes'].reverse(),'payload then header'),
    ('placement',lambda x:x['writes'][1].update(first_sector=57344),'placement'),
    ('generation',lambda x:x.update(generation=8),'generation')]:
    destination=a.output/name;shutil.copytree(a.output/'guard',destination)
    changed=json.loads((destination/'plan.json').read_text());edit(changed);(destination/'plan.json').write_text(json.dumps(changed))
    refused(lambda:converted(name,name+'-sessions'),message);cases.append(name)
# Source guard remains effective for a previously selected copy too.
put('A',generation(one,10));refused(lambda:converted('guard','changed-active'),'Non-target');cases.append('changed-active')

for name,edit,message in [
    ('missing-copy',lambda x:x['training_records'].pop('B'),'exactly copies'),
    ('mixed-format',lambda x:x.update(training_record={'first':73728,'blocks':8192}),'mutually exclusive'),
    ('changed-capacity',lambda x:x.update(card_sectors=131071),'layout')]:
    altered=json.loads(allocation.read_text());edit(altered)
    refused(lambda:s.check_allocation(altered,131072,image.read_bytes()[:512]),message);cases.append(name)
original=backup.read_bytes();final=image.read_bytes()
assert s.sha(original)==allocated['backup']['sha256']
assert final[:57344*512]==original[:57344*512] and final[73728*512:]==original[73728*512:]
result={'physical_access':False,'cases':cases,'sector_interruption_cases':sector_cases,
        'allocation_sha256':s.sha(allocation.read_bytes()),'record_sha256':s.sha(one),'next_record_sha256':s.sha(two),
        'two_copy_read_bytes':two_copy_read_bytes,
        'source_sha256':{str(file):s.sha((ROOT/file).read_bytes()) for file in
                        ['tools/training-record.py','tools/boot-slots.py','tools/storage-update.py','checks/training-record-copies.py']}}
(a.output/'results.json').write_text(json.dumps(result,indent=2)+'\n')
print('PASS paired records:',len(cases),'cases and',sector_cases,'sector interruption points; no physical access.')
