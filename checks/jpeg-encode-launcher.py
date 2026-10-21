#!/usr/bin/env python3
"""Exercise the generated AArch32 JPEG encoder launcher; retain the native ELF unchanged."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
from elftools.elf.elffile import ELFFile
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'checks'))
import sram_launcher

def digest(data):return hashlib.sha256(data).hexdigest()
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image',type=Path,required=True)
    p.add_argument('--producer',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--clang',default='/usr/bin/clang-21')
    p.add_argument('--lld',default='/home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21')
    a=p.parse_args();a.output.mkdir(exist_ok=False)
    native=a.image.read_bytes();image=a.image
    producer_path=a.producer
    command=[str(producer_path),str(a.output/'template.bin')]
    result=subprocess.run(command,capture_output=True,text=True)
    (a.output/'session-producer.log').write_text(result.stdout+result.stderr);assert result.returncode==0
    spec=importlib.util.spec_from_file_location('encoder_prepare',ROOT/'apps/jpeg-encode/prepare.py');prepare=importlib.util.module_from_spec(spec);spec.loader.exec_module(prepare)
    segments,id_address,identity=prepare.base.image_parts(image)
    template=(a.output/'template.bin').read_bytes()
    launcher=a.output/'launcher';launcher.mkdir();prepare.build_launcher(launcher,a.clang,a.lld)
    binary=(launcher/'launch.bin').read_bytes()
    with (launcher/'launch.elf').open('rb') as stream:
        elf=ELFFile(stream);symbols={s.name:int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
    assert symbols['a733_smp_launch']==0x52000 and 0<len(binary)<=0xc00
    cases=[]
    def check(name,template,version,expected_reason,code=binary):
        session=bytearray(template);struct.pack_into('<3Q',session,24,31,37,41);session[48:80]=identity
        # Mutate the unsupported case after the actual producer accepts its template.
        prepare.session_check(session);struct.pack_into('<Q',session,600,version)
        request,contract=prepare.request_bytes(id_address,identity,session,31,37,41)
        folder=a.output/name;folder.mkdir()
        for label,data in [('session',session),('request',request),('contract',contract)]: (folder/(label+'.bin')).write_bytes(data)
        launch=sram_launcher.execute([(s['address'],s['data']) for s in segments],code,request,contract,session,prepare.CONTRACT_MAGIC,224,name)
        sram_launcher.verify(launch,expected_reason,contract,prepare.CONTRACT_MAGIC,60,(31,37,41))
        report,stop,reads,writes,ticks=launch.report,launch.stop,launch.reads,launch.writes,launch.ticks
        cases.append({'case':name,'session_version':version,'reason':report[0],'stop':stop[0],
            'mmio_reads':[hex(x) for x in reads],'mmio_writes':writes,'counter_reads':ticks})
    check('encoder-version1',template,1,12)
    check('unsupported-version2',template,2,14)
    assert image.read_bytes()==native
    report={'passed':True,'physical_access':False,'native_image_sha256':digest(native),'native_image_rebuilt':False,
        'launcher_sha256':digest(binary),'producer_sha256':digest(producer_path.read_bytes()),
        'session_producer_command':command,'cases':cases,
        'coverage':'Encoder version1 handoff and invalid version2 refusal use actual generated launcher and producer-built session.',
        'scope':'Actual generated AArch32 launcher entry/admission/publication/refusal/restoration instructions. CPU system-register/counter values and MMIO are modeled. Stops before reset/hardware handoff; no AArch64 startup, USB or physical qualification.'}
    (a.output/'result.json').write_text(json.dumps(report,indent=2)+'\n');print('PASS encoder actual AArch32 admission and invalid-version refusal')
if __name__=='__main__':main()
