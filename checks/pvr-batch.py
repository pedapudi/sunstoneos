#!/usr/bin/env python3
"""Compare native profile-4 batch encoders with pinned Mesa and firmware sources."""
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
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for name in ('mesa-source','mesa-archive','mesa-build','producer','firmware-headers','output'):
        ap.add_argument('--'+name,type=Path,required=True)
    args=ap.parse_args();src=args.mesa_source.resolve();build=args.mesa_build.resolve();out=args.output.resolve()
    assert sha(args.mesa_archive)=='ca5e3ac4b61b8dc659cc489c7eb3c7bcc750f6820fbf39f0b831ac87fb00da58'
    payload=args.producer.resolve()/'payload.bin'
    out.mkdir(parents=True,exist_ok=False);(out/'fw').mkdir()
    # Pinned Linux firmware ABI headers without their kernel-only includes.
    names=('pvr_rogue_cr_defs.h','pvr_rogue_cr_defs_client.h','pvr_rogue_defs.h','pvr_rogue_fwif.h','pvr_rogue_fwif_check.h',
        'pvr_rogue_fwif_client.h','pvr_rogue_fwif_client_check.h','pvr_rogue_fwif_common.h','pvr_rogue_fwif_shared.h','pvr_rogue_fwif_shared_check.h')
    for name in names:
        text=(args.firmware_headers/name).read_text()
        (out/'fw'/name).write_text(''.join(l for l in text.splitlines(True) if not l.startswith('#include <linux/')))
    (out/'fw/reference.h').write_text('''#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;typedef int32_t s32;
typedef uint64_t aligned_u64 __attribute__((aligned(8)));
#define __aligned(n) __attribute__((aligned(n)))
#define BIT(n) (1u<<(n))
#define BIT_ULL(n) (1ull<<(n))
#define BIT_MASK BIT
#define ALIGN(n,a) (((n)+(a)-1)&~((a)-1))
#define GENMASK(h,l) ((~0u<<(l))&(~0u>>(31-(h))))
#include "pvr_rogue_fwif.h"
#include "pvr_rogue_fwif_client.h"
''')
    sampler=src/'src/imagination/vulkan/pvr_arch_sampler.c';helpers=src/'src/imagination/vulkan/pvr_csb_enum_helpers.h'
    def extract(path,prefix,name):
        text=path.read_text()
        start=re.search(prefix+r'\s+'+name+r'\(',text).start();end=text.index('{',start)+1;depth=1
        while depth:
            depth+=(text[end]=='{')-(text[end]=='}');end+=1
        return text[start:end]
    (out/'batch-mesa.inc').write_text('\n'.join([extract(helpers,r'static inline enum ROGUE_TA_CMPMODE','pvr_ta_cmpmode'),
        extract(helpers,r'static inline enum ROGUE_TA_ISPB_STENCILOP','pvr_ta_stencilop')]+
        [extract(sampler,r'static uint32_t',n) for n in ('pvr_sampler_get_hw_filter_from_vk','pvr_sampler_get_hw_addr_mode_from_vk')])+'\n')
    entry=next(c for c in json.loads((build/'compile_commands.json').read_text()) if c['file'].endswith('pco_clc.c'))
    flags=shlex.split(entry['command']);flags=flags[:flags.index('-MD')];flags=[f for f in flags if f!='-DNDEBUG']
    flags+=['-I'+str(build/'src/vulkan/util'),'-I'+str(src/'src/vulkan/runtime'),'-I'+str(build/'src/vulkan/runtime'),
        '-I'+str(ROOT/'include'),'-I'+str(src/'src/imagination/vulkan'),'-I'+str(out),'-I'+str(out/'fw'),
        '-DPVR_BUILD_ARCH_ROGUE','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer']
    sources=[ROOT/'checks/pvr-batch.c',ROOT/'checks/pvr-batch-fw.c',ROOT/'drivers/gpu/pvr_render_texture.c',
        ROOT/'drivers/gpu/pvr_render_programs.c',ROOT/'drivers/gpu/pvr_render_job.c']
    commands=[];result={}
    def run(cmd,name,cwd):
        commands.append(cmd)
        r=subprocess.run(cmd,cwd=cwd,capture_output=True,text=True)
        (out/(name+'.log')).write_text(r.stdout+r.stderr)
        (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        if r.returncode:raise RuntimeError(name+': '+(r.stdout+r.stderr)[-6000:])
        return r.stdout
    try:
        run(flags+['-MD','-MF',str(out/'check.d')]+[str(s) for s in sources]+['-o',str(out/'check')],'compile',build)
        log=run([str(out/'check'),str(payload)],'run',out)
        assert 'PASS batch reference' in log,log
        inputs=set(sources)|{ROOT/'checks/pvr-batch.py',payload,sampler,helpers}
        for name in shlex.split((out/'check.d').read_text().replace('\\\n',' ').split(':',1)[1]):
            path=Path(name);inputs.add((build/path).resolve() if not path.is_absolute() else path)
        with tarfile.open(args.mesa_archive,'r:gz') as archive:
            for path in inputs:
                if path.is_relative_to(src):
                    assert path.read_bytes()==archive.extractfile('mesa-'+REVISION+'/'+str(path.relative_to(src))).read(),path
        for name in names:
            assert (args.firmware_headers/name).is_file()
            inputs.add((args.firmware_headers/name).resolve())
        (out/'inputs.json').write_text(json.dumps([{'path':str(p),'sha256':sha(p)} for p in sorted(inputs)],indent=2)+'\n')
        result={'status':'pass','result':log.strip(),'payload_sha256':sha(payload),'physical_access':False}
    finally:
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
if __name__=='__main__':main()
