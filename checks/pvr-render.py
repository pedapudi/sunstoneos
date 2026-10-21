#!/usr/bin/env python3
"""Compare fixed graphics records against source ABI and retained compiler output."""
import argparse
import hashlib
import json
import re
import shutil
import shlex
import subprocess
import tarfile
import xml.etree.ElementTree as ET
from pathlib import Path
if not __debug__:raise SystemExit('checks require assertions; unset PYTHONOPTIMIZE and do not pass -O')
ROOT=Path(__file__).resolve().parents[1]
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for n in ('sources','mesa','mesa-archive','producer','output'):ap.add_argument('--'+n,type=Path,required=True)
    ap.add_argument('--compiler',default='clang-21');a=ap.parse_args();o=a.output.resolve();o.mkdir(parents=True,exist_ok=False)
    headers=['pvr_rogue_cr_defs.h','pvr_rogue_cr_defs_client.h','pvr_rogue_defs.h','pvr_rogue_fwif.h','pvr_rogue_fwif_client.h','pvr_rogue_fwif_common.h','pvr_rogue_fwif_shared.h']
    for name in headers:
        for included in re.findall(r'^#include "([^"]+)"', (a.sources/name).read_text(), re.M):
            if included not in headers:headers.append(included)
    source_names=headers+['pvr_queue.c','pvr_cccb.c','pvr_vm.c','pvr_rogue_heap_config.h','pvr_rogue_fwif_client_check.h','pvr_rogue_fwif_check.h']
    pins=json.loads((a.sources.parent/'manifest.json').read_text());inputs=set()
    for name in source_names:
        p=a.sources/name;pin=next(x for x in pins if x['path']=='imagination/'+name);assert sha(p)==pin['sha256'];inputs.add(p)
        if name in headers:(o/name).write_text(re.sub(r'^#include <linux/[^>]+>\n','',p.read_text(),flags=re.M))
    assert sha(a.mesa_archive)=='ca5e3ac4b61b8dc659cc489c7eb3c7bcc750f6820fbf39f0b831ac87fb00da58'
    xml_path=a.mesa/'src/imagination/csbgen/rogue/cr.xml'
    mesa_inputs=[xml_path]+[a.mesa/'src/imagination/vulkan'/n for n in ('pvr_arch_job_context.c','pvr_arch_job_render.c','pvr_arch_cmd_buffer.c','pvr_usc.c','winsys/pvr_winsys_helper.c')]
    with tarfile.open(a.mesa_archive,'r:gz') as t:
        for p in mesa_inputs:assert p.read_bytes()==t.extractfile('mesa-86c7176d2dd0065555521bf8c61f208bf53c75d6/'+str(p.relative_to(a.mesa))).read()
    inputs.update(mesa_inputs);xml=ET.fromstring(xml_path.read_text())
    def bits(struct,names):
        return sum(1<<int(xml.find(".//struct[@name='"+struct+"']/field[@name='"+n+"']").attrib['start']) for n in names)
    constants={'PPP':bits('PPP_CTRL',['fixed_point_format','wclampen']),
        'PSG':bits('TE_PSG',['completeonterminate']), 'BG':bits('ISP_BGOBJVALS',['enablebgtag','mask']),
        'ISP':bits('ISP_CTL',['skip_init_hdrs','process_empty_tiles','sample_pos'])}
    reference='''#include <assert.h>
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
'''
    reference+='\n'.join('#define REF_'+n+' '+str(v)+'u' for n,v in constants.items())+'\n';(o/'reference.h').write_text(reference)
    for n,digest in [('payload.bin','11a968c9e5cb7ddc2a7731b8b924eb27cc2363aa1f79c5a489ccf4effb1b4fa6'),('image.bin','fc61bb04be2440683c4c1a6569b019a4044cb87e40577734c84f9652cc08f4ff')]:
        p=a.producer/n;assert sha(p)==digest;inputs.add(p)
    native=[ROOT/'drivers/gpu'/('pvr_render_'+n+'.c') for n in ('context','job','programs')]
    compiler=Path(shutil.which(a.compiler)).resolve()
    inputs.update(native+[ROOT/'checks/pvr-render.c',ROOT/'checks/pvr-render.py',o/'reference.h',compiler])
    inputs.update(ROOT/'include'/('pvr_render_'+n+'.h') for n in ('context','job','programs'))
    command=[str(compiler),'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-MD','-I'+str(o),'-I'+str(ROOT/'include'),str(ROOT/'checks/pvr-render.c'),*map(str,native),'-o',str(o/'check')]
    snapshots={}
    for i,p in enumerate(sorted(inputs)):
        d=o/'inputs'/str(i)/p.name;d.parent.mkdir(parents=True);shutil.copy2(p,d);snapshots[str(p)]={'snapshot':str(d),'sha256':sha(p)}
    (o/'inputs.json').write_text(json.dumps(snapshots,indent=2)+'\n');(o/'command.json').write_text(json.dumps(command,indent=2)+'\n')
    r=subprocess.run(command,capture_output=True,text=True,cwd=o);(o/'compile.log').write_text(r.stdout+r.stderr);r.check_returncode()
    dependency_command=[str(compiler),'-M','-I'+str(o),'-I'+str(ROOT/'include'),str(ROOT/'checks/pvr-render.c'),*map(str,native)]
    (o/'dependency-command.json').write_text(json.dumps(dependency_command,indent=2)+'\n')
    dependency=subprocess.run(dependency_command,capture_output=True,text=True,cwd=o);dependency.check_returncode()
    (o/'dependencies.txt').write_text(dependency.stdout)
    for rule in re.split(r'^[^:\n]+:',dependency.stdout,flags=re.M)[1:]:
        for name in shlex.split(rule.replace('\\\n',' ')):
            p=Path(name);inputs.add(p.resolve() if p.is_absolute() else (o/p).resolve())
    inputs.update(o/name for name in headers)
    for p in sorted(inputs):
        if str(p) in snapshots:continue
        d=o/'inputs'/str(len(snapshots))/p.name;d.parent.mkdir(parents=True);shutil.copy2(p,d)
        snapshots[str(p)]={'snapshot':str(d),'sha256':sha(p)}
    (o/'inputs.json').write_text(json.dumps(snapshots,indent=2)+'\n')
    r=subprocess.run([str(o/'check'),str(a.producer/'payload.bin'),str(a.producer/'image.bin')],capture_output=True,text=True,timeout=10)
    (o/'check.log').write_text(r.stdout+r.stderr)
    (o/'result.json').write_text(json.dumps({'returncode':r.returncode,'executable_sha256':sha(o/'check'),'physical_gpu':False},indent=2)+'\n')
    print(r.stdout+r.stderr);r.check_returncode()
if __name__=='__main__':main()
