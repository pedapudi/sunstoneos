#!/usr/bin/env python3
"""Build the fixed render programs using the pinned, prebuilt Mesa compiler."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
REVISION = '86c7176d2dd0065555521bf8c61f208bf53c75d6'

def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--mesa-source', type=Path, required=True)
    ap.add_argument('--mesa-archive', type=Path, required=True)
    ap.add_argument('--mesa-build', type=Path, required=True)
    ap.add_argument('--evidence', type=Path, required=True)
    args = ap.parse_args()
    src, build, out = (p.resolve() for p in (args.mesa_source,args.mesa_build,args.evidence))
    assert sha(args.mesa_archive) == 'ca5e3ac4b61b8dc659cc489c7eb3c7bcc750f6820fbf39f0b831ac87fb00da58'
    out.mkdir(parents=True,exist_ok=False)
    commands=[]
    def run(cmd,name):
        commands.append(cmd)
        p=subprocess.run(cmd,cwd=build,capture_output=True,text=True,env={**os.environ,'PCO_DEBUG':'no_pred_cf'})
        (out/(name+'.log')).write_text(p.stdout+p.stderr)
        (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        if p.returncode: raise RuntimeError(name+': '+p.stderr[-4000:])
    generated=build/'src/imagination/pco/uscgen/pco_uscgen_programs.c'
    metadata=src/'src/imagination/pco/pco_data.h'
    struct=re.search(r'typedef struct PACKED _pco_precomp_data \{.*?\} pco_precomp_data;',metadata.read_text(),re.S)[0]
    arrays=[]
    for name in ('vs_nop_common_0_common','cs_store_sr_1024_common_0_common','cs_load_sr_256_common_0_common'):
        arrays.append(re.search(r'static const uint32_t '+name+r'\[\d+\] = \{.*?\};',generated.read_text(),re.S)[0])
    # pco_data.h already defines pco_precomp_data in this compiler harness.
    (out/'reference-usc.inc').write_text('\n'.join(arrays)+'\n')
    pds=src/'src/imagination/vulkan/pds/pvr_pds.c'
    (out/'mesa-pds.c').write_text(pds.read_text().split('#include "pvr_draw_indirect_arrays0.h"')[0])
    database=json.loads((build/'compile_commands.json').read_text())
    entry=next(c for c in database if c['file'].endswith('pco_clc.c'))
    flags=shlex.split(entry['command']);flags=flags[:flags.index('-MD')]
    flags=[f for f in flags if f!='-DNDEBUG']
    flags+=['-I'+str(ROOT/'include'),'-I'+str(src/'src/imagination/vulkan/pds'),
        '-I'+str(src/'src/imagination/vulkan'),'-I'+str(out),'-DPVR_BUILD_ARCH_ROGUE','-ffunction-sections','-fdata-sections']
    for source,name in ((out/'mesa-pds.c','pds'),(ROOT/'checks/pvr-render-producer.c','producer')):
        run(flags+['-MD','-MF',str(out/(name+'.d')),'-c',str(source),'-o',str(out/(name+'.o'))],name)
    ninja=(build/'build.ninja').read_text();start=ninja.index('build src/imagination/pco/uscgen/pco_clc:')
    link=shlex.split(re.search(r'\n LINK_ARGS = (.*)',ninja[start:])[1])
    run(['c++',str(out/'producer.o'),str(out/'pds.o'),'-Wl,--gc-sections',*link,'-o',str(out/'producer')],'link')
    inputs={generated,metadata,pds,build/'compile_commands.json',build/'build.ninja',ROOT/'checks/pvr-render-producer.c',Path(shutil.which(flags[0])).resolve(),Path(shutil.which('c++')).resolve()}
    for dep in out.glob('*.d'):
        text=dep.read_text().replace('\\\n',' ')
        for name in shlex.split(text.split(':',1)[1]):
            p=Path(name);inputs.add((build/p).resolve() if not p.is_absolute() else p)
    for name in link:
        if name.endswith(('.a','.so')):
            p=Path(name);inputs.add((build/p).resolve() if not p.is_absolute() else p)
    # Verify the source files used by this compilation against the release
    # archive before executing the producer. Generated headers and libraries
    # are separately retained build inputs; their hashes are not release proof.
    with tarfile.open(args.mesa_archive, 'r:gz') as archive:
        for p in inputs:
            if p.is_relative_to(src):
                assert p.read_bytes() == archive.extractfile('mesa-'+REVISION+'/'+str(p.relative_to(src))).read(), p
    archived=[]
    for i,p in enumerate(sorted(inputs)):
        dest=out/'inputs'/str(i)/p.name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,dest)
        archived.append({'source':str(p),'snapshot':str(dest),'sha256':sha(p)})
    (out/'inputs.json').write_text(json.dumps(archived,indent=2)+'\n')
    run([str(out/'producer'),str(out/'payload.bin'),str(out/'image.bin')],'generate')
    manifest={'mesa_revision':REVISION,'scope':'Offline real compiler/PDS production; no GPU execution',
        'commands':commands,'inputs':archived,'artifacts':{p.name:sha(p) for p in out.iterdir() if p.is_file()}}
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps({k:v for k,v in manifest['artifacts'].items() if k.endswith('.bin')},indent=2))
if __name__=='__main__': main()
