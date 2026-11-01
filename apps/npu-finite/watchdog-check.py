#!/usr/bin/env python3
"""Exercise the finite image's actual watchdog policy with modeled MMIO."""
import argparse, subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--evidence', type=Path, required=True)
a=p.parse_args(); a.evidence.mkdir(parents=True, exist_ok=True)
root=Path(__file__).resolve().parents[2]
harness=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "watchdog.h"
static uint32_t mode, configuration, output, restarted, writes, ignore;
uint32_t npu_finite_watchdog_read(uintptr_t address) {
 if(address==0x02050014)return mode;
 if(address==0x02050010)return configuration;
 assert(address==0x02050018);return output;
}
void npu_finite_watchdog_write(uintptr_t address,uint32_t value) {
 ++writes;
 if(address==0x0205000c){assert(value==0x14af);++restarted;return;}
 assert(address==0x02050014 && (value>>16)==0x16aa);
 if(!ignore)mode=value&0xffff;
}
int main(void) {
 mode=0xb1;configuration=1;output=2;
 assert(npu_finite_watchdog_admitted());
 for(unsigned i=0;i<3;++i){uint32_t *v=i==0?&mode:i==1?&configuration:&output,old=*v;*v=0;assert(!npu_finite_watchdog_admitted());*v=old;}
 assert(npu_finite_watchdog_refresh(1,100,0));assert(restarted==1);
 assert(!npu_finite_watchdog_refresh(100,100,0));
 assert(!npu_finite_watchdog_refresh(1,100,1));assert(restarted==1);
 mode=0x41;assert(!npu_finite_watchdog_refresh(1,100,0));mode=0xb1;
 ignore=1;assert(npu_finite_watchdog_disarm()==0xb1);
 ignore=0;assert(npu_finite_watchdog_disarm()==0);
 unsigned before=writes;npu_finite_watchdog_stop();assert(writes==before);
 mode=0xb1;npu_finite_watchdog_stop();assert(mode==1 && restarted==2);
 puts("PASS finite watchdog admission, refresh gates, disarm readback and stop");
}
'''
source=a.evidence/'watchdog-harness.c'; source.write_text(harness)
command=['/usr/lib/llvm-21/bin/clang','-std=c11','-Wall','-Wextra','-Werror','-DSUNSTONE_NPU_WATCHDOG_TEST=1','-I'+str(root/'apps/npu-finite'),str(source),str(root/'apps/npu-finite/watchdog.c'),'-o',str(a.evidence/'watchdog-harness')]
subprocess.run(command,check=True);subprocess.run([str(a.evidence/'watchdog-harness')],check=True)
