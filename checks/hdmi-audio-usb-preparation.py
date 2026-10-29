#!/usr/bin/env python3
"""Seal an explicit model fixture for the actual copied-USB HDMI audio image."""
import argparse
from contextlib import contextmanager
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


@contextmanager
def modeled(prep):
    """Replace only physical qualification, retaining every binding predicate."""
    original=prep.security_authority.validate
    def security(data,*args):
        record=json.loads(data)
        assert record['modeled'] is True and record['physical_authority'] is False
        record.update(modeled=False,physical_authority=True)
        return original(json.dumps(record).encode(),*args)
    def authority(record):
        assert record['modeled'] is True and record['physical_authority'] is False
    with patch.object(prep,'physical_authority',authority),patch.object(prep.security_authority,'validate',security):yield


def fixture(prep,args):
    image=args.image.read_bytes();original=args.session.read_bytes();pcm=args.pcm.read_bytes();edid=args.edid.read_bytes()
    size=prep.base.image_data(image)[3];stream=size==848
    prep.image_metadata(image,session_size=size)
    fields=prep.session_check(original,pcm);prep.sink_lpcm(edid)
    out=args.output/'MODEL-NOT-PHYSICAL-fixture';out.mkdir()
    image_hash=prep.sha(image)
    target=dict(image_sha256=image_hash,physical_authority=False,modeled=True,reviewed=True,
        identity='MODEL-NOT-A-PHYSICAL-BOARD',port='MODEL-NOT-A-USB-PORT',
        **{key:fields[key] for key in ('epoch','generation','lease','supervisor')})
    def record(name,value):
        data=(json.dumps(value,sort_keys=True,indent=2)+'\n').encode();(out/name).write_bytes(data);return data
    def ref(name):return dict(path=str((out/name).resolve()),sha256=prep.sha((out/name).read_bytes()))
    observation=record('model-observation.json',dict(**target,purpose='Model only; no physical qualification.'))
    support=ref('model-observation.json')
    pattern=bytearray(original[:440])
    for key,offset in (('analog',112),('power',144),('supervision',176)):
        data=record(key+'.json',dict(**target,kind=key));pattern[offset:offset+32]=hashlib.sha256(data).digest()
    memory=record('memory.json',dict(**target,schema='a733-hdmi-audio-memory-v1',authority=fields['authority'],
        **{key:True for key in prep.MEMORY_FACTS},dma_handoff_sha256=prep.sha(original[552:656]),
        dma_pool_ranges=[[0x41c00000,0x41e00000]],pcm_sha256=prep.sha(pcm),pcm_range=[0x41f10000,0x41f10000+len(prep.manifest_bytes(pcm) if stream else pcm)]))
    electrical=record('electrical.json',dict(**target,schema='a733-hdmi-audio-electrical-v1',authority=fields['authority'],
        **{key:True for key in prep.ELECTRICAL_FACTS},sink_edid_sha256=prep.sha(edid)))
    (out/'sink.edid').write_bytes(edid)
    usb=bytearray(original[656:816])
    for kind,offset in (('initial',16),('electrical',48),('supervision',80)):
        data=dict(**target,schema='a733-hdmi-audio-usb-'+kind+'-v1',
            usb_owner=fields['usb_owner'],usb_handoff=fields['usb_handoff'],**{key:True for key in prep.USB_FACTS[kind]})
        if kind=='electrical':data.update(supply_uv=list(struct.unpack_from('<4I',usb,120)),
            fabric=list(struct.unpack_from('<3I',usb,136)),fabric_hz=list(struct.unpack_from('<3I',usb,148)))
        blob=record('usb-'+kind+'.json',data);usb[offset:offset+32]=hashlib.sha256(blob).digest()
    controller,evidence,_,_,service=struct.unpack_from('<2QI2H',original,632)
    record('dma-security.json',dict(**target,schema='a733-dma-nonsecure-access-v1',controller=controller,
        service_channels=service,dma_evidence=evidence,dma_handoff=struct.unpack_from('<Q',original,608)[0],
        configuration_sha256=prep.sha(original[552:656]),nonsecure_channels=0xffff,
        **{key:True for key in prep.security_authority.FACTS},evidence=[support]))
    record('ddr.json',dict(**target,schema='a733-ddr-reservation-v1',exclusive=True,published_cache_disabled=True,
        remote_writers_excluded=True,ranges=[[0x41000000,0x42200000]],evidence=[support]))
    conditions=('source-backed-cpu-dsu-transition','cold-cache-and-dirty-alias-exclusion','dual-pas-dram',
        'exclusive-cpus-memory-dma','remote-writers-excluded','usb-clock-power-reset-ownership',
        'usb-device-mapping','physical-counter-continuity')
    record('platform.json',dict(**target,schema='a733-smp-observer-platform-v1',
        conditions={key:support for key in conditions},entry_conditions_established=True,physical_coherence_result='under-test'))
    raw_hz=struct.unpack_from('<I',original,300)[0];n=99 if raw_hz==24000000 else 91
    admission=record('admission.json',dict(**target,schema='a733-hdmi-supervised-admission-v1',
        audio='48khz-stereo-s16le-retained-v1',audio_authority=fields['authority'],pcm_sha256=prep.sha(pcm),pcm_bytes=len(pcm),
        analog='analog.json',power='power.json',supervision='supervision.json',audio_memory='memory.json',audio_electrical='electrical.json',
        sink_edid=dict(path='sink.edid',sha256=prep.sha(edid)),dma_nonsecure_access=ref('dma-security.json'),
        usb_capture='copied-retained-v1',usb_initial='usb-initial.json',usb_electrical='usb-electrical.json',usb_supervision='usb-supervision.json',
        reservation=fields['supervisor'],ddr=ref('ddr.json'),platform=ref('platform.json'),
        recovery=dict(mode='manual-usb-reconnect',demonstrated_fel=True,explicit_reconnect_before_resumption=True,
            preserve_microsd=True,one_attempt=True,evidence=[support]),
        clock_profile=dict(raw_dcxo_hz=raw_hz,reference_hz=24000000,sfr_source=0,sfr_hz=24000000,
            pixel_hz={'720p60':raw_hz*n//32,'1080p60':raw_hz*n//16})))
    raw=prep.session_bytes(pattern,pcm,memory=memory,electrical=electrical,authority=fields['authority'],
        dma=original[552:656],admission=admission,usb=bytes(usb),stream=stream)
    (out/'session.bin').write_bytes(raw);(out/'input.pcm').write_bytes(pcm)
    _,address,identity,_=prep.image_metadata(image,session_size=size)
    request,contract=prep.base.request_bytes(address,identity,raw,fields['epoch'],fields['generation'],fields['lease'],fields['supervisor'])
    (out/'request.bin').write_bytes(request);(out/'contract.bin').write_bytes(contract)
    (out/'baseline-session.bin').write_bytes(original)
    summary=dict(physical_access=False,modeled=True,physical_authority=False,image_sha256=image_hash,
        session_sha256=prep.sha(raw),pcm_sha256=prep.sha(pcm),template_contract_sha256=prep.sha(contract),fixture=str(out.resolve()))
    try:prep.validate(args.image,out/'session.bin',out/'admission.json',out/'input.pcm')
    except ValueError as error:assert 'Modeled HDMI audio evidence' in str(error)
    else:raise AssertionError('Modeled authority passed production admission')
    with modeled(prep):prep.validate(args.image,out/'session.bin',out/'admission.json',out/'input.pcm')
    summary['production_refused']=True
    (args.output/'fixture-result.json').write_text(json.dumps(summary,indent=2)+'\n');return summary


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for key in ('image','session','pcm','edid','output'):p.add_argument('--'+key,type=Path,required=True)
    p.add_argument('--stage',choices=('fixture','bundle'),required=True)
    for key in ('clang','lld','objcopy'):p.add_argument('--'+key,type=Path)
    args=p.parse_args();prep=module('hdmi_audio_usb_preparation_check',ROOT/'apps/hdmi-audio/prepare.py')
    if args.stage=='fixture':args.output.mkdir(parents=True,exist_ok=False);summary=fixture(prep,args)
    else:
        out=args.output/'MODEL-NOT-PHYSICAL-fixture';summary=json.loads((args.output/'fixture-result.json').read_bytes())
        assert prep.sha((out/'session.bin').read_bytes())==summary['session_sha256']
        assert all(getattr(args,key) for key in ('clang','lld','objcopy'))
        bundle=args.output/'MODEL-NOT-PHYSICAL-bundle'
        with modeled(prep):prepared=prep.prepare(args.image,out/'session.bin',out/'admission.json',out/'input.pcm',bundle,args.clang,args.lld,args.objcopy)
        summary.update(bundle=str(bundle.resolve()),launcher_sha256=prepared['files']['launch.bin'],passed=True)
        (args.output/'preparation-result.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps(summary))
if __name__=='__main__':main()
