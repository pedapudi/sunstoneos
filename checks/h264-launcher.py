#!/usr/bin/env python3
"""Exercise the generated AArch32 H.264 launcher; retain the native ELF unchanged."""
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
    p.add_argument('--prior-freeze',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--clang',default='/usr/bin/clang-21')
    p.add_argument('--lld',default='/home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21')
    a=p.parse_args();a.output.mkdir(exist_ok=False)
    prior=json.loads(a.prior_freeze.read_text());prior_root=a.prior_freeze.parent.parent
    inputs={}
    def preserved(name):
        original=str(prior_root/name)
        rows=[r for r in prior['artifacts'] if r['original']==original];assert len(rows)==1,original
        row=rows[0];path=a.prior_freeze.parent/row['snapshot'];data=path.read_bytes()
        assert digest(data)==row['sha256'];inputs[name]={'snapshot':str(path),'sha256':digest(data)}
        return data
    native=preserved('build/sunstone-a733-h264.elf');image=a.output/'accepted-native.elf';image.write_bytes(native)
    old_launcher=preserved('final-bundles/normal-bundle/launch.bin')
    producer=preserved('admission');producer_path=a.output/'session-producer';producer_path.write_bytes(producer);producer_path.chmod(0o700)
    # This is the unchanged admitted-session producer from the accepted freeze.
    source=prior['sources']['checks/h264-application-admission.c']
    assert digest((ROOT/'checks/h264-application-admission.c').read_bytes())==source['sha256']
    command=[str(producer_path),str(a.output/'legacy-template.bin')]
    result=subprocess.run(command,capture_output=True,text=True)
    (a.output/'session-producer.log').write_text(result.stdout+result.stderr);assert result.returncode==0
    spec=importlib.util.spec_from_file_location('h264_prepare',ROOT/'apps/h264/prepare.py');prepare=importlib.util.module_from_spec(spec);spec.loader.exec_module(prepare)
    segments,id_address,identity=prepare.base.image_parts(image)
    templates={'legacy':(a.output/'legacy-template.bin').read_bytes(),'VGA':preserved('normal-session.bin')}
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
    check('legacy-version1',templates['legacy'],1,12)
    check('VGA-version2',templates['VGA'],2,12)
    check('unsupported-version3',templates['VGA'],3,14)
    check('prior-launcher-refuses-VGA',templates['VGA'],2,14,old_launcher)
    assert image.read_bytes()==native
    report={'passed':True,'physical_access':False,'native_image_sha256':digest(native),'native_image_rebuilt':False,
        'launcher_sha256':digest(binary),'prior_launcher_sha256':digest(old_launcher),'inputs':inputs,
        'session_producer_command':command,'cases':cases,
        'coverage_gap':'Previous VGA host/bundle checks substituted launcher construction and did not execute its actual session-version guard. The retained old launcher refuses VGA with reason14.',
        'scope':'Actual generated AArch32 launcher entry/admission/publication/refusal/restoration instructions. CPU system-register/counter values and MMIO are modeled. Stops before reset/hardware handoff; no AArch64 startup, USB or physical qualification.'}
    (a.output/'result.json').write_text(json.dumps(report,indent=2)+'\n');print('PASS legacy/VGA actual AArch32 admission, unsupported-version refusal and prior VGA refusal')
if __name__=='__main__':main()
