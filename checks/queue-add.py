#!/usr/bin/env python3
"""Run the queue-add application against a strict host queue and injected failures."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, required=True)
p.add_argument('--cc', default='clang')
a = p.parse_args()
a.build.mkdir(parents=True, exist_ok=True)
generated = a.build / 'generated'
subprocess.run([sys.executable, str(ROOT/'tools/sunstone-manifest.py'), 'emit',
                '--manifest', str(ROOT/'apps/queue-add/manifest.json'), '--output', str(generated)], check=True)
fixture = r'''
#include "queue_add-app.h"
#include "gpu_queue.h"
#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
static jmp_buf done;
static unsigned mode, creates, submits, waits, closes, terminal, progress;
static uint32_t input[130], output[67];
int gpu_queue_context_create(const struct gpu_queue_context_config *c, uint32_t ticks,
                            struct gpu_queue_context *ctx, struct gpu_queue_lease_info *info)
{
    ++creates;
    assert(ticks && c->lease_count == 2 && c->job_us == 200000);
    assert(c->program->abi == GPU_QUEUE_ABI_GRID && c->program->coefficients == 3);
    assert(c->program->local_size[0] == 32 && c->program->code_bytes > 0);
    assert(c->lease[0].region == QUEUE_ADD_REGION_OUTPUT && c->lease[1].region == QUEUE_ADD_REGION_INPUT);
    assert(!c->lease[0].bytes && !c->lease[1].bytes);
    if(mode == 2) return GPU_QUEUE_REFUSED;
    *ctx = (struct gpu_queue_context){19};
    info[0] = (struct gpu_queue_lease_info){.address=(uintptr_t)output, .bytes=2097152};
    info[1] = (struct gpu_queue_lease_info){.address=(uintptr_t)input, .bytes=2097152};
    if(mode == 3) ++info[0].address;
    return GPU_QUEUE_OK;
}
int gpu_queue_submit(struct gpu_queue_context ctx, const struct gpu_queue_job *j)
{
    ++submits;
    assert(ctx.generation == 19 && j->sequence == 1 && j->bindings == 2 && j->argument_bytes == 4);
    assert(j->groups[0] == 3 && j->groups[1] == 1 && j->groups[2] == 1);
    assert(!j->reserved && !j->not_after_us);
    assert(j->binding[0].lease == 0 && j->binding[1].lease == 1);
    assert(j->binding[0].sync_bytes == 260 && j->binding[1].sync_bytes == 520);
    for(unsigned i=0;i<2;++i) assert(!j->binding[i].address && !j->binding[i].sync_offset && !j->binding[i].reserved);
    assert(j->arguments[0] == 65 && !j->arguments[1] && !j->arguments[2] && !j->arguments[3]);
    for(unsigned i=0;i<65;++i) {
        assert(input[i] == UINT32_MAX-3u*i && input[65+i] == 7u*i+9u);
        output[i] = input[i] + input[65+i];
    }
    if(mode == 6) ++output[64];
    return mode == 4 ? GPU_QUEUE_PARKED : GPU_QUEUE_OK;
}
int gpu_queue_wait(struct gpu_queue_context ctx, struct gpu_queue_record *r, uint32_t ticks)
{
    ++waits;assert(ctx.generation == 19 && ticks);
    *r=(struct gpu_queue_record){.sequence=mode == 8 ? 2 : 1, .result=mode == 9 ? GPU_QUEUE_FAULT : GPU_QUEUE_OK};
    return mode == 5 ? GPU_QUEUE_TIMEOUT : GPU_QUEUE_OK;
}
int gpu_queue_close(struct gpu_queue_context ctx, uint32_t ticks)
{ ++closes;assert(ctx.generation == 19 && ticks);return mode == 7 ? GPU_QUEUE_FAULT : GPU_QUEUE_OK; }
void ss_progress(void) { ++progress; }
_Noreturn void ss_fail(uint32_t detail) { terminal=detail;longjmp(done,1); }
_Noreturn void ss_finish(const void *data, size_t bytes)
{
    assert(bytes == 16);
    const uint32_t *r=data;
    assert(r[0] == 65 && r[1] == 8840 && r[2] == 1 && !r[3]);
    terminal=0;longjmp(done,1);
}
int main(int argc, char **argv)
{
    assert(argc == 2);mode=(unsigned)strtoul(argv[1],0,10);
    struct ss_grant g={.devices=SS_DEVICE_GPU,.region_count=2};
    g.regions[QUEUE_ADD_REGION_INPUT]=(struct ss_span){input,sizeof(input)};
    g.regions[QUEUE_ADD_REGION_OUTPUT]=(struct ss_span){output,260};
    output[65]=0x12345678;output[66]=0x87654321;
    if(mode == 1) g.devices=0;
    if(!setjmp(done)) queue_add_main(&g);
    unsigned expected=mode <= 7 ? mode : 5;
    assert(terminal == expected && creates == (mode != 1));
    assert(submits == (mode == 0 || mode >= 4));
    assert(waits == (mode == 0 || mode >= 5));
    assert(closes == (mode == 0 || mode == 7));
    assert(progress == submits);
    assert(output[65] == 0x12345678 && output[66] == 0x87654321);
    return 0;
}
'''
(a.build/'fixture.c').write_text(fixture)
flags=['-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'include'),'-I'+str(generated)]
command=[a.cc,*flags,'-fsanitize=address,undefined','-fno-sanitize-recover=all',
         str(a.build/'fixture.c'),str(ROOT/'apps/queue-add/main.c'),'-o',str(a.build/'queue-add')]
subprocess.run(command,check=True)
for mode in range(10):
    subprocess.run([str(a.build/'queue-add'),str(mode)],check=True,
                   env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
subprocess.run([a.cc,*flags,'--target=aarch64-none-elf','-ffreestanding','-mgeneral-regs-only',
                '-c',str(ROOT/'apps/queue-add/main.c'),'-o',str(a.build/'queue-add.o')],check=True)
result={'schema':'sunstone-queue-add-host-v1','physical_access':False,'cases':[
    'success-with-partial-workgroup-and-unsigned-wrap','missing-device','create-refused',
    'lease-address-mismatch','submit-parked','wait-timeout','wrong-output','close-fault',
    'wrong-completion-sequence','fault-completion'],
    'scope':'Application calls and error handling; queue callbacks model GPU completion, not device execution.'}
(a.build/'result.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result))
