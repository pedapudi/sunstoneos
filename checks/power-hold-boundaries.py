#!/usr/bin/env python3
"""Exercise native minimum-hold targets after fractional, costly triggers."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

from harness import CACHE, CLANG, SANITIZE, environment

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
commands = []


def run(command):
    command = [str(x) for x in command]
    p = subprocess.run(command, text=True, capture_output=True, env=environment())
    commands.append(dict(command=command, exit=p.returncode, stdout=p.stdout, stderr=p.stderr))
    (out/'commands.json').write_text(json.dumps(commands, indent=2)+'\n')
    assert p.returncode == 0, p.stderr
    return p.stdout


cadence = CACHE/'sources/pcie/kernel/bsp/drivers/phy/sunxi-cadence-combophy.c'
run([sys.executable, root/'drivers/usb/host/cadence-source.py', '--source', cadence,
     '--output', out/'ss-cadence.inc'])

common = r'''
#include <assert.h>
#include <stdio.h>
static uint64_t physical_ns;
static int floor_now(void *v,uint64_t *out)
{(void)v;*out=physical_ns/1000;return 1;}
/* The write returns after 625 us plus a fractional part. The earlier
 * timestamp cannot be the origin of the subsequent minimum hold. */
static void trigger(void){physical_ns+=625023;}
'''
usb0 = r'''
int main(void)
{
    const uint64_t delays[]={10,40,1000,150000};
    for(unsigned i=0;i<4;++i){
        physical_ns=100023;
        struct a733_usb_platform p={.ops={.now_us=floor_now},.last_now=100};
        trigger();uint64_t origin=physical_ns/1000,delay=delays[i];
        p.config.deadline_us=origin+delay+100;
        assert(wait_start(&p,delay)&&p.last_now==origin&&p.wait_until==origin+delay+1);
        assert((p.wait_until-1)*1000-physical_ns<delay*1000);
        assert(p.wait_until*1000-physical_ns>=delay*1000);
        p.config.deadline_us=origin+delay+1;
        assert(!wait_start(&p,delay)); /* Guard cannot extend the hard deadline. */
    }
    puts("PASS USB0 post-trigger floor guard:10/40/1000/150000us and unchanged deadline");
}
'''
usb3 = r'''
int main(void)
{
    const uint64_t delays[]={1000,5000,10000};
    for(unsigned i=0;i<3;++i){
        physical_ns=100023;
        struct a733_usb2_platform p={.ops={.now=floor_now},.last_now=100};
        trigger();uint64_t origin=physical_ns/1000,delay=delays[i];
        p.config.deadline=origin+delay+100;
        assert(wait_after(&p,delay)&&p.last_now==origin&&p.ss_wait==origin+delay+1);
        assert((p.ss_wait-1)*1000-physical_ns<delay*1000);
        assert(p.ss_wait*1000-physical_ns>=delay*1000);
        p.config.deadline=origin+delay+1;
        assert(!wait_after(&p,delay));
    }
    puts("PASS USB3 post-trigger floor guard:1/5/10ms and unchanged deadline");
}
'''
mmc = r'''
int main(void)
{
    const uint64_t delays[]={1000,2000,10000};
    for(unsigned i=0;i<3;++i){
        physical_ns=100023;
        struct sunxi_mmc h={.config={.ops={.now_us=floor_now}},.last_us=100,.deadline_us=1000000};
        trigger();uint64_t origin=physical_ns/1000,delay=delays[i];
        defer_hold(&h,delay);
        assert(h.state!=SUNXI_MMC_FAULT&&h.last_us==origin&&h.retry_us==origin+delay+1);
        assert((h.retry_us-1)*1000-physical_ns<delay*1000);
        assert(h.retry_us*1000-physical_ns>=delay*1000);
        defer_command(&h,100);assert(h.retry_us==origin+100); /* Poll cadence stays unchanged. */
        defer_hold(&h,UINT64_MAX);assert(h.state==SUNXI_MMC_FAULT&&h.result.error==SUNXI_MMC_TIME);
    }
    puts("PASS MMC post-trigger holds:1/2/10ms, unchanged100us polling and overflow refusal");
}
'''
records = []
dependencies = {Path(__file__).resolve(), root/'checks/harness.py',
                root/'drivers/usb/host/cadence-source.py', cadence}
for name, source, body in [('usb0', 'drivers/usb/a733_usb_platform.c', usb0),
                           ('usb3', 'drivers/usb/a733_usb3_phy.c', usb3),
                           ('mmc', 'drivers/storage/sunxi_mmc.c', mmc)]:
    c = out/(name+'.c')
    c.write_text('#include "'+str(root/source)+'"\n'+common+body)
    executable = out/name
    depfile = out/(name+'.d')
    run([CLANG, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-Wno-unused-function', '-ffunction-sections', '-fdata-sections',
         '-Wl,--gc-sections', '-MMD', '-MF', depfile, *SANITIZE, '-I'+str(root/'include'), '-I'+str(out),
         c, '-o', executable])
    dependencies.update(Path(p).resolve() for p in depfile.read_text().replace('\\\n', ' ').split()[1:])
    print(run([executable]), end='')
    records.append(dict(case=name, source=source,
                        source_sha256=hashlib.sha256((root/source).read_bytes()).hexdigest(),
                        executable_sha256=hashlib.sha256(executable.read_bytes()).hexdigest()))
inputs = []
for path in sorted(dependencies):
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    target = out/'inputs'/digest
    target.parent.mkdir(exist_ok=True)
    if not target.exists():
        target.write_bytes(data)
    inputs.append(dict(path=str(path), sha256=digest, snapshot=str(target.relative_to(out))))
(out/'inputs.json').write_text(json.dumps(inputs, indent=2)+'\n')
(out/'tools.json').write_text(json.dumps(dict(compiler=str(CLANG),
    sha256=hashlib.sha256(CLANG.read_bytes()).hexdigest(),
    version=subprocess.check_output([str(CLANG), '--version'], text=True)), indent=2)+'\n')
(out/'results.json').write_text(json.dumps(dict(status='PASS', cases=records,
    limitations=['private helper targets execute with modeled trigger/time callbacks',
                 'PCIe and MMC reset phase sequencing use their separate composed checks',
                 'no physical timing measurement']), indent=2)+'\n')
