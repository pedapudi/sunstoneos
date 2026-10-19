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
#include <string.h>
#include "pvr_compute_probe.inc"
_Static_assert(PVR_COMPUTE_DISPATCH_MAX*128u+64u<=4096u-64u,"PDS data, code and trailing fetch space");
_Static_assert(PVR_COMPUTE_DISPATCH_MAX*28u+32u<=4096u-64u,"CDM dispatch/fence/terminate guard");
_Static_assert(sizeof(probe_usc)==PVR_COMPUTE_JOB_USC_BYTES,"USC probe size");
const uint32_t pvr_compute_job_bytes[PVR_JOB_REGIONS]={320,128,64,4096,64,256,64,262144};
size_t pvr_compute_job_region_bytes(const struct pvr_compute_job_config *c,unsigned i)
{
    if(i>=PVR_JOB_REGIONS)return 0;
    if(c->package&&c->package->profile==2&&i==PVR_JOB_PDS)return PVR_COMPUTE_DISPATCH_PDS_BYTES;
    if(c->package&&(i==PVR_JOB_USC||i==PVR_JOB_INPUT||i==PVR_JOB_OUTPUT))return 4096;
    return pvr_compute_job_bytes[i];
}
unsigned pvr_compute_job_result_words(const struct pvr_compute_job_config *c)
{return c->package?c->package->output_bytes/4:PVR_COMPUTE_JOB_WORDS;}
static int range(const void *p,size_t n)
{ return p && n && n<=UINTPTR_MAX-(uintptr_t)p; }
static int gpu(uint64_t a,size_t n)
{ return a && n && a<(UINT64_C(1)<<40) && n<=(UINT64_C(1)<<40)-a; }
static int overlap(uint64_t a,uint64_t n,uint64_t b,uint64_t m)
{ return a<b+m && b<a+n; }
static int apart(const void *a,size_t n,const void *b,size_t m)
{ return range(a,n)&&range(b,m)&&!overlap((uintptr_t)a,n,(uintptr_t)b,m); }
static uint32_t get32(const void *p,size_t off)
{ const unsigned char *b=p;return (uint32_t)b[off]|(uint32_t)b[off+1]<<8|(uint32_t)b[off+2]<<16|(uint32_t)b[off+3]<<24; }
static uint64_t get64(const void *p,size_t off)
{ return get32(p,off)|((uint64_t)get32(p,off+4)<<32); }
static void put32(void *p,size_t off,uint32_t v)
{ unsigned char *b=p;for(unsigned i=0;i<4;++i)b[off+i]=(unsigned char)(v>>(8*i)); }
static void put64(void *p,size_t off,uint64_t v)
{ put32(p,off,(uint32_t)v);put32(p,off+4,(uint32_t)(v>>32)); }
static void clear(void *p,size_t n)
{ unsigned char *b=p;for(size_t i=0;i<n;++i)b[i]=0; }
static void words(void *p,size_t off,const uint32_t *w,size_t count)
{ for(size_t i=0;i<count;++i)put32(p,off+4*i,w[i]); }
static int span(const struct pvr_compute_program_span *s)
{ return range(s->cpu,s->bytes)&&gpu(s->gpu,s->bytes); }
static int heap(const struct pvr_compute_program_heap *h,int relative)
{ return gpu(h->base,h->bytes)&&!(h->base&4095)&&!(h->bytes&4095)&&(!relative||h->bytes<=(UINT64_C(1)<<32)); }
static int inside(const struct pvr_compute_program_heap *h,const struct pvr_compute_program_span *s)
{ return s->gpu>=h->base&&s->gpu-h->base<=h->bytes&&s->bytes<=h->bytes-(s->gpu-h->base); }
static int fw(uint32_t a,size_t n,unsigned alignment)
{ return a>=0xc0000000u&&a<0xc1000000u&&n<=0xc1000000u-a&&!(a&(alignment-1)); }
static int valid(const struct pvr_compute_job_config *c,const void *out,size_t out_size)
{
    if(!range(c,sizeof(*c))||(uintptr_t)c%_Alignof(struct pvr_compute_job_config)||
       !apart(c,sizeof(*c),out,out_size)||!c->admit||
       !range(c->programs,sizeof(*c->programs))||
       (uintptr_t)c->programs%_Alignof(struct pvr_compute_program_config)||
       !range(c->program_result,sizeof(*c->program_result))||
       (uintptr_t)c->program_result%_Alignof(struct pvr_compute_program_result)||
       !apart(c->programs,sizeof(*c->programs),out,out_size)||
       !apart(c->program_result,sizeof(*c->program_result),out,out_size)||
       !fw(c->firmware_context,232,8)||!fw(c->firmware_ufo,64,4))return 0;
    if(c->package&&(!range(c->package,sizeof(*c->package))||!pvr_compute_package_valid(c->package)||
       !apart(c->package,sizeof(*c->package),out,out_size)))return 0;
    const struct pvr_compute_program_config *p=c->programs;
    if(p->bvnc!=PVR_FW_A733_BVNC||p->execution_cores!=1||!p->admission_token||
       p->excluded_count<5||p->excluded_count>PVR_COMPUTE_PROGRAM_EXCLUSIONS||
       !heap(&p->pds,1)||!heap(&p->usc,1)||!heap(&p->general,0)||
       overlap(p->pds.base,p->pds.bytes,p->usc.base,p->usc.bytes)||
       overlap(p->pds.base,p->pds.bytes,p->general.base,p->general.bytes)||
       overlap(p->usc.base,p->usc.bytes,p->general.base,p->general.bytes))return 0;
    static const uint32_t min_size[5]={232,4,32,256,32};
    for(unsigned i=0;i<p->excluded_count;++i)
        if(!span(&p->excluded[i])||(i<5&&p->excluded[i].bytes<min_size[i])||
           !apart(p->excluded[i].cpu,p->excluded[i].bytes,out,out_size))return 0;
    for(unsigned i=0;i<PVR_PROGRAM_REGIONS;++i)
        if(!span(&p->buffers[i])||(p->buffers[i].gpu&63)||p->buffers[i].bytes!=pvr_compute_program_bytes[i]||
           !apart(p->buffers[i].cpu,p->buffers[i].bytes,out,out_size))return 0;
    const void *context=p->excluded[0].cpu;
    uint32_t addresses[5]={c->firmware_context,get32(context,24),get32(context,0),
                           get32(context,4),get32(context,136)};
    for(unsigned i=0;i<5;++i)
        if(!fw(addresses[i],p->excluded[i].bytes,i==1?4:8)||
           overlap(c->firmware_ufo,64,addresses[i],p->excluded[i].bytes))return 0;
    for(unsigned i=0;i<7;++i)
        if(get64(context,152+8*i)!=c->program_result->switch_registers[i])return 0;
    if(c->program_result->cdm_resume_address!=p->buffers[PVR_PROGRAM_CDM_RESUME].gpu)return 0;
    for(unsigned i=0;i<PVR_JOB_REGIONS;++i) {
        const struct pvr_compute_program_span *s=&c->buffers[i];
        const struct pvr_compute_program_heap *h=i==PVR_JOB_USC?&p->usc:
            i==PVR_JOB_PDS||i==PVR_JOB_FENCE_PDS?&p->pds:&p->general;
        if(!span(s)||s->bytes!=pvr_compute_job_region_bytes(c,i)||(s->gpu&63)||
           (i==PVR_JOB_CDM&&(s->gpu&4095))||(i!=PVR_JOB_UFO&&!inside(h,s))||
           !apart(s->cpu,s->bytes,c,sizeof(*c))||!apart(s->cpu,s->bytes,p,sizeof(*p))||
           !apart(s->cpu,s->bytes,c->program_result,sizeof(*c->program_result))||
           !apart(s->cpu,s->bytes,out,out_size))return 0;
        if(c->package&&!apart(s->cpu,s->bytes,c->package,sizeof(*c->package)))return 0;
        for(unsigned j=0;j<i;++j)
            if(!apart(s->cpu,s->bytes,c->buffers[j].cpu,c->buffers[j].bytes)||
               overlap(s->gpu,s->bytes,c->buffers[j].gpu,c->buffers[j].bytes))return 0;
        for(unsigned j=0;j<PVR_PROGRAM_REGIONS;++j)
            if(!apart(s->cpu,s->bytes,p->buffers[j].cpu,p->buffers[j].bytes)||
               overlap(s->gpu,s->bytes,p->buffers[j].gpu,p->buffers[j].bytes))return 0;
        for(unsigned j=0;j<p->excluded_count;++j)
            if(!apart(s->cpu,s->bytes,p->excluded[j].cpu,p->excluded[j].bytes)||
               overlap(s->gpu,s->bytes,p->excluded[j].gpu,p->excluded[j].bytes))return 0;
    }
    return 1;
}
static void image(const struct pvr_compute_job_config *c,struct pvr_compute_job_image *out)
{
    clear(out,sizeof(*out));
    put64(out->compute,8,c->buffers[PVR_JOB_BORDER].gpu>>2);
    put64(out->compute,40,c->buffers[PVR_JOB_CDM].gpu);
    put64(out->compute,48,c->program_result->cdm_resume_address);
    /* Resume unified four, shared allocation, four 16-byte PDS data units. */
    put32(out->compute,60,(4u<<21)|(1u<<20)|(4u<<1));
    /* Source flags: prevent all overlap and execute on one GPU core. */
    put32(out->compute,80,(1u<<2)|(1u<<5));
    put32(out->update,0,c->firmware_ufo);put32(out->update,4,1);
    for(unsigned i=0;!c->package&&i<PVR_COMPUTE_JOB_WORDS;++i)
        out->expected[i]=(c->seed+(i+1u)*c->factor)^(0x9e3779b9u+(i<<16));
}
int pvr_compute_job_build(const struct pvr_compute_job_config *c,struct pvr_compute_job_image *out)
{
    if((uintptr_t)out%_Alignof(struct pvr_compute_job_image)||!valid(c,out,sizeof(*out))||
       c->admit(c->admission_context,c)!=1)return 0;
    for(unsigned i=0;i<PVR_JOB_REGIONS;++i)clear(c->buffers[i].cpu,c->buffers[i].bytes);
    image(c,out);
    if(c->package)memcpy(c->buffers[PVR_JOB_USC].cpu,c->package->code,c->package->code_bytes);
    else words(c->buffers[PVR_JOB_USC].cpu,0,probe_usc,70);
    void *pds=c->buffers[PVR_JOB_PDS].cpu;
    uint64_t input=c->buffers[PVR_JOB_INPUT].gpu,output=c->buffers[PVR_JOB_OUTPUT].gpu+64;
    put32(pds,0,(uint32_t)(input>>32));put32(pds,4,(uint32_t)input);put32(pds,8,2);
    put32(pds,16,(uint32_t)(output>>32));put32(pds,20,(uint32_t)output);put32(pds,24,0x80004002u);
    put64(pds,32,(c->buffers[PVR_JOB_USC].gpu-c->programs->usc.base)|((uint64_t)(c->package?(pvr_compute_package_temps(c->package)+3)/4:2)<<35));
    static const uint32_t dispatch[]={0xc1100001u,0xdb000000u,0xc1080001u,0xf0020001u,0xf0060201u,0xfc000402u};
    words(pds,64,dispatch,6);
    static const uint32_t fence[]={0xc2c00002u,0xd3000000u,0x80000010u,0xf0000005u,0xd3000000u};
    words(c->buffers[PVR_JOB_FENCE_PDS].cpu,16,fence,5);
    void *cdm=c->buffers[PVR_JOB_CDM].cpu;
    uint32_t a=(uint32_t)(c->buffers[PVR_JOB_PDS].gpu-c->programs->pds.base);
    uint32_t b=(uint32_t)(c->buffers[PVR_JOB_FENCE_PDS].gpu-c->programs->pds.base);
    /* Kernel: 32 instances, one workgroup, four unified blocks, any USC. */
    put32(cdm,0,((c->package?1u:4u)<<12)|(4u<<2)|(1u<<1));put32(cdm,4,a);put32(cdm,8,a+64);
    put32(cdm,24,31u<<18);
    /* Fence: eight maximum packed one-instance groups, PDS dependency, shared free. */
    put32(cdm,28,(1u<<2)|(1u<<1)|1u);put32(cdm,32,b|(1u<<2)|(1u<<1));
    put32(cdm,36,b+16);put32(cdm,52,8u<<27);put32(cdm,56,2u<<30);
    if(c->package&&c->package->profile==2) {
        const struct pvr_compute_package *p=c->package;
        uint64_t task=(c->buffers[PVR_JOB_USC].gpu-c->programs->usc.base)|
            ((uint64_t)((pvr_compute_package_temps(p)+3)/4)<<35);
        static const uint32_t code[]={0xc1100001u,0xdb000000u,0xc1080001u,
            0xf0020001u,0xf0060201u,0xf00a0401u,0xfc000602u};
        /* Replace legacy data/code. All31 slots and shared code fit one page. */
        clear(pds,c->buffers[PVR_JOB_PDS].bytes);
        clear(cdm,c->buffers[PVR_JOB_CDM].bytes);
        words(pds,PVR_COMPUTE_DISPATCH_PDS_CODE,code,7);
        for(unsigned group=0;group<p->dispatch_count;++group) {
            unsigned first=group*PVR_COMPUTE_DISPATCH_LANES;
            unsigned count=p->count-first;
            if(count>PVR_COMPUTE_DISPATCH_LANES)count=PVR_COMPUTE_DISPATCH_LANES;
            size_t slot=(size_t)group*PVR_COMPUTE_DISPATCH_PDS_SLOT,stream=(size_t)group*28;
            put32(pds,slot,(uint32_t)(input>>32));put32(pds,slot+4,(uint32_t)input);put32(pds,slot+8,2);
            put32(pds,slot+16,(uint32_t)(output>>32));put32(pds,slot+20,(uint32_t)output);put32(pds,slot+24,0x4002);
            /* Mesa H32/L32 constant order: even USC register gets low half. */
            put32(pds,slot+32,count);put32(pds,slot+36,first);put32(pds,slot+40,0x80008002u);
            put64(pds,slot+48,task);
            put32(cdm,stream,(2u<<12)|(5u<<2)|(1u<<1));
            put32(cdm,stream+4,a+(uint32_t)slot);
            put32(cdm,stream+8,a+PVR_COMPUTE_DISPATCH_PDS_CODE);
            put32(cdm,stream+24,31u<<18);
        }
        size_t fence=(size_t)p->dispatch_count*28;
        put32(cdm,fence,(1u<<2)|(1u<<1)|1u);put32(cdm,fence+4,b|(1u<<2)|(1u<<1));
        put32(cdm,fence+8,b+16);put32(cdm,fence+24,8u<<27);put32(cdm,fence+28,2u<<30);
    }
    if(c->package) {
        memcpy(c->buffers[PVR_JOB_INPUT].cpu,c->package->input,c->package->input_bytes);
        for(unsigned i=0;i<1024;++i)put32(c->buffers[PVR_JOB_OUTPUT].cpu,4*i,
            i>=16&&i<16+pvr_compute_job_result_words(c)?UINT32_C(0xa5a5a5a5):PVR_COMPUTE_JOB_GUARD);
        return 1;
    }
    put32(c->buffers[PVR_JOB_INPUT].cpu,0,c->seed);put32(c->buffers[PVR_JOB_INPUT].cpu,4,c->factor);
    for(unsigned i=0;i<64;++i)put32(c->buffers[PVR_JOB_OUTPUT].cpu,4*i,
        i>=16&&i<48?~out->expected[i-16]:PVR_COMPUTE_JOB_GUARD);
    return 1;
}
static int fail(struct pvr_compute_job *j,unsigned error)
{ j->phase=PVR_JOB_FAILED;j->error=error;pvr_cccb_abort(j->transport);return -1; }
static int clock_ok(struct pvr_compute_job *j)
{
    uint64_t now=0;
    if(j->ops.now(j->context,&now)!=1||now<j->last_now||now<j->transport->last_now||
       now<j->transport->queue->last_now)return fail(j,PVR_JOB_CLOCK);
    j->last_now=now;
    if(now>=j->deadline)return fail(j,PVR_JOB_TIMEOUT);
    return 1;
}
int pvr_compute_job_init(struct pvr_compute_job *j,const struct pvr_compute_job_config *c,
                         const struct pvr_compute_job_image *i,const struct pvr_compute_job_ops *ops,
                         void *ctx,struct pvr_cccb *t,uint64_t deadline)
{
    if(!range(j,sizeof(*j))||(uintptr_t)j%_Alignof(struct pvr_compute_job)||j->phase!=PVR_JOB_NEW||
       !range(i,sizeof(*i))||(uintptr_t)i%_Alignof(struct pvr_compute_job_image)||
       !range(ops,sizeof(*ops))||(uintptr_t)ops%_Alignof(struct pvr_compute_job_ops)||
       !range(t,sizeof(*t))||(uintptr_t)t%_Alignof(struct pvr_cccb)||
       !range(t->queue,sizeof(*t->queue))||(uintptr_t)t->queue%_Alignof(struct pvr_ccb)||
       !apart(j,sizeof(*j),i,sizeof(*i))||!apart(j,sizeof(*j),ops,sizeof(*ops))||
       !apart(j,sizeof(*j),t,sizeof(*t))||!apart(j,sizeof(*j),t->queue,sizeof(*t->queue))||
       !valid(c,j,sizeof(*j))||!valid(c,i,sizeof(*i))||
       !valid(c,t,sizeof(*t))||!valid(c,t->queue,sizeof(*t->queue))||
       !apart(t,sizeof(*t),t->queue,sizeof(*t->queue))||
       !apart(t,sizeof(*t),i,sizeof(*i))||!apart(t->queue,sizeof(*t->queue),i,sizeof(*i))||
       !ops->now||!ops->publish||!ops->read_fence||!ops->read_output||
       t->phase!=PVR_CCCB_IDLE||t->queue->phase!=PVR_CCB_ACTIVE||!t->epoch||
       t->epoch!=t->queue->epoch||t->serial||t->size<512||
       t->size!=c->programs->excluded[3].bytes||t->firmware_context!=c->firmware_context||
       deadline<=t->last_now||deadline<=t->queue->last_now)return 0;
    struct pvr_compute_job_image expected;image(c,&expected);
    const unsigned char *x=(const void *)i,*y=(const void *)&expected;
    for(size_t n=0;n<sizeof(expected);++n)if(x[n]!=y[n])return 0;
    struct pvr_compute_job value={.ops=*ops,.context=ctx,.transport=t,.config=c,.image=i,
        .deadline=deadline,.last_now=t->last_now,.epoch=t->epoch,.phase=PVR_JOB_PUBLISH,.sequence=1,.serial=1};
    memcpy(value.update,i->update,sizeof(value.update));
    if(value.last_now<t->queue->last_now)value.last_now=t->queue->last_now;
    *j=value;return 1;
}
int pvr_compute_job_repeat(struct pvr_compute_job *j,uint64_t deadline)
{
    if(!range(j,sizeof(*j))||(uintptr_t)j%_Alignof(struct pvr_compute_job)||
       j->phase!=PVR_JOB_COMPLETE||j->sequence==UINT32_MAX||j->serial==UINT64_MAX||
       j->transport->phase!=PVR_CCCB_IDLE||j->transport->queue->phase!=PVR_CCB_ACTIVE||
       j->transport->serial!=j->serial||j->transport->epoch!=j->epoch||
       j->transport->queue->epoch!=j->epoch||deadline<=j->last_now||
       deadline<=j->transport->last_now||deadline<=j->transport->queue->last_now)return 0;
    ++j->sequence;++j->serial;j->deadline=deadline;j->phase=PVR_JOB_PUBLISH;
    memset(j->results,0,sizeof(j->results));put32(j->update,4,j->sequence);return 1;
}
int pvr_compute_job_poll(struct pvr_compute_job *j)
{
    if(!range(j,sizeof(*j))||(uintptr_t)j%_Alignof(struct pvr_compute_job)||j->phase==PVR_JOB_NEW)return -1;
    if(j->phase==PVR_JOB_COMPLETE)return 1;
    if(j->phase==PVR_JOB_FAILED)return -1;
    if(j->epoch!=j->transport->epoch||j->epoch!=j->transport->queue->epoch||
       j->transport->queue->phase!=PVR_CCB_ACTIVE||
       (j->phase>=PVR_JOB_KICK&&j->transport->serial!=j->serial))return fail(j,PVR_JOB_TRANSPORT);
    if(clock_ok(j)!=1)return -1;
    switch(j->phase) {
    case PVR_JOB_CHECK_FENCE:
    case PVR_JOB_WAIT_FENCE: {
        uint32_t value=UINT32_MAX;
        if(j->ops.read_fence(j->context,&value)!=1)return fail(j,PVR_JOB_IO);
        if(clock_ok(j)!=1)return -1;
        if(j->phase==PVR_JOB_CHECK_FENCE) {
            if(value!=j->sequence-1)return fail(j,PVR_JOB_FENCE_VALUE);
            j->phase=PVR_JOB_SUBMIT;
        } else if(value==j->sequence)j->phase=PVR_JOB_READ_OUTPUT;
        else if(value!=j->sequence-1)return fail(j,PVR_JOB_FENCE_VALUE);
        break;
    }
    case PVR_JOB_PUBLISH:
        if(j->ops.publish(j->context,j->config,j->deadline)!=1)return fail(j,PVR_JOB_IO);
        if(clock_ok(j)!=1)return -1;
        j->phase=PVR_JOB_CHECK_FENCE;break;
    case PVR_JOB_SUBMIT: {
        struct pvr_cccb_command commands[2]={
            {PVR_CCB_MAGIC|PVR_CCCB_TASK|205u,96,j->config->job_id,j->config->job_id,j->image->compute},
            {PVR_CCB_MAGIC|213u,8,0,0,j->update}};
        if(!pvr_cccb_begin(j->transport,commands,2,NULL,0,j->deadline))return fail(j,PVR_JOB_TRANSPORT);
        if(clock_ok(j)!=1)return -1;
        j->phase=PVR_JOB_KICK;break;
    }
    case PVR_JOB_KICK: {
        enum pvr_cccb_result r=pvr_cccb_poll(j->transport);
        if(clock_ok(j)!=1)return -1;
        if(r==PVR_CCCB_OK)j->phase=PVR_JOB_WAIT_FENCE;
        else if(r!=PVR_CCCB_AGAIN)return fail(j,PVR_JOB_TRANSPORT);
        break;
    }
    case PVR_JOB_READ_OUTPUT: {
        unsigned char data[4096];
        size_t bytes=pvr_compute_job_region_bytes(j->config,PVR_JOB_OUTPUT);
        if(j->ops.read_output(j->context,data,bytes)!=1)return fail(j,PVR_JOB_IO);
        if(clock_ok(j)!=1)return -1;
        unsigned count=pvr_compute_job_result_words(j->config);
        for(unsigned n=0;n<bytes/4;++n) {
            if(n>=16&&n<16+count) {
                if(!j->config->package&&get32(data,4*n)!=j->image->expected[n-16])return fail(j,PVR_JOB_RESULT);
            } else if(get32(data,4*n)!=PVR_COMPUTE_JOB_GUARD)return fail(j,PVR_JOB_RESULT);
        }
        for(unsigned n=0;n<count;++n)j->results[n]=get32(data,64+4*n);
        j->phase=PVR_JOB_COMPLETE;return 1;
    }
    default:return fail(j,PVR_JOB_TRANSPORT);
    }
    return 0;
}
int pvr_compute_job_abort(struct pvr_compute_job *j)
{
    if(!range(j,sizeof(*j))||(uintptr_t)j%_Alignof(struct pvr_compute_job)||j->phase==PVR_JOB_NEW)return -1;
    if(j->phase==PVR_JOB_COMPLETE)return 1;
    if(j->phase==PVR_JOB_FAILED)return -1;
    return fail(j,PVR_JOB_TRANSPORT);
}
