#!/usr/bin/env python3
"""Check program session identity, native linked copy survival, and output decoding."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for x in ('evidence','image','manifest','base-session'):p.add_argument('--'+x,type=Path,required=True)
    p.add_argument('--count',type=int,default=127)
    a=p.parse_args();prep=module('program_prepare',ROOT/'apps/gpu-program/prepare.py')
    observe=module('program_observe',ROOT/'apps/gpu-program/observe.py')
    canonical=module('gpu_prepare',ROOT/'apps/gpu-render/prepare.py')
    def refuse(fn):
        try:fn()
        except (ValueError,RuntimeError):return
        raise AssertionError('Invalid input accepted')
    with a.image.open('rb') as f:
        elf=ELFFile(f);syms={s.name:(s['st_value'],s['st_size']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
        uploaded=elf.get_section_by_name('.gpu_session')
        assert uploaded['sh_addr']==0x41f00000 and uploaded['sh_size']>=8944
    first=syms['program_session'][0];last=first+syms['program_session'][1]
    bss_first=syms['__bss_start'][0];bss_last=syms['__bss_end'][0]
    assert last-first==8944 and last<=bss_first and first==syms['__gpu_program_copy_start'][0] and last==syms['__gpu_program_copy_end'][0]
    image_hash=hashlib.sha256(a.image.read_bytes()).hexdigest();results=[]
    for name in ('affine','add'):
        pkg=(a.evidence/name/f'package-{a.count}.bin').read_bytes()
        record={'schema':'sunstone-gpu-trusted-program-v1','source_reviewed':True,
            'package_sha256':hashlib.sha256(pkg).hexdigest(),'source_sha256':pkg[80:112].hex(),'compiler_sha256':pkg[112:144].hex()}
        raw=prep.envelope(a.base_session.read_bytes(),pkg,json.dumps(record));canonical.session_check(raw)
        (a.evidence/name/f'session-{a.count}.bin').write_bytes(raw)
        (a.evidence/name/f'source-review-MODELED-{a.count}.json').write_text(json.dumps({**record,'synthetic_fixture':True},indent=2)+'\n')
        canonical.image_manifest_check(json.loads(a.manifest.read_text()),a.image.read_bytes(),raw)
        admission={'gpu_program':{'path':'synthetic-review','sha256':hashlib.sha256(json.dumps(record).encode()).hexdigest()}}
        cache={'synthetic-review':json.dumps(record).encode()}
        canonical.program_admission(admission,cache,raw)
        refuse(lambda:canonical.program_admission({},cache,raw))
        changed=bytearray(raw);changed[-1]^=1;refuse(lambda:canonical.session_check(changed))
        changed=bytearray(raw);changed[736]^=1
        refuse(lambda:canonical.program_admission(admission,cache,changed))
        # Model the existing EL1 clear using the actual linked addresses and a
        # complete caller-selected envelope. This checks placement, not startup.
        memory=bytearray([0xa5])*(bss_last-first)
        memory[:len(raw)]=raw;memory[bss_first-first:bss_last-first]=bytes(bss_last-bss_first)
        assert memory[:len(raw)]==raw
        epoch=17;expected=(a.evidence/name/f'expected-{a.count}.bin').read_bytes();copied=bytearray();total=len(expected)//4
        for chunk in range((total+31)//32):
            count=min(32,total-32*chunk);data=expected[chunk*128:chunk*128+count*4]
            response=struct.pack('<10I',0x50555047,1,1,chunk,count,total,1,epoch,0,total)+data
            copied+=observe.decode(response,chunk,epoch,total)
            refuse(lambda:observe.decode(response,chunk,epoch+1,total))
        assert copied==expected
        results.append({'kernel':name,'package_version':struct.unpack_from('<I',pkg,4)[0],'dispatch_count':struct.unpack_from('<I',pkg,68)[0],'element_count':a.count,'elf_sha256':image_hash,'session_sha256':hashlib.sha256(raw).hexdigest(),
            'copied_output_sha256':hashlib.sha256(copied).hexdigest()})
    (a.evidence/'host-check.json').write_text(json.dumps({'scope':'Synthetic source-review records; actual compiler package inputs and linked BSS bounds; no physical admission or launch',
        'results':results,'retained':[first,last],'bss':[bss_first,bss_last],'upload_bytes':int(uploaded['sh_size'])},indent=2)+'\n')
    print('PASS two program envelopes use identical ELF, survive modeled native BSS clear, bind source review and decode paged output')
if __name__=='__main__':main()
