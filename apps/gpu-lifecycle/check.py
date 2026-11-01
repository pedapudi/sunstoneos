#!/usr/bin/env python3
"""Check lifecycle application calls, failure stops and complete payload decoding."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, required=True)
p.add_argument('--cc', default='/usr/bin/clang-21')
a = p.parse_args()
a.build.mkdir(parents=True, exist_ok=True)
generated = a.build / 'generated'
subprocess.run([sys.executable, str(ROOT/'tools/sunstone-manifest.py'), 'emit', '--manifest',
                str(ROOT/'apps/gpu-lifecycle/manifest.json'), '--output', str(generated)], check=True)
(generated/'FreeRTOS.h').write_text('#include <stdint.h>\ntypedef uint64_t TickType_t;\n')
(generated/'task.h').write_text('TickType_t xTaskGetTickCount(void);\nvoid vTaskDelay(TickType_t);\n')
fixture = r'''
#include "gpu_lifecycle-app.h"
#include "gpu_queue.h"
#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
static jmp_buf done;
static unsigned mode, fail_at, creates, submits, waits, closes, opens, recovers, releases, peers, terminal;
static uint32_t input[130], output[67];
static uint64_t ticks;
static unsigned attempts, delays;
const struct ss_manifest gpu_lifecycle_manifest={0}, gpu_queue_peer_manifest={0};
void gpu_queue_peer_main(const struct ss_grant *g) {(void)g;}
void gpu_queue_peer_release(void) {assert(submits==1 && !releases);++releases;}
int gpu_queue_peer_accepted(uint32_t ticks) {assert(ticks==50 && submits==1 && releases==1);return mode!=14;}
int gpu_queue_peer_wait(uint32_t ticks) {assert(ticks==4000 && releases==1);++peers;return mode!=11;}
int gpu_queue_context_create(const struct gpu_queue_context_config *c,uint32_t ticks,
 struct gpu_queue_context *ctx,struct gpu_queue_lease_info *info)
{
 ++creates;assert(ticks==4000 && c->job_us==200000 && c->lease_count==2);
 assert(c->program->abi==GPU_QUEUE_ABI_GRID && c->program->local_size[0]==32);
 assert(c->lease[0].region==GPU_LIFECYCLE_REGION_OUTPUT && c->lease[1].region==GPU_LIFECYCLE_REGION_INPUT);
 if(mode==2)return GPU_QUEUE_REFUSED;
 *ctx=(struct gpu_queue_context){.generation=19};
 info[0]=(struct gpu_queue_lease_info){.address=(uintptr_t)output};
 info[1]=(struct gpu_queue_lease_info){.address=(uintptr_t)input};
 if(mode==3)++info[0].address;
 return GPU_QUEUE_OK;
}
int gpu_queue_submit(struct gpu_queue_context ctx,const struct gpu_queue_job *j)
{
 ++attempts;if(submits==waits)++submits;assert(ctx.generation==19 && j->sequence==submits && j->bindings==2 && j->argument_bytes==4);
 assert(j->groups[0]==3 && j->groups[1]==1 && j->groups[2]==1 && !j->not_after_us);
 assert(j->binding[0].sync_bytes==260 && j->binding[1].sync_bytes==520);
 if(submits==fail_at){
  if(mode==4 || mode==16 || mode==17 || ((mode==15 || mode==18) && delays<3))return GPU_QUEUE_PARKED;
  if(mode==18)return GPU_QUEUE_REFUSED;
 }
 for(unsigned i=0;i<65;++i){unsigned at=(i+j->sequence-1)%65;
  assert(input[i]==UINT32_MAX-3u*at && input[65+i]==7u*at+9u);
  if(!(mode==12 && submits==fail_at))output[i]=input[i]+input[65+i];}
 if(mode==6 && submits==fail_at)++output[64];
 return mode==4 && submits==fail_at?GPU_QUEUE_PARKED:GPU_QUEUE_OK;
}
int gpu_queue_wait(struct gpu_queue_context ctx,struct gpu_queue_record *r,uint32_t ticks)
{
 ++waits;assert(ctx.generation==19 && ticks==4000 && waits==submits);
 *r=(struct gpu_queue_record){.sequence=waits,.kick_to_done_us=100+waits};
 if(mode==5 && waits==fail_at)return GPU_QUEUE_TIMEOUT;
 if(mode==13 && waits==fail_at)++r->sequence;
 if(GPU_LIFECYCLE_RECOVERY && waits==1){r->result=GPU_QUEUE_FAULT;r->detail=GPU_QUEUE_HANG;
  if(mode==9)r->detail=GPU_QUEUE_RESET;return GPU_QUEUE_FAULT;}
 r->result=GPU_QUEUE_OK;return GPU_QUEUE_OK;
}
int gpu_queue_close(struct gpu_queue_context ctx,uint32_t ticks)
{assert(ctx.generation==19 && ticks==4000);++closes;return mode==7 && closes==fail_at?GPU_QUEUE_FAULT:GPU_QUEUE_OK;}
int gpu_queue_open(struct gpu_queue_context ctx,uint32_t ticks)
{assert(ctx.generation==19 && ticks==4000 && closes==opens+1);++opens;return mode==8 && opens==fail_at?GPU_QUEUE_FAULT:GPU_QUEUE_OK;}
int gpu_queue_recover(struct gpu_queue_context ctx,uint32_t ticks)
{assert(ctx.generation==19 && ticks==4000 && waits==1 && !recovers);++recovers;return mode==10?GPU_QUEUE_FAULT:GPU_QUEUE_OK;}
uint64_t xTaskGetTickCount(void){return ticks;}
void vTaskDelay(uint64_t n){assert(n==1);ticks+=n;++delays;}
int ss_live(void){return mode==16 && delays==2?-1:1;}
void ss_progress(void) {}
_Noreturn void ss_fail(uint32_t detail){terminal=detail;longjmp(done,1);}
_Noreturn void ss_finish(const void *data,size_t bytes)
{
 const uint32_t *r=data;assert(bytes==88 && r[0]==0x314c5047 && r[6]==8840 && r[10]==1 && r[11]==65);
 if(GPU_LIFECYCLE_RECOVERY){assert(r[1]==2 && r[2]==2 && r[3]==1 && r[4]==1 && r[5]==2 && r[7]==GPU_QUEUE_FAULT && r[8]==GPU_QUEUE_HANG && r[9]==1);assert(r[12]==102);for(unsigned i=13;i<22;++i)assert(!r[i]);}
 else{assert(r[1]==1 && r[2]==10 && r[3]==10 && r[4]==10 && r[5]==10 && !r[7] && !r[8] && !r[9]);for(unsigned i=0;i<10;++i)assert(r[12+i]==101+i);}
 terminal=0;longjmp(done,1);
}
int main(int argc,char **argv)
{
 assert(argc==3);mode=strtoul(argv[1],0,10);fail_at=strtoul(argv[2],0,10);
 struct ss_grant g={.devices=SS_DEVICE_GPU,.region_count=2};
 g.regions[GPU_LIFECYCLE_REGION_INPUT]=(struct ss_span){input,sizeof(input)};
 g.regions[GPU_LIFECYCLE_REGION_OUTPUT]=(struct ss_span){output,260};
 output[65]=0x12345678;output[66]=0x87654321;if(mode==1)g.devices=0;
 if(mode==17)ticks=UINT64_MAX-100;
 if(!setjmp(done))gpu_lifecycle_main(&g);
 unsigned expected=mode==15?0:mode>=16?4:mode;
 if(mode==5)expected=GPU_LIFECYCLE_RECOVERY && fail_at==1?9:5;
 if(mode==12)expected=6;if(mode==13)expected=GPU_LIFECYCLE_RECOVERY && fail_at==1?9:5;
 if(mode==14)expected=12;
 assert(terminal==expected && creates==(mode!=1));
 if(!mode || mode==15){assert(submits==(GPU_LIFECYCLE_RECOVERY?2:10) && waits==submits);assert(closes==(GPU_LIFECYCLE_RECOVERY?1:10));assert(opens==(GPU_LIFECYCLE_RECOVERY?0:9));assert(recovers==GPU_LIFECYCLE_RECOVERY && releases==1 && peers==1);}
 if(mode==4 || mode>=16)assert(submits==fail_at && waits==submits-1);
 if(mode==5 || mode==6 || mode==12 || mode==13)assert(submits==fail_at && waits==fail_at);
 if(mode==7)assert(closes==fail_at);
 if(mode==8)assert(opens==fail_at && submits==fail_at);
 if(mode==9 || mode==10)assert(submits==1 && !closes);
 if(mode==11)assert(peers==1 && !closes);
 assert(attempts==submits+delays);
 assert(delays==(mode==4 || mode==17?500:mode==16?2:mode==15 || mode==18?3:0));
 assert(output[65]==0x12345678 && output[66]==0x87654321);return 0;
}
'''
(a.build/'fixture.c').write_text(fixture)
flags=['-std=c11', '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT/'include'),
       '-I'+str(generated), '-I'+str(ROOT/'apps/gpu-queue-peer'), '-DGPU_LIFECYCLE_PEER=1']
cases=[]
for recovery in (0, 1):
    definitions=['-DGPU_LIFECYCLE_RECOVERY='+str(recovery)]
    binary=a.build/('lifecycle-'+str(recovery))
    subprocess.run([a.cc,*flags,*definitions,'-fsanitize=address,undefined','-fno-sanitize-recover=all',
                    str(a.build/'fixture.c'),str(ROOT/'apps/gpu-lifecycle/main.c'),'-o',str(binary)],check=True)
    selected=[(mode,1) for mode in ((0,1,2,3,4,5,6,7,8,11,13,15,16,17,18) if not recovery else (0,1,2,3,4,5,7,9,10,11,13,14,15,16,17,18))]
    selected += ([(mode,10) for mode in (4,5,6,7,12,13,15,16,17,18)] + [(8,9)] if not recovery else [(mode,2) for mode in (4,5,6,13,15,16,17,18)])
    for mode,at in selected:
        subprocess.run([str(binary),str(mode),str(at)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
        cases.append({'recovery':bool(recovery),'injected_failure':mode,'at':at})
    subprocess.run([a.cc,*flags,*definitions,'--target=aarch64-none-elf','-ffreestanding','-mgeneral-regs-only',
                    '-c',str(ROOT/'apps/gpu-lifecycle/main.c'),'-o',str(a.build/('lifecycle-'+str(recovery)+'.o'))],check=True)
spec=importlib.util.spec_from_file_location('lifecycle_host',ROOT/'apps/gpu-lifecycle/host.py')
host=importlib.util.module_from_spec(spec);spec.loader.exec_module(host)
decoder_cases=0
for mode in (1,2):
    words=([0x314c5047,1,10,10,10,10,8840,0,0,0,1,65]+[100]*10 if mode==1 else
           [0x314c5047,2,2,1,1,2,8840,4,11,1,1,65]+[100]+[0]*9)
    raw=struct.pack('<22I',*words);host.decode(raw);decoder_cases+=1
    for index in range(12):
        changed=words.copy();changed[index]^=0x100
        try:host.decode(struct.pack('<22I',*changed))
        except ValueError:decoder_cases+=1
        else:raise AssertionError(('accepted changed field',mode,index))
    for index in range(12,22):
        changed=words.copy();changed[index]=200000 if mode==1 or index==12 else 1
        try:host.decode(struct.pack('<22I',*changed))
        except ValueError:decoder_cases+=1
        else:raise AssertionError(('accepted changed timing',mode,index))
    for changed in (raw[:-1],raw+b'\0'):
        try:host.decode(changed)
        except ValueError:decoder_cases+=1
        else:raise AssertionError('accepted changed payload length')
result=dict(physical_access=False,application_cases=cases,decoder_cases=decoder_cases,
            scope='Application control flow and payload format; queue callbacks model GPU completion.')
(a.build/'result.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(dict(application_cases=len(cases),decoder_cases=decoder_cases,physical_access=False)))
