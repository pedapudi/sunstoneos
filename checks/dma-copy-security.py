#!/usr/bin/env python3
"""Focused external DMA0 security admission; no device, image build or execution.

The existing ELF supplies immutable controller/channel/session identities.
All observations and physical assertions remain modeled. Positive validation
substitutes only the physical-authority decision inside this checker; production
must reject the unchanged modeled record. Other inherited platform conditions
are digest-bound model notes, not physical evidence or replayed entry checks.
"""
import argparse,copy,hashlib,importlib.util,json
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--image',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
spec=importlib.util.spec_from_file_location('dma_security_host',ROOT/'apps/dma-copy/observe.py')
host=importlib.util.module_from_spec(spec);spec.loader.exec_module(host)
image=a.image.read_bytes();profile=host.image_profile(a.image)
assert profile['controller']==0x04601000 and profile['channels'] and profile['channels']<=0xffff
prepared=dict(profile,image_sha256=hashlib.sha256(image).hexdigest())
identity,port='MODEL-NOT-PHYSICAL','no-device'
def record(name,value):
    data=(json.dumps(value,sort_keys=True,indent=2)+'\n').encode();path=(a.output/name).resolve();path.write_bytes(data)
    return {'path':str(path),'sha256':hashlib.sha256(data).hexdigest()},data
note,_=record('MODEL-NOT-PHYSICAL.json',{'modeled':True,'physical_authority':False,'scope':__doc__})
security=dict(schema='a733-dma-nonsecure-access-v1',identity=identity,port=port,
    image_sha256=prepared['image_sha256'],reviewed=True,physical_authority=False,modeled=True,
    service_channels=profile['channels'],nonsecure_channels=profile['channels'],
    controller_access_nonsecure_el1=True,descriptor_data_access_nonsecure=True,
    valid_after_reset_release=True,security_writers_excluded_until_recovery=True,evidence=[note])
for name in ('epoch','generation','lease','controller','dma_evidence','dma_handoff'):security[name]=profile[name]
conditions={name:note for name in ('source-backed-cpu-dsu-transition','cold-cache-and-dirty-alias-exclusion',
    'dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','usb-clock-power-reset-ownership',
    'usb-device-mapping','physical-counter-continuity','dma-startup-drain-ownership')}
platform=dict(schema='a733-dma-copy-platform-v1',identity=identity,port=port,image_sha256=prepared['image_sha256'],
    conditions=conditions,entry_conditions_established=True,physical_coherence_result='under-test')
for name in ('epoch','generation','lease','usb_evidence','dma_evidence'):platform[name]=profile[name]
original_require=host.require;overrides=0
def modeled_require(condition,message):
    global overrides
    if message=='DMA security requires reviewed physical authority.':
        assert condition is False and fact['modeled'] is True and fact['physical_authority'] is False
        assert fact['reviewed'] is True
        overrides+=1;return
    original_require(condition,message)
cases=[]
for case in ('missing-authority','modeled-production','missing-mask','wrong-controller',
             'wrong-session','boolean-mask','missing-post-reset','nominal-model'):
    fact=copy.deepcopy(security);value=copy.deepcopy(platform)
    if case=='missing-mask':fact.pop('nonsecure_channels')
    if case=='wrong-controller':fact['controller']=0x04024000
    if case=='wrong-session':fact['generation']^=1
    if case=='boolean-mask':fact['nonsecure_channels']=True
    if case=='missing-post-reset':fact['valid_after_reset_release']=False
    ref,raw=record(case+'-security.json',fact)
    if case!='missing-authority':value['conditions']['dma-nonsecure-access']=ref
    record(case+'-platform.json',value)
    if case=='modeled-production':
        try:host.validate_platform(value,prepared,identity,port)
        except ValueError as error:assert 'reviewed physical authority' in str(error)
        else:raise AssertionError('Production accepted modeled DMA security')
        cases.append(case);continue
    with patch.object(host,'require',modeled_require):
        try:verified=host.validate_platform(value,prepared,identity,port)
        except ValueError as error:
            expected={'missing-authority':'conditions are incomplete','missing-mask':'omits selected',
                'wrong-controller':'binding differs: controller','wrong-session':'binding differs: generation',
                'boolean-mask':'omits selected','missing-post-reset':'access/lifetime'}[case]
            assert expected in str(error),(case,str(error))
        else:assert case=='nominal-model' and verified==raw
    cases.append(case)
assert a.image.read_bytes()==image
assert overrides==6
report=dict(passed=True,cases=cases,physical_authority=False,modeled=True,scope=__doc__,
    image_sha256=prepared['image_sha256'],immutable_profile=profile,model_authority_overrides=overrides,
    sources={str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in
        (Path(__file__),ROOT/'apps/dma-copy/observe.py',ROOT/'tools/check-a733-smp.py',ROOT/'tools/check-a733-smp-observe.py')})
(a.output/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({'passed':True,'cases':len(cases),'physical_authority':False}))
