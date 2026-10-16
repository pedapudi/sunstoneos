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
#include "pvr_compute_programs.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef UPSTREAM
#include "pvr_pds.h"
#include "common/device_info/bxm-4-64.h"
#include "pvr_types.h"
#define __pvr_address_type pvr_dev_addr_t
#define __pvr_get_address(a) (a).addr
#define __pvr_make_address(a) PVR_DEV_ADDR(a)
#include "csbgen/rogue/cr.h"
#include "reference-usc.inc"
static const uint32_t sizes[10] = {576,576,128,128,128,128,64,16448,16448,512};
#define pvr_compute_program_bytes sizes
static void reference(const struct pvr_compute_program_config *c,
                      struct pvr_compute_program_result *out)
{
    const struct pvr_device_info *dev = &pvr_device_info_36_56_104_183;
    const pco_precomp_data *usc[2] = {
        (const void *)cs_store_sr_1024_common_0_common,
        (const void *)cs_load_sr_256_common_0_common
    };
    assert(usc[0]->temps == 10 && usc[1]->temps == 10);
    assert(usc[0]->size_dwords == 136 && usc[1]->size_dwords == 122);
    uint32_t offset[5], data_size[5];
    for(unsigned i=0;i<10;++i) memset(c->buffers[i].cpu,0,c->buffers[i].bytes);
    for(unsigned i=0;i<2;++i) memcpy(c->buffers[i].cpu,usc[i]->binary,usc[i]->size_dwords*4);
    for(unsigned i=0;i<4;++i) {
        uint64_t state=c->buffers[7+i/2].gpu;
        uint32_t buf[64]={0};
        struct pvr_pds_shared_storing_program p={.cc_enable=PVR_HAS_ENHANCEMENT(dev,35421),
          .doutw_control={.dest_store=PDS_UNIFIED_STORE,.num_const64=2,
                         .doutw_data={state,state+48},.last_instruction=false}};
        pvr_pds_setup_doutu(&p.usc_task.usc_task_control,
                            c->buffers[i%2].gpu-c->usc.base,usc[i%2]->temps,0,false);
        pvr_pds_generate_shared_storing_program(&p,buf,PDS_GENERATE_DATA_SEGMENT,dev);
        offset[i]=ALIGN_POT(p.data_size,4)*4;data_size[i]=offset[i]/4;
        uint32_t *end=pvr_pds_generate_compute_barrier_conditional(buf+offset[i]/4,PDS_GENERATE_CODE_SEGMENT);
        end=pvr_pds_generate_shared_storing_program(&p,end,PDS_GENERATE_CODE_SEGMENT,dev);
        assert(offset[i]==64 && end-buf==22);
        memcpy(c->buffers[i+2].cpu,buf,(size_t)(end-buf)*4);
    }
    uint32_t buf[64]={0};struct pvr_pds_fence_program f={0};
    pvr_pds_generate_fence_terminate_program(&f,buf,PDS_GENERATE_DATA_SEGMENT,dev);
    offset[4]=ALIGN_POT(f.data_size,4)*4;data_size[4]=offset[4]/4;
    uint32_t *end=pvr_pds_generate_fence_terminate_program(&f,buf+offset[4]/4,PDS_GENERATE_CODE_SEGMENT,dev);
    assert(offset[4]==16 && end-buf==9);
    memcpy(c->buffers[6].cpu,buf,(size_t)(end-buf)*4);
    const unsigned indices[5]={0,4,5,6,2};
    for(unsigned i=0;i<5;++i) {
        uint64_t off=c->buffers[i+2].gpu-c->pds.base;
        struct ROGUE_CR_CDM_CONTEXT_PDS0 p={.data_addr=PVR_DEV_ADDR(off),.code_addr=PVR_DEV_ADDR(off+offset[i])};
        ROGUE_CR_CDM_CONTEXT_PDS0_pack(&out->switch_registers[indices[i]],&p);
    }
    struct ROGUE_CR_CDM_CONTEXT_PDS1 s={.pds_seq_dep=true,.target=true,.unified_size=4,.data_size=data_size[0]/4,.fence=true};
    struct ROGUE_CR_CDM_TERMINATE_PDS1 t={.pds_seq_dep=true,.target=!PVR_HAS_FEATURE(dev,compute_morton_capable),.common_shared=true,.data_size=data_size[4]/4,.fence=true};
    out->switch_registers[1]=out->switch_registers[3]=0;
    ROGUE_CR_CDM_CONTEXT_PDS1_pack(&out->switch_registers[1],&s);
    ROGUE_CR_CDM_TERMINATE_PDS1_pack(&out->switch_registers[3],&t);
    out->cdm_resume_address=c->buffers[9].gpu;
}
#endif
static unsigned char backing[10][16450], excluded[5][256];
static int permission=1, calls;
static int admit(void *ctx,const struct pvr_compute_program_config *c)
{ assert(ctx==&permission && c->admission_token==17);++calls;return permission; }
static struct pvr_compute_program_config config(unsigned variant)
{
    struct pvr_compute_program_config c={.bvnc=PVR_FW_A733_BVNC,
      .pds={variant?UINT64_C(0xfe00000000):UINT64_C(0x400000000),UINT64_C(1)<<32},
      .usc={variant?UINT64_C(0xfd00000000):UINT64_C(0x500000000),UINT64_C(1)<<32},
      .general={variant?UINT64_C(0xfc00000000):UINT64_C(0x600000000),UINT64_C(1)<<32},
      .excluded_count=5,.execution_cores=1,.admission_token=17,.admission_context=&permission,.admit=admit};
    uint64_t offset=variant?UINT64_C(0xfffff000):0x1000;
    for(unsigned i=0;i<10;++i) {
        uint64_t gpu=i<2?c.usc.base+offset+i*1024:
          i<7?c.pds.base+offset+(i-2)*256:
          i<9?c.pds.base+0x200000+(i-7)*0x10000:c.general.base+0x10000;
        c.buffers[i]=(struct pvr_compute_program_span){backing[i]+1,pvr_compute_program_bytes[i],gpu};
    }
    static const size_t sizes[5]={232,4,32,256,32};
    for(unsigned i=0;i<5;++i)c.excluded[i]=(struct pvr_compute_program_span){excluded[i],sizes[i],c.general.base+0x100000+i*0x10000};
    return c;
}
#ifndef UPSTREAM
static unsigned refusals;
static void refuse(struct pvr_compute_program_config c)
{
    struct pvr_compute_program_result out;
    memset(backing,0xa5,sizeof(backing));memset(&out,0xa5,sizeof(out));
    assert(!pvr_compute_programs_build(&c,&out));
    for(size_t i=0;i<sizeof(backing);++i)assert(((unsigned char *)backing)[i]==0xa5);
    for(size_t i=0;i<sizeof(out);++i)assert(((unsigned char *)&out)[i]==0xa5);
    ++refusals;
}
static void invalid(void)
{
    struct pvr_compute_program_config c;
#define BAD(expr) do {c=config(0);expr;refuse(c);} while(0)
    BAD(c.bvnc++);BAD(c.execution_cores=0);BAD(c.execution_cores=2);
    BAD(c.admission_token=0);BAD(c.admit=NULL);BAD(c.excluded_count=4);BAD(c.excluded_count=17);
    BAD(c.pds.base++);BAD(c.usc.bytes+=4096);BAD(c.general.base=UINT64_C(1)<<40);
    BAD(c.usc.base=c.pds.base);BAD(c.general.base=c.pds.base);
    BAD(c.buffers[0].gpu=c.usc.base+c.usc.bytes-64);
    BAD(c.buffers[2].gpu=c.pds.base+c.pds.bytes-64);
    for(unsigned i=0;i<10;++i) {
        BAD(c.buffers[i].bytes--);BAD(c.buffers[i].cpu=NULL);BAD(c.buffers[i].gpu++);
        BAD(c.buffers[i].cpu=(void *)(UINTPTR_MAX-16));
        BAD(c.buffers[i].gpu=UINT64_MAX-32);
        BAD(c.buffers[i].cpu=c.buffers[(i+1)%10].cpu);
        BAD(c.buffers[i].gpu=c.buffers[(i+1)%10].gpu);
        BAD(c.buffers[i].cpu=c.excluded[0].cpu);
        BAD(c.excluded[0].gpu=c.buffers[i].gpu);
    }
    BAD(c.excluded[0].cpu=NULL);BAD(c.excluded[0].gpu=UINT64_MAX);BAD(c.excluded[0].bytes=SIZE_MAX);
    permission=0;refuse(config(0));permission=-1;refuse(config(0));permission=1;
    /* Input/result alias and input/owned alias are refused before dereference/write. */
    c=config(0);struct pvr_compute_program_config saved=c;
    assert(!pvr_compute_programs_build(&c,(void *)&c));assert(!memcmp(&c,&saved,sizeof(c)));
    c.buffers[0].cpu=&c;saved=c;struct pvr_compute_program_result out;
    assert(!pvr_compute_programs_build(&c,&out));assert(!memcmp(&c,&saved,sizeof(c)));
    c=config(0);memset(backing,0xa5,sizeof(backing));
    assert(!pvr_compute_programs_build(&c,(void *)(backing[0]+1)));
    for(size_t i=0;i<sizeof(backing);++i)assert(((unsigned char *)backing)[i]==0xa5);
    assert(!pvr_compute_programs_build(NULL,&out));assert(!pvr_compute_programs_build(&c,NULL));
    fprintf(stderr,"atomic refusals %u; aliases/nulls 5\n",refusals);
}
#endif
int main(int argc,char **argv)
{
    #ifndef UPSTREAM
    if (argc==2 && !strcmp(argv[1],"--alignment-only")) {
        struct pvr_compute_program_config c=config(0);
        struct pvr_compute_program_result out;
        _Alignas(struct pvr_compute_program_config) unsigned char input[sizeof(c)+1];
        _Alignas(struct pvr_compute_program_result) unsigned char result[sizeof(out)+1];
        memcpy(input+1,&c,sizeof(c));memset(result,0xa5,sizeof(result));
        memset(backing,0xa5,sizeof(backing));memset(&out,0xa5,sizeof(out));
        assert(!pvr_compute_programs_build((void *)(input+1),&out));
        assert(!pvr_compute_programs_build(&c,(void *)(result+1)));
        for(size_t i=0;i<sizeof(out);++i)assert(((unsigned char *)&out)[i]==0xa5);
        for(size_t i=0;i<sizeof(result);++i)assert(result[i]==0xa5);
        for(size_t i=0;i<sizeof(backing);++i)assert(((unsigned char *)backing)[i]==0xa5);
        assert(!calls);puts("unaligned input/result refused before callback or writes");return 0;
    }
#endif
    assert(argc==3);unsigned variant=(unsigned)strtoul(argv[1],NULL,0);assert(variant<2);
    struct pvr_compute_program_config c=config(variant);struct pvr_compute_program_result out;
    memset(backing,0xa5,sizeof(backing));memset(&out,0xa5,sizeof(out));
#ifdef UPSTREAM
    reference(&c,&out);
#else
    assert(pvr_compute_programs_build(&c,&out));assert(calls==1);
#endif
    FILE *f=fopen(argv[2],"wb");assert(f);
    for(unsigned i=0;i<10;++i) {
        assert(backing[i][0]==0xa5 && backing[i][c.buffers[i].bytes+1]==0xa5);
        assert(fwrite(c.buffers[i].cpu,c.buffers[i].bytes,1,f)==1);
    }
    assert(fwrite(&out,sizeof(out),1,f)==1);assert(!fclose(f));
#ifndef UPSTREAM
    if(!variant)invalid();
#endif
    return 0;
}
