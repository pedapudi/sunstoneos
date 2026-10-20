#!/usr/bin/env python3
"""Check new HDMI audio bindings and durable offline observation from an actual reply."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('bundle','result','ready','output'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    capture=module('hdmi_audio_host_capture_check',ROOT/'apps/hdmi-audio/capture.py');prep=capture.preparation
    try:prep.validate_bundle(args.bundle)
    except ValueError as error:assert 'Modeled HDMI audio evidence' in str(error)
    else:raise AssertionError('Modeled bundle passed physical admission')
    with patch.object(prep,'physical_authority',lambda value:None):prepared=prep.validate_bundle(args.bundle)
    assert prepared['modeled'] is True and prepared['physical_authority'] is False
    frozen=prepared['_frozen'];image=frozen['image_elf.bin'];session=frozen['source-session.bin'];pcm=frozen['pcm.bin']
    result=args.result.read_bytes();ready=args.ready.read_bytes()
    nominal=capture.capture(image,session,pcm,result,ready,args.output/'nominal')
    assert nominal['result']['stage']=='input-ended-retained' and not nominal['result']['wire_drained']
    failure=bytearray(result);struct.pack_into('<Q',failure,24,3)
    assert capture.decode(bytes(failure),ready,session,pcm)['stage']=='fault-retained'
    refused=[]
    for case in ('not-ready','short-result','running-result','wrong-frames','wrong-pcm','wrong-session-digest','cross-session-same-pcm','partial-consumption',
        'dma-failed','fifo-not-empty','wrong-tmds','wrong-rate','wire-drain'):
        raw=bytearray(result);publication=ready;trial_session=session
        if case=='not-ready':publication=bytes(4)
        elif case=='short-result':raw=raw[:-1]
        elif case=='running-result':struct.pack_into('<Q',raw,24,1)
        elif case=='wrong-frames':struct.pack_into('<Q',raw,32,1+len(pcm)//4)
        elif case=='wrong-pcm':raw[96]^=1
        elif case=='wrong-session-digest':raw[128]^=1
        elif case=='cross-session-same-pcm':
            trial_session=bytearray(session);epoch=struct.unpack_from('<Q',session,24)[0]
            struct.pack_into('<Q',trial_session,24,1 if epoch!=1 else 2)
            trial_session=bytes(trial_session)
        elif case=='partial-consumption':struct.pack_into('<Q',raw,48,len(pcm)-4)
        elif case=='dma-failed':struct.pack_into('<Q',raw,56,3)
        elif case=='fifo-not-empty':struct.pack_into('<Q',raw,64,0)
        elif case=='wrong-tmds':struct.pack_into('<Q',raw,72,74250001)
        elif case=='wrong-rate':struct.pack_into('<Q',raw,80,44100)
        elif case=='wire-drain':struct.pack_into('<Q',raw,88,1)
        try:capture.decode(bytes(raw),publication,trial_session,pcm)
        except ValueError:refused.append(case)
        else:raise AssertionError('Bad result accepted: '+case)
    records={key:frozen[key+'.bin'] for key in ('analog','power','supervision','audio_memory','audio_electrical','sink_edid')}
    for case in ('pcm','authority','handoff','clock','video-mode','memory-record','sink-record','session-image'):
        raw=bytearray(session);source=bytearray(pcm);facts=records.copy();admission=json.loads(frozen['admission.bin'])
        if case=='pcm':source[0]^=1
        elif case=='authority':struct.pack_into('<Q',raw,536,0)
        elif case=='handoff':raw[552]^=1
        elif case=='clock':struct.pack_into('<I',raw,300,24000000 if struct.unpack_from('<I',raw,300)[0]==26000000 else 26000000)
        elif case=='video-mode':struct.pack_into('<I',raw,288,16)
        elif case=='memory-record':facts['audio_memory']+=b' '
        elif case=='sink-record':facts['sink_edid']=bytes(256)
        elif case=='session-image':raw[48]^=1
        with patch.object(prep,'physical_authority',lambda value:None):
            try:prep.validate_data(image,bytes(raw),frozen['admission.bin'],bytes(source),facts)
            except ValueError:refused.append(case)
            else:raise AssertionError('Bad admission accepted: '+case)
    for case in ('edid-checksum','edid-without-lpcm'):
        edid=bytearray(records['sink_edid'])
        if case=='edid-checksum':edid[127]^=1
        else:
            for first in range(128,len(edid),128):
                if edid[first]!=2 or edid[first+1]<3:continue
                edid[first+3]&=~0x40
                cursor=first+4;end=first+edid[first+2]
                while cursor<end:
                    tag=edid[cursor]>>5;count=edid[cursor]&31;cursor+=1
                    if tag==1:
                        for pos in range(cursor,cursor+count,3):edid[pos+2]&=~1
                    cursor+=count
                edid[first+127]=(-sum(edid[first:first+127]))&255
        try:prep.sink_lpcm(bytes(edid))
        except ValueError:refused.append(case)
        else:raise AssertionError('Unsupported sink accepted: '+case)
    failures=[]
    for name in ('result.bin','observation.json'):
        folder=args.output/('save-failure-'+name)
        def save(path,data):
            if path.name==name:raise OSError('modeled durable archive failure')
            capture.durable(path,data)
        try:capture.capture(image,session,pcm,result,ready,folder,save=save)
        except OSError:pass
        else:raise AssertionError('Failed archive reported success')
        assert not (folder/'observation.json').exists();failures.append(name)
    summary=dict(passed=True,physical_access=False,live_capture=False,physical_qualified=False,
        image_sha256=prepared['image_sha256'],session_sha256=prepared['session_sha256'],
        native_reply_sha256=hashlib.sha256(result).hexdigest(),ready_sha256=hashlib.sha256(ready).hexdigest(),
        refused=refused,durability_failures=failures,
        sources={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in
            (Path(__file__),Path(capture.__file__),Path(prep.__file__),ROOT/'apps/hdmi-audio/observe.py')})
    (args.output/'result.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary))


if __name__=='__main__':main()
