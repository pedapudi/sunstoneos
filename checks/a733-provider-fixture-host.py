#!/usr/bin/env python3
"""Verify changed software fixture setup without native builds or emulation."""
import argparse,hashlib,importlib.util,json,struct,subprocess,sys
from pathlib import Path
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
CACHE=Path.home()/'.cache/sunstoneos/evidence'
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def refused(call):
    try:call()
    except ValueError:return
    raise AssertionError('Incomplete nominal provider fixture accepted')

def run(output):
    output.mkdir(parents=True,exist_ok=False);results={}
    def passed(name):
        results[name]=True;(output/'completed.json').write_text(json.dumps(results,indent=2)+'\n')
    providers=module('nominal_providers',ROOT/'checks/a733-provider-fixture.py')
    source=providers.SOURCE.read_bytes();providers.validate_nominal_providers(source)
    passed('pinned-accepted-thermal-provider-source')
    original=(CACHE/'crypto-single-core-20261018/session-fixture.bin').read_bytes()
    incomplete=bytearray(original);incomplete[492:500]=bytes(8);incomplete[564:570]=bytes(6)
    crypto=providers.nominal_provider_session(incomplete)
    assert crypto[488:570]==source[488:570]
    assert crypto[:488]==incomplete[:488] and crypto[570:]==incomplete[570:]
    assert crypto[:488]==original[:488] and crypto[570:]==original[570:]
    assert len(crypto)==824
    prep=module('crypto_prepare',ROOT/'apps/crypto/prepare.py')
    crypto_image=CACHE/'crypto-single-core-20261018/image.elf'
    _,_,build=prep.base.image_parts(crypto_image);crypto[48:80]=build
    prep.session_check(crypto);prep.embedded_reference(crypto_image.read_bytes(),crypto)
    passed('crypto-provider-construction-preserves-identity-authority-and-extension')
    service=module('pwm_service',ROOT/'checks/pwm-service-fixture.py')
    pwm_image=CACHE/'pwm-service-20261018/build/image.elf'
    template,_=service.produce(pwm_image,output/'pwm-service')
    providers.validate_nominal_providers(template.read_bytes(),usb_reference_mode=1)
    single=module('pwm_single',ROOT/'checks/pwm-single-core-fixture.py')
    single_image=CACHE/'pwm-single-core-20261018/image.elf'
    template,_=single.produce(single_image,output/'pwm-single-core')
    providers.validate_nominal_providers(template.read_bytes(),usb_reference_mode=2)
    passed('actual-pwm-service-and-boot-cpu-producers-validate')
    for offset in (492,496,500,512,516,532,564,567):
        raw=bytearray(crypto);raw[offset:offset+(4 if offset<564 else 1)]=bytes(4 if offset<564 else 1)
        refused(lambda:providers.validate_nominal_providers(raw))
    for offset in (0,8,16,400,404,456,464):
        raw=bytearray(crypto);raw[offset]^=1
        # Owners remain valid when changed; explicitly zero them to test absence.
        if offset in (456,464):raw[offset:offset+8]=bytes(8)
        refused(lambda:providers.validate_nominal_providers(raw))
    raw=bytearray(crypto);raw[464:472]=raw[456:464];refused(lambda:providers.validate_nominal_providers(raw))
    refused(lambda:providers.validate_nominal_providers(crypto,usb_reference_mode=1))
    passed('incomplete-clocks-supplies-pins-abi-workload-owners-and-mode-refused')
    altered=output/'altered-source.bin';altered.write_bytes(source[:-1]+bytes([source[-1]^1]))
    with patch.object(providers,'SOURCE',altered):refused(lambda:providers.nominal_provider_session(original))
    passed('changed-source-pin-refused')
    # Execute only each actual startup fixture's setup guard. An unusable image
    # ensures an incorrect ordering fails before any ELF/disassembly/emulation.
    guards={}
    for name,raw,offset in [('crypto',crypto,492),('pwm',bytearray(template.read_bytes()),564)]:
        invalid=bytearray(raw);invalid[offset:offset+(4 if offset<564 else 1)]=bytes(4 if offset<564 else 1)
        session=output/(name+'-incomplete.bin');session.write_bytes(invalid)
        command=[sys.executable,str(ROOT/'checks'/f'{name}-single-core.py'),'--image',str(output/'DO-NOT-OPEN.elf'),'--session',str(session),'--output',str(output/(name+'-guard'))]
        if name=='pwm':command+=['--manifest',str(output/'DO-NOT-OPEN.json')]
        result=subprocess.run(command,capture_output=True,text=True)
        (output/(name+'-guard.log')).write_text(result.stdout+result.stderr)
        assert result.returncode!=0 and 'Nominal PMIC/USB profile is incomplete' in result.stderr
        assert 'DO-NOT-OPEN' not in result.stderr
        guards[name]=command
    passed('actual-pwm-and-crypto-startup-reject-before-elf-or-instructions')
    inputs={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in (providers.SOURCE,CACHE/'crypto-single-core-20261018/session-fixture.bin',crypto_image,pwm_image,single_image)}
    (output/'result.json').write_text(json.dumps(dict(physical_access=False,cases=results,inputs=inputs,guard_commands=guards,limits='Pure-host fixture generation, production session/image validation and early-rejection setup only. No native image build, peripheral instruction run or physical authority.'),indent=2)+'\n')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True);run(parser.parse_args().output)
