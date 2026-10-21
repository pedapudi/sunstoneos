#!/usr/bin/env python3
"""Check only copied HDMI audio transport and new admission using an actual reply."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import shutil
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for key in ('bundle','response','output'):p.add_argument('--'+key,type=Path,required=True)
    args=p.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    host=module('hdmi_audio_usb_capture_check',ROOT/'apps/hdmi-audio/capture.py');prep=host.preparation
    helper=module('hdmi_audio_usb_fixture_helper',ROOT/'checks/hdmi-audio-usb-preparation.py')
    try:prep.validate_bundle(args.bundle)
    except ValueError as error:assert 'Modeled HDMI audio evidence' in str(error)
    else:raise AssertionError('Model bundle passed production admission')
    with helper.modeled(prep):prepared=prep.validate_bundle(args.bundle)
    frozen=prepared['_frozen'];session=frozen['source-session.bin'];pcm=frozen['input.pcm'];actual=args.response.read_bytes()
    assert len(session)==816 and len(actual)==512
    nominal=host.decode_observation(actual,1,session,pcm);assert nominal['state']==1 and nominal['flags']==7
    refused=[]
    for case in ('short','long','magic','version','bytes','request-id','epoch','generation','lease','image','session',
        'reserved','unknown-flags','fatal-success','missing-stop','missing-usb','missing-result','pending-fatal',
        'unpublished-body','zero-time','zero-remaining','over-lifetime','overflow'):
        raw=bytearray(actual)
        if case=='short':raw=raw[:-1]
        elif case=='long':raw+=b'\0'
        elif case in ('magic','version','bytes','request-id'):raw[{'magic':0,'version':4,'bytes':8,'request-id':12}[case]]^=1
        elif case in ('epoch','generation','lease','image','session','reserved'):raw[{'epoch':16,'generation':24,'lease':32,'image':64,'session':96,'reserved':288}[case]]^=1
        elif case=='unknown-flags':struct.pack_into('<I',raw,44,23)
        elif case=='fatal-success':struct.pack_into('<I',raw,44,15)
        elif case=='missing-stop':struct.pack_into('<I',raw,44,3)
        elif case=='missing-usb':struct.pack_into('<I',raw,44,5)
        elif case=='missing-result':struct.pack_into('<I',raw,44,6);raw[128:288]=bytes(160)
        elif case=='pending-fatal':struct.pack_into('<2I',raw,40,0,15)
        elif case=='unpublished-body':struct.pack_into('<2I',raw,40,0,2)
        elif case=='zero-time':struct.pack_into('<Q',raw,48,0)
        elif case=='zero-remaining':struct.pack_into('<Q',raw,56,0)
        elif case=='over-lifetime':struct.pack_into('<Q',raw,56,20000001)
        elif case=='overflow':struct.pack_into('<Q',raw,48,(1<<64)-1)
        try:host.decode_observation(bytes(raw),1,session,pcm)
        except ValueError:refused.append(case)
        else:raise AssertionError('Invalid copied frame accepted: '+case)
    archives=[]
    def capture(name,exchange,*,clock=None,fail=False):
        folder=args.output/name;folder.mkdir();record={};now=(lambda:0.0) if clock is None else clock
        def save():
            (folder/'journal.json').write_text(json.dumps(record,indent=2)+'\n')
        try:
            value=host.capture_copied(exchange,session,pcm,folder,record,save,19.0,now=now,pause=lambda delay:None)
        except (ValueError,OSError):
            assert fail and record['hdmi_audio_result']=='unobserved' and 'durable_result' not in record
            value=None
        else:assert not fail
        archives.append(name);return folder,record,value
    folder,record,_=capture('actual-terminal',lambda packet,timeout:actual)
    assert record['hdmi_audio_result']=='observed-success' and (folder/'hdmi-audio-001.bin').read_bytes()==actual
    for state,flags in ((0,2),(0,3),(2,2),(2,11)):
        raw=bytearray(actual);struct.pack_into('<2I',raw,40,state,flags)
        if not flags&1:raw[128:288]=bytes(160)
        value=host.decode_observation(bytes(raw),1,session,pcm);assert value['state']==state
    bad=bytearray(actual);bad[96]^=1
    folder,record,_=capture('stale-raw-retained',lambda packet,timeout:bytes(bad),fail=True)
    assert (folder/'hdmi-audio-001.bin').read_bytes()==bytes(bad)
    clock=[0.0]
    def late(packet,timeout):clock[0]=1.1;return actual
    folder,record,_=capture('late-raw-retained',late,clock=lambda:clock[0],fail=True)
    assert (folder/'hdmi-audio-001.bin').read_bytes()==actual
    def pending(packet,timeout):
        sequence=struct.unpack_from('<I',packet,8)[0];raw=bytearray(actual)
        struct.pack_into('<I',raw,12,sequence);struct.pack_into('<2I',raw,40,0,2);raw[128:288]=bytes(160)
        return bytes(raw)
    _,record,_=capture('request-budget',pending,fail=True);assert len(record['hdmi_audio_requests'])==200
    for name,offset in (('regressed-time',48),('changed-deadline',56)):
        calls=[0]
        def exchange(packet,timeout):
            calls[0]+=1;raw=bytearray(pending(packet,timeout))
            if calls[0]==2:
                value=struct.unpack_from('<Q',raw,offset)[0];struct.pack_into('<Q',raw,offset,value-1 if offset==48 else value+1)
            return bytes(raw)
        _,record,_=capture(name,exchange,fail=True);assert len(record['hdmi_audio_requests'])==2
    original_durable=host.durable
    def failed(path,data):raise OSError('Modeled archive failure')
    with patch.object(host,'durable',failed):capture('archive-failure',lambda packet,timeout:actual,fail=True)
    class Device:
        def write(self,endpoint,data,timeout):assert endpoint==2 and len(data)==32 and 0<timeout<=1000;return len(data)
        def read(self,endpoint,size,timeout):assert endpoint==0x81 and size==513 and 0<timeout<=1000;return actual
    folder=args.output/'device-terminal';folder.mkdir();record={}
    host.capture_device(Device(),lambda:None,19.0,session,pcm,folder,record,lambda:None,now=lambda:0.0,pause=lambda delay:None)
    assert record['hdmi_audio_result']=='observed-success'
    admission=json.loads(frozen['admission.bin']);cache={path:frozen['entry-evidence-'+str(i)+'.bin'] for i,path in enumerate(json.loads(frozen['entry-index.bin']))}
    for name,change in (
        ('entry-reservation',lambda record:record.update(reservation=0)),
        ('entry-recovery',lambda record:record['recovery'].update(demonstrated_fel=False))):
        record=json.loads(json.dumps(admission));change(record)
        try:prep.validate_entry(record,prepared['image_sha256'],cache,True)
        except ValueError:refused.append(name)
        else:raise AssertionError('Invalid shared entry accepted: '+name)
    for name,offset in (('rom-reference',768),('usb-owner-alias',656),('usb-reserved',772),('usb-rail',776)):
        raw=bytearray(session)
        if name=='usb-owner-alias':raw[offset:offset+8]=session[600:608]
        else:struct.pack_into('<I',raw,offset,2 if name=='rom-reference' else 1)
        try:prep.session_check(bytes(raw),pcm)
        except ValueError:refused.append(name)
        else:raise AssertionError('Invalid USB admission accepted: '+name)
    records={key[:-4]:data for key,data in frozen.items() if key.endswith('.bin')}
    for kind,offset in (('initial',672),('electrical',704),('supervision',736)):
        trial=records.copy();fact=json.loads(trial['usb_'+kind]);fact[prep.USB_FACTS[kind][0]]=False
        data=json.dumps(fact).encode();trial['usb_'+kind]=data;raw=bytearray(session)
        raw[offset:offset+32]=hashlib.sha256(data).digest()
        with helper.modeled(prep):
            try:prep.validate_data(frozen['image.elf'],bytes(raw),frozen['admission.bin'],pcm,trial)
            except ValueError:refused.append('usb-'+kind+'-authority')
            else:raise AssertionError('Incomplete USB authority accepted: '+kind)
    copy=args.output/'segment-refusal';shutil.copytree(args.bundle,copy)
    metadata=json.loads((copy/'prepared.json').read_bytes());metadata['segments'][0]['address']+=4096
    (copy/'prepared.json').write_text(json.dumps(metadata))
    with helper.modeled(prep):
        try:prep.validate_bundle(copy)
        except ValueError as error:
            assert 'supervised upload segments differ' in str(error);refused.append('resealed-segment-address')
        else:raise AssertionError('Altered preload address accepted')
    summary=dict(passed=True,physical_access=False,physical_qualified=False,modeled_transport=True,
        image_sha256=prepared['image_sha256'],session_sha256=prepared['session_sha256'],
        actual_response_sha256=hashlib.sha256(actual).hexdigest(),refused=refused,archives=archives,
        source_sha256={str(path.relative_to(ROOT)):hashlib.sha256(path.read_bytes()).hexdigest() for path in
            (Path(__file__),Path(host.__file__),Path(prep.__file__),ROOT/'checks/hdmi-audio-usb-preparation.py')})
    (args.output/'result.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary))
if __name__=='__main__':main()
