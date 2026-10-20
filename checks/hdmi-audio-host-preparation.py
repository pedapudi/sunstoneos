#!/usr/bin/env python3
"""Emit a typed model session, then package its unchanged actual native inputs."""
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
    for name in ('image','session','pcm','edid','output'):parser.add_argument('--'+name,type=Path,required=True)
    for name in ('clang','lld','objcopy'):parser.add_argument('--'+name,type=Path)
    parser.add_argument('--stage',choices=('fixture','bundle'),required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=args.stage=='bundle')
    prep=module('hdmi_audio_preparation_check',ROOT/'apps/hdmi-audio/prepare.py')
    image=args.image.read_bytes();original=args.session.read_bytes();pcm=args.pcm.read_bytes();edid=args.edid.read_bytes()
    prep.image_metadata(image);prep.session_check(original,pcm);prep.sink_lpcm(edid)
    image_hash=prep.sha(image);fields=prep.session_check(original,pcm)
    fixture=args.output/'MODEL-NOT-PHYSICAL-fixture';fixture.mkdir(exist_ok=args.stage=='bundle')
    def record(name,value):
        data=(json.dumps(value,sort_keys=True,indent=2)+'\n').encode();(fixture/name).write_bytes(data);return data
    target=dict(image_sha256=image_hash,physical_authority=False,modeled=True,reviewed=True,
        **{key:fields[key] for key in ('epoch','generation','lease','supervisor')})
    pattern=bytearray(original[:440])
    for key,offset in (('analog',112),('power',144),('supervision',176)):
        data=record(key+'.json',dict(**target,kind=key,
            purpose='Explicit software fixture; no physical HDMI or recovery authority.'))
        pattern[offset:offset+32]=hashlib.sha256(data).digest()
    memory=record('memory.json',dict(**target,schema='a733-hdmi-audio-memory-v1',authority=fields['authority'],
        **{key:True for key in prep.MEMORY_FACTS},dma_handoff_sha256=prep.sha(original[552:656]),
        dma_pool_ranges=[[0x41c00000,0x41e00000]],pcm_sha256=prep.sha(pcm),pcm_range=[0x41f10000,0x41f10000+len(pcm)]))
    electrical=record('electrical.json',dict(**target,schema='a733-hdmi-audio-electrical-v1',authority=fields['authority'],
        **{key:True for key in prep.ELECTRICAL_FACTS},sink_edid_sha256=prep.sha(edid)))
    (fixture/'sink.edid').write_bytes(edid)
    raw_hz=struct.unpack_from('<I',original,300)[0];n=99 if raw_hz==24000000 else 91
    admission=record('admission.json',dict(**target,schema='a733-hdmi-supervised-admission-v1',
        audio='48khz-stereo-s16le-retained-v1',audio_authority=fields['authority'],pcm_sha256=prep.sha(pcm),pcm_bytes=len(pcm),
        analog='analog.json',power='power.json',supervision='supervision.json',audio_memory='memory.json',audio_electrical='electrical.json',
        sink_edid=dict(path='sink.edid',sha256=prep.sha(edid)),
        clock_profile=dict(raw_dcxo_hz=raw_hz,reference_hz=24000000,sfr_source=0,sfr_hz=24000000,
            pixel_hz={'720p60':raw_hz*n//32,'1080p60':raw_hz*n//16})))
    raw=prep.session_bytes(pattern,pcm,memory=memory,electrical=electrical,authority=fields['authority'],dma=original[552:656],admission=admission)
    (fixture/'session.bin').write_bytes(raw);(fixture/'input.pcm').write_bytes(pcm)
    _,address,identity,_=prep.image_metadata(image)
    request,contract=prep.base.request_bytes(address,identity,raw,fields['epoch'],fields['generation'],fields['lease'],fields['supervisor'])
    (fixture/'request.bin').write_bytes(request);(fixture/'contract.bin').write_bytes(contract)
    summary=dict(physical_access=False,physical_authority=False,image_sha256=image_hash,
        session_sha256=prep.sha(raw),pcm_sha256=prep.sha(pcm),fixture=str(fixture.resolve()))
    if args.stage=='fixture':print(json.dumps(summary));return
    if any(getattr(args,key) is None for key in ('clang','lld','objcopy')):parser.error('Bundle needs all launcher tool paths.')
    try:prep.validate(args.image,fixture/'session.bin',fixture/'admission.json',fixture/'input.pcm')
    except ValueError as error:assert 'Modeled HDMI audio evidence' in str(error)
    else:raise AssertionError('Modeled authority passed physical admission')
    bundle=args.output/'MODEL-NOT-PHYSICAL-bundle'
    with patch.object(prep,'physical_authority',lambda value:None):
        prepared=prep.prepare(args.image,fixture/'session.bin',fixture/'admission.json',fixture/'input.pcm',bundle,args.clang,args.lld,args.objcopy)
    assert prepared['modeled'] is True and prepared['physical_authority'] is False
    summary.update(passed=True,production_refused=True,bundle=str(bundle.resolve()),launcher_sha256=prepared['files']['launch.bin'])
    (args.output/'preparation-result.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary))


if __name__=='__main__':main()
