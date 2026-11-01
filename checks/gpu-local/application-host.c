/* SPDX-License-Identifier: MIT */
#include "queue_gemm-app.h"
#include "gpu_queue.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static jmp_buf done;
static uint32_t input[1024],output[1024];
static unsigned submissions,waits,closes,mode,terminal;
static struct gpu_queue_record completion;
int gpu_queue_context_create(const struct gpu_queue_context_config *c,uint32_t ticks,
                            struct gpu_queue_context *ctx,struct gpu_queue_lease_info *info)
{
    assert(ticks==500&&c->job_us==200000&&c->lease_count==2);
#ifdef SUNSTONE_GPU_GEMM_CONTROL
    assert(c->program->abi==GPU_QUEUE_ABI_GRID&&c->program->coefficients==3&&!c->program->local_words&&!c->program->barriers);
#else
    assert(c->program->abi==GPU_QUEUE_ABI_LOCAL&&c->program->coefficients==3&&c->program->local_words==96&&c->program->barriers==1);
#endif
    assert(c->lease[0].region==QUEUE_GEMM_REGION_OUTPUT&&c->lease[1].region==QUEUE_GEMM_REGION_INPUT);
    *ctx=(struct gpu_queue_context){1,0};info[0].address=(uintptr_t)output;info[1].address=(uintptr_t)input;return GPU_QUEUE_OK;
}
int gpu_queue_submit(struct gpu_queue_context ctx,const struct gpu_queue_job *j)
{
    assert(ctx.generation==1&&j->sequence==submissions+1&&waits==submissions&&j->bindings==2&&j->argument_bytes==12);
    assert(j->groups[0]==3&&j->groups[1]==2&&j->groups[2]==1);
    assert(j->binding[0].lease==0&&j->binding[0].address==64&&j->binding[0].sync_offset==0&&j->binding[0].sync_bytes==700);
    assert(j->binding[1].lease==1&&!j->binding[1].address&&j->binding[1].sync_bytes==1824);
    const uint32_t dim[3]={13,11,19};assert(!memcmp(j->arguments,dim,12));
    ++submissions;
    if(!(mode==2&&submissions==4))for(unsigned row=0;row<13;++row)for(unsigned col=0;col<11;++col) {
        float value=0;for(unsigned k=0;k<19;++k) {
            float a,b;memcpy(&a,input+row*19+k,4);memcpy(&b,input+13*19+k*11+col,4);value+=a*b;
        }
        memcpy(output+16+row*11+col,&value,4);
    }
    if(mode==1)output[16]^=1;
    if(mode==3)output[0]=1;
    completion=(struct gpu_queue_record){.sequence=j->sequence,.result=GPU_QUEUE_OK,.kick_to_done_us=17};return GPU_QUEUE_OK;
}
int gpu_queue_wait(struct gpu_queue_context ctx,struct gpu_queue_record *r,uint32_t ticks)
{assert(ctx.generation==1&&ticks==500&&waits+1==submissions);++waits;*r=completion;return GPU_QUEUE_OK;}
int gpu_queue_close(struct gpu_queue_context ctx,uint32_t ticks)
{assert(ctx.generation==1&&ticks==500&&waits==4);++closes;return GPU_QUEUE_OK;}
uint64_t xTaskGetTickCount(void){return 0;}
void vTaskDelay(uint64_t ticks){(void)ticks;assert(0);}
int ss_live(void){return 1;}
void ss_progress(void){}
_Noreturn void ss_fail(uint32_t code){terminal=code;longjmp(done,1);}
_Noreturn void ss_finish(const void *data,size_t bytes)
{
    assert(bytes==48&&submissions==4&&waits==4&&closes==1);
    const uint32_t *r=data;assert(r[0]==4&&r[1]==13&&r[2]==11&&r[3]==19&&r[5]==4&&r[6]==
#ifdef SUNSTONE_GPU_GEMM_CONTROL
    0
#else
    384
#endif
    &&r[7]==32);
    FILE *f=fopen("result.bin","wb");assert(f&&fwrite(data,bytes,1,f)==1&&!fclose(f));
    terminal=0;longjmp(done,1);
}
void queue_gemm_main(const struct ss_grant *);
int main(int argc,char **argv)
{
    mode=argc>1?(unsigned)strtoul(argv[1],NULL,0):0;
    for(unsigned i=0;i<1024;++i){uint32_t bits=0xa5a50000u+i;memcpy(output+i,&bits,4);}
    struct ss_grant g={.devices=SS_DEVICE_GPU,.region_count=2};
    g.regions[QUEUE_GEMM_REGION_INPUT].base=input;g.regions[QUEUE_GEMM_REGION_INPUT].bytes=sizeof(input);
    g.regions[QUEUE_GEMM_REGION_OUTPUT].base=output;g.regions[QUEUE_GEMM_REGION_OUTPUT].bytes=sizeof(output);
    if(!setjmp(done))queue_gemm_main(&g);
    assert(terminal==(mode==3?8u:mode?6u:0u));
    assert(mode?!closes:closes==1);puts("PASS GEMM application bindings, verification and terminal contract");return 0;
}
