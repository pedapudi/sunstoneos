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

def upstream_binary(build,out,compiler=None):
    env={**os.environ,'PCO_DEBUG':'no_pred_cf'}
    command=[str(compiler or build/'src/imagination/pco/uscgen/pco_clc'),str(out/'kernel.spv'),str(out/'upstream.h'),str(out/'upstream.c')]
    run(command,build,out,'upstream-usc',env)
    arrays=re.findall(r'static const uint32_t \w+\[\d+\] = \{(.*?)\};',(out/'upstream.c').read_text(),re.S)
    if len(arrays)!=1:raise ValueError('Unexpected upstream program count')
    return b''.join(struct.pack('<I',int(x,16)) for x in re.findall(r'0x[0-9a-fA-F]+',arrays[0])),command

def metadata_valid(meta,profile):
    if len(meta)!=18 or any(meta[3:10]) or any(meta[13:]) or (meta[0]+3)//4>63:return False
    if profile==3:
        return meta[1:3]==[5,7] and meta[12]==1 and 0<meta[10]<=32 and 0<meta[11]<=32 and meta[10]*meta[11]==32
    return meta[2]==0 and meta[10:13]==[32,1,1] and meta[1]<=(6 if profile==2 else 4)

def grid_compiler_source(original,mesa):
    pipeline=(mesa/'src/imagination/vulkan/pvr_arch_pipeline.c').read_text()
    if digest(pipeline.encode())!='4e2742f0534d9de7783cb1f7180b22f5c9ada27802c23a253d3143d73171c4e8':
        raise ValueError('Pinned compute sysval allocator differs')
    def extract(name):
        start=pipeline.index('static void '+name+'(');end=pipeline.index('{',start)+1;depth=1
        while depth:
            depth+=(pipeline[end]=='{')-(pipeline[end]=='}');end+=1
        return pipeline[start:end]
    functions='\n'.join(extract(x) for x in ('allocate_val','check_unused_sysvals','pvr_alloc_cs_sysvals'))
    source=original.decode().replace('static pco_shader *build_shader(',functions+'\nstatic pco_shader *build_shader(')
    # Same postprocess/allocate/translate order as the Vulkan compiler path.
    allocation=r'''
   for (unsigned u=0; u<SYSTEM_VALUE_MAX; ++u) {
      if (BITSET_TEST(nir->info.system_values_read,u) &&
          u!=SYSTEM_VALUE_LOCAL_INVOCATION_INDEX && u!=SYSTEM_VALUE_WORKGROUP_ID && u!=SYSTEM_VALUE_NUM_WORKGROUPS)
         abort();
   }
   data->common.vtxins=4; /* Two explicit pointer arguments. */
   pvr_alloc_cs_sysvals(data,nir);
'''
    return source.replace('   pco_postprocess_nir(ctx, nir, data);','   pco_postprocess_nir(ctx, nir, data);'+allocation)

def compile_program(source,mesa,build,out,ninja,profile=1):
    """The supplied source requires a trusted reviewer; syntax is not a sandbox."""
    if profile not in (1,2,3):raise ValueError('Unsupported register/dispatch ABI')
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
    scan=r'''
   sunstone_vtxin_mask = 0;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic) continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_load_vtxin_pco) continue;
            if (!nir_src_is_const(intr->src[0])) { sunstone_vtxin_mask = 0xffffffffu; continue; }
            unsigned first = nir_src_as_uint(intr->src[0]);
            unsigned width = intr->def.num_components;
            if (first >= 32 || width > 32-first || width == 32) { sunstone_vtxin_mask = 0xffffffffu; continue; }
            sunstone_vtxin_mask |= ((1u << width)-1u) << first;
         }
      }
   }
'''
    base_source=grid_compiler_source(original,mesa) if profile==3 else original.decode()
    patched=base_source.replace('static pco_shader *build_shader(', 'static unsigned sunstone_vtxin_mask;\nstatic pco_shader *build_shader(')
    patched=patched.replace('   pco_postprocess_nir(ctx, nir, data);', '   pco_postprocess_nir(ctx, nir, data);'+scan)
    patched=patched.replace(anchor,anchor+export+'\n            fprintf(stderr,"SUNSTONE_VTXIN_MASK %u\\n",sunstone_vtxin_mask);\n')
    if profile==3:
        sysvals=r'''
            fprintf(stderr,"SUNSTONE_SYSVALS %u %u %u %u %u %u\n",
              m->common.sys_vals[SYSTEM_VALUE_LOCAL_INVOCATION_INDEX].start,m->common.sys_vals[SYSTEM_VALUE_LOCAL_INVOCATION_INDEX].count,
              m->common.sys_vals[SYSTEM_VALUE_WORKGROUP_ID].start,m->common.sys_vals[SYSTEM_VALUE_WORKGROUP_ID].count,
              m->common.sys_vals[SYSTEM_VALUE_NUM_WORKGROUPS].start,m->common.sys_vals[SYSTEM_VALUE_NUM_WORKGROUPS].count);
'''
        patched=patched.replace(export,export+sysvals)
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
    reference_compiler=None
    if profile==3:
        (out/'pco_clc_grid_reference.c').write_text(base_source)
        ref_cc=[str(out/'reference.o') if x==str(out/'metadata.o') else str(out/'pco_clc_grid_reference.c') if x==str(out/'pco_clc_metadata.c') else x for x in cc]
        commands.append(ref_cc);run(ref_cc,build,out,'reference-build')
        ref_link=[str(out/'reference.o') if x==str(out/'metadata.o') else str(out/'pco_clc_grid_reference') if x==str(out/'pco_clc_metadata') else x for x in link]
        commands.append(ref_link);run(ref_link,build,out,'reference-link');reference_compiler=out/'pco_clc_grid_reference'
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
    masks=re.findall(r'^SUNSTONE_VTXIN_MASK (\d+)$',err,re.M)
    if len(masks)!=1:raise ValueError('Missing explicit input-register use metadata')
    explicit_mask=int(masks[0])
    if profile==2 and explicit_mask!=63:raise ValueError('Profile2 requires exactly six explicit input registers')
    if not metadata_valid(meta,profile):
        raise ValueError('Unsupported compiler metadata: '+str(meta))
    arrays=re.findall(r'static const uint32_t \w+\[\d+\] = \{(.*?)\};',(out/'kernel.c').read_text(),re.S)
    if len(arrays)!=1:raise ValueError('Require exactly one common binary')
    blob=b''.join(struct.pack('<I',int(x,16)) for x in re.findall(r'0x[0-9a-fA-F]+',arrays[0]))
    if len(blob)<12 or len(blob)!=8+int.from_bytes(blob[5:7],'little')*4 or len(blob)-8>CODE-(32 if profile==3 else 0):raise ValueError('Invalid USC extent')
    if blob[:2]!=bytes(meta[:2]) or blob[2:5]!=bytes([meta[2],0,0]):raise ValueError('Metadata/header disagreement')
    upstream,command=upstream_binary(build,out,reference_compiler)
    commands.append(command)
    if upstream!=blob:raise ValueError('Instrumented compiler changed USC bytes')
    sysvals=[]
    if profile==3:
        fields=re.findall(r'^SUNSTONE_SYSVALS (.*)$',err,re.M)
        if len(fields)!=1:raise ValueError('Missing compiler sysval allocation')
        sysvals=list(map(int,fields[0].split()))
        if sysvals!=[4,1,0,3,4,3] or explicit_mask!=15:raise ValueError('Grid register ABI differs')
    (out/'program.bin').write_bytes(blob)
    (out/'source.cl').write_bytes(source.read_bytes())
    manifest={'schema':'sunstone-pco-profile-v'+str(profile),'profile':profile,'explicit_vtxin_mask':explicit_mask,'abi_vtxins':5 if profile==3 else 6 if profile==2 else 4,'sysval_ranges':sysvals,'mesa_revision':MESA,'source_sha256':digest(source.read_bytes()),
      'spirv_sha256':digest((out/'kernel.spv').read_bytes()),'program_sha256':digest(blob),'metadata':meta,
      'compiler_sha256':digest((out/'pco_clc_metadata').read_bytes()),
      'upstream_compiler_sha256':digest((build/'src/imagination/pco/uscgen/pco_clc').read_bytes()),
      'reference_compiler_sha256':digest((reference_compiler or build/'src/imagination/pco/uscgen/pco_clc').read_bytes()),
      'reference_adapter_source_sha256':digest(base_source.encode()) if profile==3 else None,
      'metadata_export_source_sha256':digest(patched.encode()),'commands':commands,
      'source_admission':'Required independently; metadata does not prove USC memory behavior.'}
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return manifest

def package(program,manifest,input_data,output_bytes,input_header_bytes,input_element_bytes,output_element_bytes):
    m=json.loads(Path(manifest).read_text());blob=Path(program).read_bytes();data=Path(input_data).read_bytes()
    profile=m.get('profile',1)
    if profile not in (1,2,3) or m['schema']!='sunstone-pco-profile-v'+str(profile) or m['mesa_revision']!=MESA or digest(blob)!=m['program_sha256']:raise ValueError('Program identity differs')
    if len(data)<4 or len(data)>INPUT or not 0<output_bytes<=3968 or output_bytes%4:raise ValueError('Invalid data bounds')
    meta=m['metadata']
    if not metadata_valid(meta,profile):raise ValueError('Unsupported profile')
    if blob[:5]!=bytes([meta[0],meta[1],meta[2],0,0]) or meta[1]>(5 if profile==3 else 6 if profile==2 else 4) or (meta[0]+3)//4>63:raise ValueError('Compiler header/resource metadata differs')
    count=struct.unpack_from('<I',data)[0]
    groups=(count+31)//32 if profile>=2 else 0
    if profile==2 and (count>992 or m.get('explicit_vtxin_mask')!=63 or m.get('abi_vtxins')!=6):raise ValueError('Unsupported dispatch extent or register ABI')
    geometry=b''
    if profile==3:
        if len(data)<12 or input_header_bytes<12 or m.get('sysval_ranges')!=[4,1,0,3,4,3] or m.get('explicit_vtxin_mask')!=15 or m.get('abi_vtxins')!=5:
            raise ValueError('Grid metadata/input header differs')
        width,height=struct.unpack_from('<2I',data,4);lx,ly,_=meta[10:13]
        gx,gy=(width+lx-1)//lx,(height+ly-1)//ly
        if not width or not height or width*height!=count or count>992 or gx*gy>31 or len(blob)-8>4000:raise ValueError('Grid extent exceeds supported bounds')
        geometry=struct.pack('<8I',gx,gy,1,width,height,1,1,0);groups=1
    header=struct.pack('<4IQ8I4I' ,0x50435650,profile,8288,profile,BVNC,len(blob)-8,len(data),output_bytes,struct.unpack_from('<I',data)[0],0,0,0,0,*meta[10:13],groups)
    header+=blob[:8]+bytes.fromhex(m['source_sha256'])+bytes.fromhex(m['compiler_sha256'])+struct.pack('<4I',input_header_bytes,input_element_bytes,output_element_bytes,32 if profile>=2 else 0)
    count=struct.unpack_from('<I',data)[0]
    if not count or input_header_bytes<4 or input_element_bytes<1 or output_element_bytes<1 or len(data)!=input_header_bytes+count*input_element_bytes or output_bytes!=count*output_element_bytes:raise ValueError('Caller data violates trusted element bounds')
    return header+geometry+blob[8:]+bytes(CODE-len(geometry)-(len(blob)-8))+data+bytes(INPUT-len(data))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='operation',required=True)
    c=sub.add_parser('compile')
    for x in ('source','mesa','build','output','ninja'):c.add_argument('--'+x,type=Path,required=True)
    c.add_argument('--profile',type=int,choices=(1,2,3),default=1)
    b=sub.add_parser('package')
    for x in ('program','manifest','input','output'):b.add_argument('--'+x,type=Path,required=True)
    for x in ('output-bytes','input-header-bytes','input-element-bytes','output-element-bytes'):b.add_argument('--'+x,type=int,required=True)
    a=p.parse_args()
    if a.operation=='compile':compile_program(a.source,a.mesa,a.build,a.output,a.ninja,a.profile)
    else:a.output.write_bytes(package(a.program,a.manifest,a.input,a.output_bytes,a.input_header_bytes,a.input_element_bytes,a.output_element_bytes))
