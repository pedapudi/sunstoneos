#!/usr/bin/env python3
"""Compile a source-admitted two-pointer kernel using pinned Mesa, offline."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess

MESA='86c7176d2dd0065555521bf8c61f208bf53c75d6'
CLC_HASH='1281aa7e7b909d496a25f145ebc72bcdebc2755aec83bdde351b700091e9a1d4'
CODE=4032
INPUT=4096
BVNC=(36<<48)|(56<<32)|(104<<16)|183

def digest(raw):return hashlib.sha256(raw).hexdigest()
def run(command,cwd,out,name,env=None):
    p=subprocess.run(command,cwd=cwd,env=env,capture_output=True,text=True)
    (out/(name+'.log')).write_text(p.stdout+p.stderr)
    if p.returncode:raise ValueError(name+' failed; inspect '+str(out/(name+'.log')))
    return p.stdout,p.stderr

def upstream_binary(build,out):
    env={**os.environ,'PCO_DEBUG':'no_pred_cf'}
    command=[str(build/'src/imagination/pco/uscgen/pco_clc'),str(out/'kernel.spv'),str(out/'upstream.h'),str(out/'upstream.c')]
    run(command,build,out,'upstream-usc',env)
    arrays=re.findall(r'static const uint32_t \w+\[\d+\] = \{(.*?)\};',(out/'upstream.c').read_text(),re.S)
    if len(arrays)!=1:raise ValueError('Unexpected upstream program count')
    return b''.join(struct.pack('<I',int(x,16)) for x in re.findall(r'0x[0-9a-fA-F]+',arrays[0])),command

def compile_program(source,mesa,build,out,ninja):
    """The supplied source requires a trusted reviewer; syntax is not a sandbox."""
    source,mesa,build,out=map(lambda p:Path(p).resolve(),(source,mesa,build,out))
    out.mkdir(parents=True,exist_ok=True)
    original=(mesa/'src/imagination/pco/uscgen/pco_clc.c').read_bytes()
    if digest(original)!=CLC_HASH:raise ValueError('Pinned Mesa pco_clc source differs')
    # Instrument only metadata export; compilation passes and code output stay upstream.
    anchor='pco_shader *shader = build_shader(ctx, clone, &data);'
    export=r'''
            const pco_data *m = pco_shader_data(shader);
            fprintf(stderr,"SUNSTONE_META %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u\n",
              m->common.temps,m->common.vtxins,m->common.coeffs,m->common.shareds,
              m->common.entry_offset,m->common.scratch,m->common.spilled_temps,
              m->common.uses.atomics,m->common.uses.barriers,m->common.uses.usclib,
              m->cs.workgroup_size[0],m->cs.workgroup_size[1],m->cs.workgroup_size[2],
              m->cs.shmem.count,m->cs.zero_shmem,m->cs.global_shmem,
              m->common.uses.point_sampler,m->common.uses.ia_sampler);
'''
    patched=original.decode().replace(anchor,anchor+export)
    (out/'pco_clc_metadata.c').write_text(patched)
    entries=json.loads((build/'compile_commands.json').read_text())
    entry=next(e for e in entries if e['file'].endswith('/pco_clc.c'))
    cc=shlex.split(entry['command']);cc[cc.index('-o')+1]=str(out/'metadata.o')
    cc[-1]=str(out/'pco_clc_metadata.c')
    # Quoted local includes belong to the unchanged source directory.
    cc+=['-I'+str(mesa/'src/imagination/pco/uscgen')]
    commands=[cc]
    run(cc,build,out,'metadata-build')
    link=shlex.split(subprocess.check_output([str(ninja),'-t','commands','src/imagination/pco/uscgen/pco_clc'],cwd=build,text=True).splitlines()[-1])
    link[link.index('-o')+1]=str(out/'pco_clc_metadata')
    link=[str(out/'metadata.o') if x=='src/imagination/pco/uscgen/pco_clc.p/pco_clc.c.o' else x for x in link]
    commands.append(link);run(link,build,out,'metadata-link')
    includes=['include','src/compiler/libcl','src/imagination/pco/usclib','src/imagination','src/imagination/include','src']
    clc=[str(build/'src/compiler/clc/mesa_clc'),'-o',str(out/'kernel.spv'),str(source),'--']
    clc+=['-I'+str(mesa/x) for x in includes]+['-I'+str(build/'src/imagination'),'-cl-std=cl2.0','-D__OPENCL_VERSION__=200','-DHAVE___BUILTIN_FFS','-DHAVE___BUILTIN_CLZ','-DNDEBUG']
    commands.append(clc);run(clc,build,out,'spirv')
    env={**os.environ,'PCO_DEBUG':'no_pred_cf','PYTHONDONTWRITEBYTECODE':'1'}
    cmd=[str(out/'pco_clc_metadata'),str(out/'kernel.spv'),str(out/'kernel.h'),str(out/'kernel.c')]
    commands.append(cmd);_,err=run(cmd,build,out,'usc',env)
    lines=re.findall(r'^SUNSTONE_META (.*)$',err,re.M)
    if len(lines)!=1:raise ValueError('Require exactly one common-profile entrypoint')
    meta=list(map(int,lines[0].split()))
    if meta[2:10]!=[0]*8 or meta[10:13]!=[32,1,1] or any(meta[13:]) or meta[1]>4 or (meta[0]+3)//4>63:
        raise ValueError('Unsupported compiler metadata: '+str(meta))
    arrays=re.findall(r'static const uint32_t \w+\[\d+\] = \{(.*?)\};',(out/'kernel.c').read_text(),re.S)
    if len(arrays)!=1:raise ValueError('Require exactly one common binary')
    blob=b''.join(struct.pack('<I',int(x,16)) for x in re.findall(r'0x[0-9a-fA-F]+',arrays[0]))
    if len(blob)<12 or len(blob)!=8+int.from_bytes(blob[5:7],'little')*4 or len(blob)-8>CODE:raise ValueError('Invalid USC extent')
    if blob[:2]!=bytes(meta[:2]) or any(blob[2:5]):raise ValueError('Metadata/header disagreement')
    upstream,command=upstream_binary(build,out)
    commands.append(command)
    if upstream!=blob:raise ValueError('Instrumented compiler changed USC bytes')
    (out/'program.bin').write_bytes(blob)
    (out/'source.cl').write_bytes(source.read_bytes())
    manifest={'schema':'sunstone-pco-profile-v1','mesa_revision':MESA,'source_sha256':digest(source.read_bytes()),
      'spirv_sha256':digest((out/'kernel.spv').read_bytes()),'program_sha256':digest(blob),'metadata':meta,
      'compiler_sha256':digest((out/'pco_clc_metadata').read_bytes()),
      'upstream_compiler_sha256':digest((build/'src/imagination/pco/uscgen/pco_clc').read_bytes()),
      'metadata_export_source_sha256':digest(patched.encode()),'commands':commands,
      'source_admission':'Required independently; metadata does not prove USC memory behavior.'}
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return manifest

def package(program,manifest,input_data,output_bytes,input_header_bytes,input_element_bytes,output_element_bytes):
    m=json.loads(Path(manifest).read_text());blob=Path(program).read_bytes();data=Path(input_data).read_bytes()
    if m['schema']!='sunstone-pco-profile-v1' or m['mesa_revision']!=MESA or digest(blob)!=m['program_sha256']:raise ValueError('Program identity differs')
    if len(data)<4 or len(data)>INPUT or not 0<output_bytes<=3968 or output_bytes%4:raise ValueError('Invalid data bounds')
    meta=m['metadata']
    if len(meta)!=18 or meta[2:10]!=[0]*8 or meta[10:13]!=[32,1,1] or any(meta[13:]):raise ValueError('Unsupported profile')
    header=struct.pack('<4IQ8I4I',0x50435650,1,8288,1,BVNC,len(blob)-8,len(data),output_bytes,struct.unpack_from('<I',data)[0],0,0,0,0,32,1,1,0)
    header+=blob[:8]+bytes.fromhex(m['source_sha256'])+bytes.fromhex(m['compiler_sha256'])+struct.pack('<4I',input_header_bytes,input_element_bytes,output_element_bytes,0)
    count=struct.unpack_from('<I',data)[0]
    if not count or input_header_bytes<4 or input_element_bytes<1 or output_element_bytes<1 or len(data)!=input_header_bytes+count*input_element_bytes or output_bytes!=count*output_element_bytes:raise ValueError('Caller data violates trusted element bounds')
    return header+blob[8:]+bytes(CODE-(len(blob)-8))+data+bytes(INPUT-len(data))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='operation',required=True)
    c=sub.add_parser('compile')
    for x in ('source','mesa','build','output','ninja'):c.add_argument('--'+x,type=Path,required=True)
    b=sub.add_parser('package')
    for x in ('program','manifest','input','output'):b.add_argument('--'+x,type=Path,required=True)
    for x in ('output-bytes','input-header-bytes','input-element-bytes','output-element-bytes'):b.add_argument('--'+x,type=int,required=True)
    a=p.parse_args()
    if a.operation=='compile':compile_program(a.source,a.mesa,a.build,a.output,a.ninja)
    else:a.output.write_bytes(package(a.program,a.manifest,a.input,a.output_bytes,a.input_header_bytes,a.input_element_bytes,a.output_element_bytes))
