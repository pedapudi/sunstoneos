#!/usr/bin/env python3
"""Only new fixture-pair admission and copied nonconstant host acceptance."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def run(native,oracle,out):
    out.mkdir(parents=True,exist_ok=False);host=module('jpeg_nonconstant_host',ROOT/'tools/check-a733-jpeg-supervised.py')
    session=(native/'session-fixture.bin').read_bytes();reference=(oracle/'reference.yuv').read_bytes();frame=(native/'native-frame.bin').read_bytes()
    assert host.preparation.fixtures.profile_for(session[768:800].hex(),session[800:832].hex())['fixture_bytes']==520
    legacy=host.preparation.fixtures.profiles()[0]
    assert host.preparation.fixtures.profile_for(legacy['fixture_sha256'],legacy['reference_sha256'])['fixture_bytes']==155
    for changed in ('fixture','reference'):
        bad=bytearray(session);start=768 if changed=='fixture' else 800
        bad[start:start+32]=bytes.fromhex(legacy[changed+'_sha256'])
        try:host.preparation.session_check(bad)
        except ValueError:pass
        else:raise AssertionError('Cross-paired JPEG hashes accepted')
    calls=[];clock=[1.];record={}
    pending=struct.pack('<4I2Q',0x4745504a,1,0,0,1,2)+session[768:832]+bytes(32)
    def exchange(packet,timeout):
        assert packet==bytes(1) and 0<timeout<=500;calls.append(True)
        return pending if len(calls)==1 else frame
    host.capture(exchange,session,out,record,lambda:None,60,now=lambda:clock[0],pause=lambda seconds:clock.__setitem__(0,clock[0]+seconds),reference=reference)
    assert record['jpeg_result']=='observed-success' and (out/'output.yu12').read_bytes()==reference
    rejected=[]
    for value in (bytes([128])*2304,reference[:1536]+reference[1920:]+reference[1536:1920]):
        forged=bytearray(frame);forged[128:]=value;forged[96:128]=hashlib.sha256(value).digest()
        try:host.decoder.decode(forged,session,reference)
        except ValueError:rejected.append(True)
        else:raise AssertionError('Wrong copied output accepted with matching transport hash')
    # Pair validation fails before the inherited CMake target is constructed.
    model=out/'cross-pair';model.mkdir();(model/'constant.yuv').write_bytes(bytes([128])*2304)
    (model/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.24)\nproject(pair NONE)\nadd_library(sunstone-a733-board INTERFACE)\nset_property(TARGET sunstone-a733-board PROPERTY COMPILE_DEFINITIONS SUNSTONE_BOARD_SUPERVISED=1)\nset(SUNSTONE_SOURCE_ROOT "'+str(ROOT)+'")\nset(SUNSTONE_JPEG_FIXTURE "'+str(oracle/'fixture.jpg')+'")\nset(SUNSTONE_JPEG_REFERENCE "'+str(model/'constant.yuv')+'")\ninclude("'+str(ROOT/'cmake/a733-jpeg.cmake')+'")\n')
    command=['cmake','-S',str(model),'-B',str(model/'build')];result=subprocess.run(command,capture_output=True,text=True)
    (out/'cross-pair.log').write_text(result.stdout+result.stderr)
    assert result.returncode and 'JPEG fixed fixture/reference pair mismatch' in result.stderr
    (out/'result.json').write_text(json.dumps({'passed':True,'hardware_access':False,'scope':'Both allowed identities; crossed input/reference refusals in preparation and CMake; pending-to-durable native frame capture; constant/swapped payload refusal despite matching transport hash. Existing transport/lifecycle checks reused.','commands':[command]},indent=2)+'\n')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('native','oracle','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();run(a.native,a.oracle,a.output)
