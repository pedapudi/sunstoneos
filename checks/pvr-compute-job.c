/*
 * Copyright © 2022, 2025 Imagination Technologies Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "pvr_compute_job.h"
#include "pvr_compute_context.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef UPSTREAM
#include "reference.h"
#endif
static struct pvr_compute_program_config programs;
static struct pvr_compute_program_result program_result;
static struct pvr_compute_job_config config;
static struct pvr_compute_job_image built;
static struct pvr_compute_buffer context_buffers[5];
static unsigned char context_memory[5][1024], program_memory[10][16448], job_memory[8][262146];
static unsigned char tables[3*4096];
static uint64_t pages[3]={0x400000,0x401000,0x402000};
static int admission=1;
static uint32_t get(const void *p,size_t off)
{const unsigned char *b=p;return (uint32_t)b[off]|(uint32_t)b[off+1]<<8|(uint32_t)b[off+2]<<16|(uint32_t)b[off+3]<<24;}
static void put(void *p,size_t off,uint32_t v)
{unsigned char *b=p;for(unsigned n=0;n<4;++n)b[off+n]=(unsigned char)(v>>(8*n));}
static int pa(void *ctx,const struct pvr_compute_program_config *p)
{(void)ctx;assert(p==&programs);return 1;}
static int ja(void *ctx,const struct pvr_compute_job_config *c)
{(void)ctx;assert(c->programs==&programs);return admission;}
static void setup(unsigned variant)
{
    uint64_t base=variant?UINT64_C(0xfc00000000):UINT64_C(0x400000000);
    programs=(struct pvr_compute_program_config){.bvnc=PVR_FW_A733_BVNC,
      .pds={base,UINT64_C(1)<<32},.usc={base+(UINT64_C(1)<<32),UINT64_C(1)<<32},
      .general={base+(UINT64_C(2)<<32),UINT64_C(1)<<32},.excluded_count=5,
      .execution_cores=1,.admission_token=17,.admit=pa};
    static const size_t ctx_sizes[5]={232,4,32,1024,32};
    static const uint32_t ctx_fw[5]={0xc0800000,0xc0800100,0xc0800200,0xc0810000,0xc0800300};
    for(unsigned n=0;n<5;++n) {
        memset(context_memory[n],0,sizeof(context_memory[n]));
        programs.excluded[n]=(struct pvr_compute_program_span){context_memory[n],ctx_sizes[n],programs.general.base+0x100000+n*0x10000};
        context_buffers[n]=(struct pvr_compute_buffer){context_memory[n],ctx_sizes[n],ctx_fw[n]};
    }
    for(unsigned n=0;n<10;++n) {
        uint64_t addr=n<2?programs.usc.base+0x1000+n*1024:n<7?programs.pds.base+0x1000+(n-2)*256:
            n<9?programs.pds.base+0x200000+(n-7)*0x10000:programs.general.base+0x10000;
        programs.buffers[n]=(struct pvr_compute_program_span){program_memory[n],pvr_compute_program_bytes[n],addr};
    }
    assert(pvr_compute_programs_build(&programs,&program_result));
    struct pvr_gpu_mapping mapping={programs.general.base+0x10000,0x800000,4096,12,PVR_GPU_VM_READ_ONLY};
    struct pvr_gpu_vm_tables pt={tables,sizeof(tables),pages,3};
    assert(pvr_gpu_vm_build(&mapping,1,&pt,context_memory[4],32));
    struct pvr_compute_config context={.bvnc=PVR_FW_A733_BVNC,.priority=1,.context_id=7,.owner_id=11};
    memcpy(context.switch_registers,program_result.switch_registers,sizeof(context.switch_registers));
    assert(pvr_compute_context_build(&context,context_buffers));
    config=(struct pvr_compute_job_config){.programs=&programs,.program_result=&program_result,
      .firmware_context=ctx_fw[0],.firmware_ufo=0xc0800400,.seed=variant?0xfffffff1u:17u,
      .factor=variant?0x87654321u:3u,.job_id=31,.admit=ja};
    uint64_t off=variant?0xfffff000u:0x4000u;
    for(unsigned n=0;n<8;++n) {
        uint64_t addr=n==0?programs.usc.base+off:n<3?programs.pds.base+off+(n-1)*256:
            programs.general.base+0x400000+(n-3)*0x100000;
        config.buffers[n]=(struct pvr_compute_program_span){job_memory[n]+1,pvr_compute_job_bytes[n],addr};
        memset(job_memory[n],0xa5,sizeof(job_memory[n]));
    }
    admission=1;memset(&built,0xa5,sizeof(built));
}
#ifdef UPSTREAM
static void reference(void)
{
    const struct pvr_device_info *dev=&pvr_device_info_36_56_104_183;
    for(unsigned n=0;n<8;++n)memset(config.buffers[n].cpu,0,config.buffers[n].bytes);
    memset(&built,0,sizeof(built));
    const pco_precomp_data *usc=(const void *)cs_sunstone_probe_common_0_common;
    assert(usc->size_dwords==70&&usc->temps==6);
    memcpy(config.buffers[0].cpu,usc->binary,usc->size_dwords*4);
    uint32_t data[128]={0};
    struct pvr_pds_shared_storing_program p={.cc_enable=true,.doutw_control={.dest_store=PDS_UNIFIED_STORE,
      .num_const64=2,.doutw_data={config.buffers[PVR_JOB_INPUT].gpu,config.buffers[PVR_JOB_OUTPUT].gpu+64}}};
    pvr_pds_setup_doutu(&p.usc_task.usc_task_control,config.buffers[0].gpu-programs.usc.base,usc->temps,0,false);
    pvr_pds_generate_shared_storing_program(&p,data,PDS_GENERATE_DATA_SEGMENT,dev);
    uint32_t *end=pvr_pds_generate_compute_barrier_conditional(data+ALIGN_POT(p.data_size,4),PDS_GENERATE_CODE_SEGMENT);
    end=pvr_pds_generate_shared_storing_program(&p,end,PDS_GENERATE_CODE_SEGMENT,dev);
    assert(p.data_size==16&&end-data==22);memcpy(config.buffers[1].cpu,data,88);
    struct pvr_pds_compute_shader_program f;pvr_pds_compute_shader_program_init(&f);f.fence=true;f.clear_pds_barrier=true;
    pvr_pds_compute_shader(&f,NULL,PDS_GENERATE_SIZES,dev);memset(data,0,sizeof(data));
    uint32_t *code=pvr_pds_compute_shader(&f,data,PDS_GENERATE_DATA_SEGMENT,dev);
    end=pvr_pds_compute_shader(&f,code,PDS_GENERATE_CODE_SEGMENT,dev);
    assert(f.data_size==2&&f.code_size==20);memcpy(config.buffers[2].cpu,data,f.data_size*4);
    memcpy((unsigned char *)config.buffers[2].cpu+16,code,(size_t)(end-code)*4);
    uint32_t stream[1024]={0};struct pvr_csb csb={stream};struct pvr_sub_cmd_compute sub={0};
    uint32_t a=(uint32_t)(config.buffers[1].gpu-programs.pds.base),b=(uint32_t)(config.buffers[2].gpu-programs.pds.base);
    struct pvr_compute_kernel_info work={.usc_unified_size=4,.pds_data_size=4,.usc_target=ROGUE_CDMCTRL_USC_TARGET_ANY,
      .pds_data_offset=a,.pds_code_offset=a+64,.global_size={1,1,1},.local_size={32,1,1},.max_instances=32};
    pvr_compute_generate_control_stream(&csb,&sub,&work);
    struct pvr_compute_kernel_info fence={.pds_data_size=1,.usc_target=ROGUE_CDMCTRL_USC_TARGET_ANY,.is_fence=true,
      .pds_data_offset=b,.pds_code_offset=b+16,.sd_type=ROGUE_CDMCTRL_SD_TYPE_PDS,.usc_common_shared=true,
      .global_size={1,1,1},.local_size={1,1,1},.max_instances=ROGUE_CDM_MAX_PACKED_WORKGROUPS_PER_TASK};
    pvr_compute_generate_control_stream(&csb,&sub,&fence);
    pvr_csb_emit(&csb,CDMCTRL_STREAM_TERMINATE,term){}
    assert(csb.cursor-stream==15);memcpy(config.buffers[3].cpu,stream,60);
    uint32_t fwstream[11]={44,0};
    struct ROGUE_CR_TPU_BORDER_COLOUR_TABLE_CDM border={.border_colour_table_address=PVR_DEV_ADDR(config.buffers[7].gpu)};
    ROGUE_CR_TPU_BORDER_COLOUR_TABLE_CDM_pack(fwstream+2,&border);
    struct ROGUE_CR_CDM_CTRL_STREAM_BASE control={.addr=PVR_DEV_ADDR(config.buffers[3].gpu)};
    ROGUE_CR_CDM_CTRL_STREAM_BASE_pack(fwstream+4,&control);
    struct ROGUE_CR_CDM_CONTEXT_STATE_BASE state={.addr=PVR_DEV_ADDR(program_result.cdm_resume_address)};
    ROGUE_CR_CDM_CONTEXT_STATE_BASE_pack(fwstream+6,&state);
    struct ROGUE_CR_CDM_CONTEXT_PDS1 resume={.unified_size=4,.common_shared=true,.data_size=4};
    ROGUE_CR_CDM_CONTEXT_PDS1_pack(fwstream+8,&resume);
    struct rogue_fwif_cmd_compute cmd={0};struct pvr_device device={0};
    assert(!pvr_stream_process(&device,&pvr_cmd_compute_stream,fwstream,sizeof(fwstream),&cmd));
    cmd.flags=convert_compute_flags(DRM_PVR_SUBMIT_JOB_COMPUTE_CMD_PREVENT_ALL_OVERLAP|DRM_PVR_SUBMIT_JOB_COMPUTE_CMD_SINGLE_CORE);
    memcpy(built.compute,&cmd,sizeof(cmd));
    struct rogue_fwif_ufo ufo={.addr=config.firmware_ufo,.value=1};memcpy(built.update,&ufo,sizeof(ufo));
    put(config.buffers[4].cpu,0,config.seed);put(config.buffers[4].cpu,4,config.factor);
    for(unsigned n=0;n<32;++n)built.expected[n]=(config.seed+(n+1u)*config.factor)^(0x9e3779b9u+(n<<16));
    for(unsigned n=0;n<64;++n)put(config.buffers[5].cpu,4*n,n>=16&&n<48?~built.expected[n-16]:PVR_COMPUTE_JOB_GUARD);
}
#else
static struct pvr_compute_job job;
static struct pvr_cccb client;
static struct pvr_ccb queue;
static unsigned char kernel[5][128*88], visible[1024];
static uint64_t now,epoch;
static unsigned io,kicks,reads,published,fail_publish,late_publish,fail_output,backwards;
static int qr(void *ctx,enum pvr_ccb_region r,uint32_t off,void *out,size_t n)
{(void)ctx;assert(off+n<=pvr_ccb_region_bytes(r));++io;memcpy(out,kernel[r]+off,n);return 1;}
static int qw(void *ctx,enum pvr_ccb_region r,uint32_t off,const void *in,size_t n)
{(void)ctx;assert(off+n<=pvr_ccb_region_bytes(r));++io;memcpy(kernel[r]+off,in,n);return 1;}
static int qb(void *ctx,enum pvr_ccb_barrier b){(void)ctx;(void)b;++io;return 1;}
static int qk(void *ctx,uint32_t off,uint32_t v)
{
    (void)ctx;assert(off==0xb00&&!v);++io;++kicks;assert(published);
    uint32_t next=get(kernel[0],0),slot=(next-1)&127;const unsigned char *cmd=kernel[1]+slot*88;
    assert(get(cmd,0)==(PVR_CCB_MAGIC|101u)&&get(cmd,8)==config.firmware_context);
    assert(get(visible,0)==(PVR_CCB_MAGIC|PVR_CCCB_TASK|205u)&&get(visible,4)==96);
    assert(!memcmp(visible+40,built.compute,96));assert(get(visible,136)==(PVR_CCB_MAGIC|213u));
    assert(!memcmp(visible+176,built.update,8));
    put(context_memory[2],0,get(cmd,12));put(kernel[0],128,next);put(kernel[2],slot*4,1);return 1;
}
static int cr(void *ctx,enum pvr_cccb_region r,uint32_t off,void *out,size_t n)
{(void)ctx;assert(r==PVR_CCCB_CONTROL&&n==4&&off+4<=32);++io;memcpy(out,context_memory[2]+off,n);return 1;}
static int cw(void *ctx,enum pvr_cccb_region r,uint32_t off,const void *in,size_t n)
{(void)ctx;++io;if(r==PVR_CCCB_CONTROL){assert(!off&&n==32);memcpy(context_memory[2],in,n);}else{assert(off+n<=1024);memcpy(context_memory[3]+off,in,n);}return 1;}
static int cb(void *ctx,enum pvr_ccb_barrier b)
{(void)ctx;++io;if(b==PVR_CCB_RELEASE)memcpy(visible,context_memory[3],1024);return 1;}
static int clock_now(void *ctx,uint64_t *out){(void)ctx;++io;*out=backwards?0:now;return 1;}
static int publish(void *ctx,const struct pvr_compute_job_config *c,uint64_t deadline)
{(void)ctx;assert(c==&config&&deadline==job.deadline);++io;published=1;if(late_publish)now=deadline;return !fail_publish;}
static int fence_read(void *ctx,uint32_t *out){(void)ctx;assert(published);++io;*out=get(config.buffers[6].cpu,0);return 1;}
static int output_read(void *ctx,void *out,size_t n)
{(void)ctx;assert(n==256);++io;++reads;memcpy(out,config.buffers[5].cpu,n);return !fail_output;}
static const struct pvr_compute_job_ops job_ops={clock_now,publish,fence_read,output_read};
static void runtime(void)
{
    memset(&job,0,sizeof(job));memset(&client,0,sizeof(client));memset(&queue,0,sizeof(queue));memset(kernel,0,sizeof(kernel));
    put(config.buffers[6].cpu,0,0);now=1;io=kicks=reads=published=fail_publish=late_publish=fail_output=backwards=0;
    const struct pvr_ccb_ops qops={qr,qw,qb,qk};const struct pvr_cccb_ops cops={clock_now,cr,cw,cb};
    assert(pvr_ccb_init(&queue,&qops,NULL,++epoch)==PVR_CCB_OK);
    assert(pvr_cccb_init(&client,&cops,NULL,&queue,1024,config.firmware_context,10000));
    assert(pvr_cccb_poll(&client)==PVR_CCCB_OK);assert(pvr_ccb_start(&queue,++now)==PVR_CCB_OK);
    assert(pvr_compute_job_init(&job,&config,&built,&job_ops,NULL,&client,10000));
}
static int tick(void)
{++now;if(queue.phase==PVR_CCB_ACTIVE)assert(pvr_ccb_poll(&queue,now)==PVR_CCB_OK);return pvr_compute_job_poll(&job);}
static void until(unsigned phase)
{for(unsigned n=0;n<64&&job.phase!=phase;++n)assert(tick()==0);assert(job.phase==phase);}
static void completed_data(void)
{
    for(unsigned n=0;n<32;++n)put(config.buffers[5].cpu,64+4*n,(config.seed+(n+1u)*config.factor)^(0x9e3779b9u+(n<<16)));
    put(config.buffers[6].cpu,0,1);
}
static void terminal(unsigned error)
{
    assert(job.phase==PVR_JOB_FAILED&&job.error==error&&queue.phase==PVR_CCB_FAILED);
    unsigned before=io;assert(pvr_compute_job_poll(&job)==-1);assert(pvr_compute_job_abort(&job)==-1);assert(io==before);
}
static void init_checks(void)
{
    runtime();struct pvr_compute_job empty={0},snapshot=empty;
    struct pvr_ccb saved_queue=queue;unsigned before=io;
    client.size=512;
    assert(!pvr_compute_job_init(&empty,&config,&built,&job_ops,NULL,&client,10000));
    client.size=1024;
    struct pvr_compute_job_config aliased=config;aliased.buffers[0].cpu=&queue;
    assert(!pvr_compute_job_init(&empty,&aliased,&built,&job_ops,NULL,&client,10000));
    assert(!memcmp(&empty,&snapshot,sizeof(empty))&&!memcmp(&queue,&saved_queue,sizeof(queue))&&io==before);
    fprintf(stderr,"ring-size and mutable queue alias refusal passed without I/O or state changes\n");
}
static unsigned refusals;
static void refuse(struct pvr_compute_job_config c)
{
    struct pvr_compute_job_image image;memset(&image,0xa5,sizeof(image));
    for(unsigned n=0;n<8;++n)memset(job_memory[n],0xa5,sizeof(job_memory[n]));
    assert(!pvr_compute_job_build(&c,&image));
    for(unsigned n=0;n<8;++n)for(size_t k=0;k<sizeof(job_memory[n]);++k)assert(job_memory[n][k]==0xa5);
    for(size_t k=0;k<sizeof(image);++k)assert(((unsigned char *)&image)[k]==0xa5);
    ++refusals;
}
static void completion_checks(void)
{
    runtime();until(PVR_JOB_WAIT_FENCE);assert(kicks==1&&reads==0);assert(tick()==0&&reads==0);
    completed_data();assert(tick()==0&&reads==0);assert(tick()==1&&reads==1);
    unsigned before=io;assert(pvr_compute_job_poll(&job)==1&&pvr_compute_job_abort(&job)==1&&io==before);
    runtime();until(PVR_JOB_WAIT_FENCE);put(config.buffers[6].cpu,0,2);assert(tick()==-1);terminal(PVR_JOB_FENCE_VALUE);
    runtime();until(PVR_JOB_WAIT_FENCE);completed_data();put(config.buffers[5].cpu,0,0);assert(tick()==0);assert(tick()==-1);terminal(PVR_JOB_RESULT);
    put(config.buffers[5].cpu,0,PVR_COMPUTE_JOB_GUARD);
    runtime();until(PVR_JOB_WAIT_FENCE);completed_data();put(config.buffers[5].cpu,64,0);assert(tick()==0);assert(tick()==-1);terminal(PVR_JOB_RESULT);
    runtime();fail_publish=1;assert(tick()==-1&&published&&!kicks);terminal(PVR_JOB_IO);
    runtime();late_publish=1;assert(tick()==-1&&!kicks);terminal(PVR_JOB_TIMEOUT);
    runtime();until(PVR_JOB_WAIT_FENCE);now=job.deadline;assert(tick()==-1);terminal(PVR_JOB_TIMEOUT);
    runtime();backwards=1;assert(tick()==-1);terminal(PVR_JOB_CLOCK);
    runtime();queue.last_now=now+10;assert(pvr_compute_job_poll(&job)==-1);terminal(PVR_JOB_CLOCK);
    runtime();until(PVR_JOB_WAIT_FENCE);completed_data();assert(tick()==0);fail_output=1;assert(tick()==-1);terminal(PVR_JOB_IO);
    fprintf(stderr,"completion/retained-fault model passed with publication before initial fence read\n");
}
static void checks(void)
{
    completion_checks();
    struct pvr_compute_job_config c;
#define BAD(change) do{c=config;change;refuse(c);}while(0)
    BAD(c.admit=NULL);BAD(c.firmware_context++);BAD(c.firmware_ufo++);BAD(c.firmware_ufo=c.firmware_context);
    BAD(c.firmware_ufo=context_buffers[3].address+512);BAD(c.program_result=NULL);BAD(c.programs=NULL);
    for(unsigned n=0;n<8;++n) {
        BAD(c.buffers[n].bytes--);BAD(c.buffers[n].gpu++);BAD(c.buffers[n].cpu=NULL);
        BAD(c.buffers[n].cpu=programs.buffers[0].cpu);BAD(c.buffers[n].gpu=programs.buffers[0].gpu);
        BAD(c.buffers[n].cpu=context_memory[0]);BAD(c.buffers[n].gpu=programs.excluded[0].gpu);
    }
    BAD(c.buffers[PVR_JOB_CDM].gpu+=64);BAD(c.buffers[0].gpu=programs.usc.base+programs.usc.bytes-64);
    admission=0;refuse(config);admission=-1;refuse(config);admission=1;
    fprintf(stderr,"completion/retained-fault model passed; atomic refusals %u\n",refusals);
}
#endif
int main(int argc,char **argv)
{
    assert(argc==3);unsigned variant=(unsigned)strtoul(argv[1],NULL,0);assert(variant<4);setup(variant>=2?0:variant);
#ifndef UPSTREAM
    if(variant==3){assert(pvr_compute_job_build(&config,&built));completion_checks();return 0;}
    if(variant==2){assert(pvr_compute_job_build(&config,&built));init_checks();return 0;}
#endif
#ifdef UPSTREAM
    reference();
#else
    assert(pvr_compute_job_build(&config,&built));
#endif
    FILE *f=fopen(argv[2],"wb");assert(f);
    for(unsigned n=0;n<8;++n){assert(job_memory[n][0]==0xa5&&job_memory[n][config.buffers[n].bytes+1]==0xa5);assert(fwrite(config.buffers[n].cpu,config.buffers[n].bytes,1,f)==1);}
    assert(fwrite(&built,sizeof(built),1,f)==1);assert(!fclose(f));
#ifndef UPSTREAM
    if(!variant)checks();
#endif
    return 0;
}
