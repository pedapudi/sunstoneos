#!/usr/bin/env python3
"""Offline ELF/launcher/package binding; physical authority is explicitly modeled."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
from unittest.mock import patch
from contextlib import ExitStack,nullcontext
import types
import sys
import time
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def routing(host,bundle,out,session):
    """Actual main and shared selector/lookup; target/transport authority is modeled."""
    import usb.core
    shared=host.supervisor;port=out/'sys/1-2';port.mkdir(parents=True);old=out/'001/002';native=out/'001/003'
    for name,value in [('bcdDevice','0100'),('product',host.PRODUCT),('serial',host.SERIAL)]: (port/name).write_text(value)
    access=module('crypto_routing_access',ROOT/'tools/usb-access.py')
    # Real discovery and access-rule selectors operate only on local fixture files.
    for name,value in [('idVendor','1f3a'),('idProduct','efe8'),('busnum','1'),('devnum','3')]: (port/name).write_text(value)
    access.SYS_USB=port.parent;access.DEV_USB=out
    access.board_lock=lambda:nullcontext(old);access.require_session=lambda n:None;access.require_rom=lambda n:None
    access.identity=lambda n:'initial' if n==old else 'native';access.target=lambda n:port;access.quarantine_instance=lambda *a:None
    assert access.session_node(dict(port='1-2',sysfs=str(port.resolve())))==native
    assert 'ATTR{idProduct}=="efe8"' in access.session_rule('1-2',1000)
    guard=types.SimpleNamespace(__file__=ROOT/'tools/usb-access.py',check_health=lambda:None,reenumeration=lambda *a:nullcontext())
    calls=[]
    frame=__import__('struct').pack('<4I2Q',0x54505943,1,1,160,*__import__('struct').unpack_from('<2Q',session,24))+session[760:824]+hashlib.sha256(host.REFERENCE).digest()+host.REFERENCE
    class Device:
        bcdDevice=0x0100
        def set_configuration(self,n):assert n==1
        def write(self,ep,data,timeout):assert ep==2 and data==bytes(1);return 1
        def read(self,ep,n,timeout):assert ep==0x81 and n==288;return frame
    def find(**kw):
        assert kw==dict(idVendor=0x1f3a,idProduct=0xefe8,bus=1,address=3);calls.append(kw);return Device()
    def launch(transaction,observe,bundle,prepared,evidence,record,save,ready,*,result_key):
        assert result_key=='crypto_result';observe(time.monotonic());record['complete']=record[result_key]=='observed-success'
    with ExitStack() as stack:
        stack.enter_context(patch.object(host,'claimed',lambda *a:None));stack.enter_context(patch.object(shared,'module',lambda *a:access))
        stack.enter_context(patch.dict(sys.modules,{'usb_guard':guard}));stack.enter_context(patch.object(shared,'digest',lambda *a:shared.base.FEL_HASH))
        stack.enter_context(patch.object(shared.shutil,'copy2',lambda *a:None));stack.enter_context(patch.object(shared.os,'access',lambda *a:True))
        stack.enter_context(patch.object(shared.subprocess,'run',lambda *a,**k:types.SimpleNamespace(returncode=0,stdout='soc=00001903(A733)',stderr='')))
        stack.enter_context(patch.object(shared,'launch_once',launch));stack.enter_context(patch.object(shared,'finish_after_observation',lambda *a:None))
        stack.enter_context(patch.object(shared.base,'stop_after_failure',lambda *a:None));stack.enter_context(patch.object(usb.core,'find',find))
        args=['--bundle',str(bundle),'--fel',str(out/'MODEL-fel'),'--evidence',str(out/'routing-capture'),'--database',str(out/'MODEL-db'),'--claim','MODEL','--owner','MODEL','--supervised-manual-reconnect']
        assert host.main(args)==0
    assert len(calls)==1

def run(image,session,out,clang,lld):
    out.mkdir(parents=True,exist_ok=False);host=module('crypto_package_host',ROOT/'tools/check-a733-crypto-supervised.py');prep=host.preparation
    (out/'MODEL-NOT-DEPLOYABLE.txt').write_text('Only offline package geometry is exercised. Physical entry authority is mocked. Never deploy this model bundle.\n')
    manifest=out/'image-input-manifest.json';manifest.write_text(json.dumps(dict(target='sunstone-a733-crypto',execution_profile='crypto-supervised-v1',final_elf=hashlib.sha256(image.read_bytes()).hexdigest())))
    admission=out/'MODEL-admission.json';admission.write_text(json.dumps(dict(schema='a733-crypto-supervised-entry-v1',reservation=1,model_only=True,identity='initial',port='1-2')))
    with patch.object(prep,'validate_admission',return_value={}):
        prep.prepare(image,manifest,admission,session,out/'MODEL-bundle',clang,lld)
        prepared=host.freeze_bundle(out/'MODEL-bundle');assert prepared['_reference']==host.REFERENCE
        routing(host,out/'MODEL-bundle',out,(out/'MODEL-bundle/session.bin').read_bytes())
        for filename in ('session.bin','contract.bin','segment-0.bin','launch.bin'):
            path=out/'MODEL-bundle'/filename;original=path.read_bytes();path.write_bytes(bytes([original[0]^1])+original[1:])
            try:prep.validate_bundle(out/'MODEL-bundle')
            except ValueError:pass
            else:raise AssertionError('Changed sealed file accepted:'+filename)
            path.write_bytes(original)
        prep.validate_bundle(out/'MODEL-bundle')
    try:prep.validate_bundle(out/'MODEL-bundle')
    except (ValueError,KeyError):pass
    else:raise AssertionError('Packaging model accepted without physical-authority mock')
    (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,scope='Actual ELF immutable vectors, ARM launcher compile/link and public preparation/freeze; sealed session/contract/segment/launcher refusals. Physical admission mocked; resulting model bundle is not deployable.'),indent=2)+'\n')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','session','output','clang','lld'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();run(a.image,a.session,a.output,a.clang,a.lld)
