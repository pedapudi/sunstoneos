#!/usr/bin/env python3
"""Compile the bounded native indexed texture pipeline with pinned Mesa sources."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tarfile

ROOT=Path(__file__).resolve().parents[1]
REVISION='86c7176d2dd0065555521bf8c61f208bf53c75d6'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for name in ('mesa-source','mesa-archive','mesa-build','evidence'):
        ap.add_argument('--'+name,type=Path,required=True)
    args=ap.parse_args();src=args.mesa_source.resolve();build=args.mesa_build.resolve();out=args.evidence.resolve()
    assert sha(args.mesa_archive)=='ca5e3ac4b61b8dc659cc489c7eb3c7bcc750f6820fbf39f0b831ac87fb00da58'
    out.mkdir(parents=True,exist_ok=False)
    pipeline=src/'src/imagination/vulkan/pvr_arch_pipeline.c'
    assert sha(pipeline)=='4e2742f0534d9de7783cb1f7180b22f5c9ada27802c23a253d3143d73171c4e8'
    text=pipeline.read_text()
    util=src/'src/vulkan/util/vk_util.h'
    stage=re.search(r'static inline VkShaderStageFlagBits\s+mesa_to_vk_shader_stage\([^}]+}',util.read_text())[0]
    (out/'texture-stage.inc').write_text(stage+'\n')
    (out/'texture-base.inc').write_text((ROOT/'checks/pvr-draw-producer.c').read_text().split('int main(int argc,char **argv)')[0])
    def extract(name):
        start=re.search(r'static void\s+'+name+r'\(',text).start();end=text.index('{',start)+1;depth=1
        while depth:
            depth+=(text[end]=='{')-(text[end]=='}');end+=1
        return text[start:end]
    functions=('allocate_var','try_allocate_var','allocate_val','try_allocate_vars','check_unused_sysvals',
        'pvr_alloc_vs_sysvals','pvr_alloc_vs_attribs','pvr_alloc_vs_varyings','pvr_alloc_fs_sysvals','pvr_alloc_fs_varyings')
    (out/'texture-descriptors.inc').write_text('\n'.join(extract(name) for name in ('pvr_init_descriptors','pvr_setup_descriptors'))+'\n')
    (out/'draw-allocators.inc').write_text('\n'.join(extract(name) for name in functions)+'\n')
    cmd_source=src/'src/imagination/vulkan/pvr_arch_cmd_buffer.c'
    command_text=cmd_source.read_text()
    start=command_text.index('void pvr_arch_calculate_vertex_cam_size(')
    end=command_text.index('{',start)+1;depth=1
    while depth:
        depth+=(command_text[end]=='{')-(command_text[end]=='}');end+=1
    # Keep the actual feature-dependent CAM allocator in the host producer.
    (out/'draw-cam.inc').write_text(command_text[start:end]+'\n')
    generated=build/'src/imagination/pco/uscgen/pco_uscgen_programs.c'
    arrays=[]
    for name in ('vs_nop_common_0_common','cs_store_sr_1024_common_0_common','cs_load_sr_256_common_0_common'):
        arrays.append(re.search(r'static const uint32_t '+name+r'\[\d+\] = \{.*?\};',generated.read_text(),re.S)[0])
    (out/'reference-usc.inc').write_text('\n'.join(arrays)+'\n')
    pds=src/'src/imagination/vulkan/pds/pvr_pds.c'
    (out/'mesa-pds.c').write_text(pds.read_text().split('#include "pvr_draw_indirect_arrays0.h"')[0])
    entry=next(c for c in json.loads((build/'compile_commands.json').read_text()) if c['file'].endswith('pco_clc.c'))
    flags=shlex.split(entry['command']);flags=flags[:flags.index('-MD')];flags=[f for f in flags if f!='-DNDEBUG']
    flags+=['-I'+str(build/'src/vulkan/util'),'-I'+str(src/'src/vulkan/runtime'),'-I'+str(build/'src/vulkan/runtime'),'-I'+str(ROOT/'checks'),'-I'+str(ROOT/'include'),'-I'+str(src/'src/imagination/vulkan/pds'),
        '-I'+str(src/'src/imagination/vulkan'),'-I'+str(out),'-DPVR_BUILD_ARCH_ROGUE','-ffunction-sections','-fdata-sections']
    commands=[]
    def run(cmd,name):
        commands.append(cmd)
        result=subprocess.run(cmd,cwd=build,capture_output=True,text=True,env={**os.environ,'PCO_DEBUG':'no_pred_cf'})
        (out/(name+'.log')).write_text(result.stdout+result.stderr)
        (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        if result.returncode:raise RuntimeError(name+': '+result.stderr[-5000:])
    objects=[]
    for source,name in ((out/'mesa-pds.c','pds'),(src/'src/imagination/vulkan/pds/pvr_pipeline_pds.c','pipeline-pds'),
                        (ROOT/'checks/pvr-texture-producer.c','producer')):
        obj=out/(name+'.o');objects.append(str(obj))
        run(flags+['-MD','-MF',str(out/(name+'.d')),'-c',str(source),'-o',str(obj)],name)
    ninja=(build/'build.ninja').read_text();start=ninja.index('build src/imagination/pco/uscgen/pco_clc:')
    link=shlex.split(re.search(r'\n LINK_ARGS = (.*)',ninja[start:])[1])
    run(['c++',*objects,'-Wl,--gc-sections',*link,'-o',str(out/'producer')],'link')
    inputs={util,pipeline,cmd_source,generated,ROOT/'checks/pvr-texture-producer.c',ROOT/'checks/pvr-draw-producer.c',ROOT/'checks/pvr-texture-producer.py',build/'build.ninja',build/'compile_commands.json'}
    for dep in out.glob('*.d'):
        for name in shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1]):
            path=Path(name);inputs.add((build/path).resolve() if not path.is_absolute() else path)
    for name in link:
        if name.endswith(('.a','.so')):
            path=Path(name);inputs.add((build/path).resolve() if not path.is_absolute() else path)
    with tarfile.open(args.mesa_archive,'r:gz') as archive:
        for path in inputs:
            if path.is_relative_to(src):
                assert path.read_bytes()==archive.extractfile('mesa-'+REVISION+'/'+str(path.relative_to(src))).read(),path
    (out/'inputs.json').write_text(json.dumps([{'path':str(p),'sha256':sha(p)} for p in sorted(inputs)],indent=2)+'\n')
    run([str(out/'producer'),str(out/'vertex.bin'),str(out/'fragment.bin'),str(out/'payload.bin')],'generate')
    print((out/'generate.log').read_text(),end='')
if __name__=='__main__':main()
