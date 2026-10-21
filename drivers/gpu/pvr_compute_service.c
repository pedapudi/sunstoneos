#include "pvr_compute_service.h"
#include "pvr_compute_context.h"
#include "pvr_gpu_service.h"
#include "a733_gpu_mapping.h"
#include <string.h>

#define PAGE 4096u
#define OWNED (PVR_COMPUTE_SERVICE_OBJECTS + PVR_PROGRAM_REGIONS + PVR_JOB_REGIONS - 1u)
static int span(const void *p,size_t n)
{ return p && n && n<=UINTPTR_MAX-(uintptr_t)p; }
static int apart(const void *a,size_t n,const void *b,size_t m)
{ return span(a,n)&&span(b,m)&&((uintptr_t)a+n<=(uintptr_t)b||(uintptr_t)b+m<=(uintptr_t)a); }
static const struct pvr_mips_buffer *owned(const struct pvr_compute_service_config *c,unsigned i)
{
    if(i<PVR_COMPUTE_SERVICE_OBJECTS)return &c->firmware[i];
    i-=PVR_COMPUTE_SERVICE_OBJECTS;
    if(i<PVR_PROGRAM_REGIONS)return &c->programs[i];
    i-=PVR_PROGRAM_REGIONS;
    return &c->buffers[i>=PVR_JOB_UFO?i+1:i];
}
static const struct pvr_compute_program_span *logical(const struct pvr_compute_service_config *c,unsigned i)
{
    if(i<5)return &c->job->programs->excluded[i];
    if(i==5)return &c->job->buffers[PVR_JOB_UFO];
    i-=6;
    if(i<PVR_PROGRAM_REGIONS)return &c->job->programs->buffers[i];
    i-=PVR_PROGRAM_REGIONS;
    return &c->job->buffers[i>=PVR_JOB_UFO?i+1:i];
}
static const struct pvr_mips_buffer *system_buffer(const struct pvr_gpu_service_config *c,unsigned i)
{ return i<3?&c->firmware_buffers[i]:&c->resources[i-3]; }
static int pa(uint64_t p)
{ return p&&!(p&4095)&&p<(UINT64_C(1)<<36)&&!(p>=0x1fc00000&&p<0x1fc04000); }
static int mapped_access(const struct pvr_mips_buffer *b,unsigned page,unsigned attr,int write)
{ return a733_gpu_page_mapped((uintptr_t)b->storage.cpu+(size_t)page*PAGE,b->pages[page],attr,write); }
static int mapped(const struct pvr_mips_buffer *b,unsigned page,unsigned attr)
{ return mapped_access(b,page,attr,1); }
static int descriptor(const struct pvr_mips_buffer *b)
{
    return span(b->storage.cpu,b->storage.bytes)&&!((uintptr_t)b->storage.cpu&4095)&&
        b->page_count&&b->page_count<=64&&b->storage.bytes==(size_t)b->page_count*PAGE&&
        span(b->pages,(size_t)b->page_count*8)&&!((uintptr_t)b->pages&7);
}
static int private_range(const struct pvr_compute_service *s,const struct pvr_gpu_service *g,
    const struct pvr_gpu_service_config *gc,const struct pvr_compute_service_config *c,const void *p,size_t n)
{
    return apart(p,n,s,sizeof(*s))&&apart(p,n,g,sizeof(*g))&&apart(p,n,gc,sizeof(*gc))&&
        apart(p,n,c,sizeof(*c))&&apart(p,n,c->job,sizeof(*c->job))&&apart(p,n,c->image,sizeof(*c->image))&&
        apart(p,n,c->job->programs,sizeof(*c->job->programs))&&
        apart(p,n,c->job->program_result,sizeof(*c->job->program_result))&&
        apart(p,n,c->vm,sizeof(*c->vm))&&apart(p,n,c->maps,c->map_count*sizeof(*c->maps))&&
        apart(p,n,gc->firmware,gc->firmware_bytes)&&
        apart(p,n,c->vm->tables.physical_pages,(size_t)c->vm->tables.page_count*8);
}
static int readonly(unsigned i)
{
    if(i<6)return 0;
    i-=6;if(i<PVR_PROGRAM_REGIONS)return i<PVR_PROGRAM_SHARED_A;
    i-=PVR_PROGRAM_REGIONS;if(i>=PVR_JOB_UFO)++i;
    return i!=PVR_JOB_OUTPUT;
}
static int map_matches(const struct pvr_compute_service_config *c,unsigned index,unsigned page)
{
    const struct pvr_mips_buffer *b=owned(c,index);
    const struct pvr_compute_program_span *l=logical(c,index);
    uint64_t address=l->gpu+(size_t)page*PAGE;unsigned found=0;
    for(unsigned i=0;i<c->map_count;++i) {
        const struct pvr_gpu_mapping *m=&c->maps[i];
        if(m->address==address&&m->physical==b->pages[page]&&m->bytes==PAGE&&m->page_shift==12&&
           m->flags==(readonly(index)?PVR_GPU_VM_READ_ONLY:0))++found;
    }
    return found==1;
}
struct view {const void *p;size_t n;};
static int metadata(struct pvr_compute_service *s,struct pvr_gpu_service *g,
    struct pvr_gpu_service_config *gc,const struct pvr_compute_service_config *c)
{
    enum { VIEW_CAPACITY=10+OWNED+3+PVR_FW_RESOURCE_COUNT+1+PVR_FREE_LISTS };
    _Static_assert(VIEW_CAPACITY<=128,"bounded metadata admission");
    struct view views[VIEW_CAPACITY];unsigned count=0;
#define ADD(p,n) do{views[count++]=(struct view){(p),(n)};}while(0)
    ADD(gc,sizeof(*gc));ADD(c,sizeof(*c));ADD(c->job,sizeof(*c->job));ADD(c->image,sizeof(*c->image));
    ADD(c->job->programs,sizeof(*c->job->programs));ADD(c->job->program_result,sizeof(*c->job->program_result));
    ADD(c->maps,c->map_count*sizeof(*c->maps));ADD(c->vm->tables.physical_pages,c->vm->tables.page_count*8);
    ADD(gc->firmware,gc->firmware_bytes);
    if(c->job->package)ADD(c->job->package,sizeof(*c->job->package));
    for(unsigned i=0;i<OWNED;++i){const struct pvr_mips_buffer *b=owned(c,i);ADD(b->pages,b->page_count*8);}
    for(unsigned i=0;i<3+PVR_FW_RESOURCE_COUNT;++i){const struct pvr_mips_buffer *b=system_buffer(gc,i);ADD(b->pages,b->page_count*8);}
    if(gc->free_lists.count) {
        if(gc->free_lists.count>PVR_FREE_LISTS||!span(gc->free_lists.lists,gc->free_lists.count*sizeof(*gc->free_lists.lists)))return 0;
        ADD(gc->free_lists.lists,gc->free_lists.count*sizeof(*gc->free_lists.lists));
        for(unsigned i=0;i<gc->free_lists.count;++i) {
            const struct pvr_free_list_config *f=&gc->free_lists.lists[i];
            if(!f->maximum_pages||f->maximum_pages>PVR_FREE_LIST_MAX_PAGES)return 0;
            ADD(f->pages,(size_t)f->maximum_pages*sizeof(*f->pages));
        }
    }
#undef ADD
    for(unsigned i=0;i<count;++i) {
        const struct view *v=&views[i];
        if(!apart(v->p,v->n,s,sizeof(*s))||!apart(v->p,v->n,g,sizeof(*g))||
           !apart(v->p,v->n,c->vm,sizeof(*c->vm))||
           !apart(v->p,v->n,c->vm->tables.cpu,c->vm->tables.bytes))return 0;
        for(unsigned j=0;j<OWNED;++j) {
            const struct pvr_mips_buffer *b=owned(c,j);
            if(!apart(v->p,v->n,b->storage.cpu,b->storage.bytes))return 0;
        }
        for(unsigned j=0;j<3+PVR_FW_RESOURCE_COUNT;++j) {
            const struct pvr_mips_buffer *b=system_buffer(gc,j);
            if(!apart(v->p,v->n,b->storage.cpu,b->storage.bytes))return 0;
        }
    }
    if(!apart(c->vm,sizeof(*c->vm),s,sizeof(*s))||!apart(c->vm,sizeof(*c->vm),g,sizeof(*g))||
       !private_range(s,g,gc,c,c->vm->tables.cpu,c->vm->tables.bytes))return 0;
    for(unsigned j=0;j<3+PVR_FW_RESOURCE_COUNT;++j) {
        const struct pvr_mips_buffer *b=system_buffer(gc,j);
        if(!apart(b->storage.cpu,b->storage.bytes,s,sizeof(*s))||
           !apart(b->storage.cpu,b->storage.bytes,c->vm,sizeof(*c->vm))||
           !apart(b->storage.cpu,b->storage.bytes,c->vm->tables.cpu,c->vm->tables.bytes))return 0;
        for(unsigned k=0;k<b->page_count;++k)
            for(unsigned i=0;i<c->vm->tables.page_count;++i)if(b->pages[k]==c->vm->tables.physical_pages[i])return 0;
    }
    return 1;
}
static int validate(struct pvr_compute_service *s,struct pvr_gpu_service *g,
    struct pvr_gpu_service_config *gc,const struct pvr_compute_service_config *c,int rebuilding)
{
    if(!span(s,sizeof(*s))||!span(g,sizeof(*g))||!span(gc,sizeof(*gc))||!span(c,sizeof(*c))||
       (uintptr_t)s%_Alignof(struct pvr_compute_service)||(uintptr_t)c%_Alignof(struct pvr_compute_service_config)||
       (uintptr_t)g%_Alignof(struct pvr_gpu_service)||(uintptr_t)gc%_Alignof(struct pvr_gpu_service_config)||
       s->phase||(g->initialized && (!rebuilding || g->phase!=PVR_GPU_SERVICE_RECONSTRUCTING))||gc->compute||!apart(s,sizeof(*s),g,sizeof(*g))||
       !apart(s,sizeof(*s),gc,sizeof(*gc))||!apart(s,sizeof(*s),c,sizeof(*c))||
       !span(c->job,sizeof(*c->job))||!span(c->image,sizeof(*c->image))||
       (uintptr_t)c->job%_Alignof(struct pvr_compute_job_config)||
       (uintptr_t)c->image%_Alignof(struct pvr_compute_job_image)||
       !span(c->vm,sizeof(*c->vm))||(uintptr_t)c->vm%_Alignof(struct a733_gpu_vm_memory)||!c->map_count||c->map_count>PVR_COMPUTE_SERVICE_MAPS||
       !span(c->maps,c->map_count*sizeof(*c->maps))||
       (uintptr_t)c->maps%_Alignof(struct pvr_gpu_mapping)||
       !span(c->job->programs,sizeof(*c->job->programs))||
       (uintptr_t)c->job->programs%_Alignof(struct pvr_compute_program_config)||
       !span(c->job->program_result,sizeof(*c->job->program_result))||
       (uintptr_t)c->job->program_result%_Alignof(struct pvr_compute_program_result)||!c->job->admit||
       c->vm->phase!=A733_GPU_VM_MEMORY_PREPARED||!c->vm->tables.page_count||
       c->vm->tables.page_count>PVR_COMPUTE_SERVICE_TABLE_PAGES||
       !span(c->vm->tables.cpu,c->vm->tables.bytes)||
       c->vm->tables.bytes!=(size_t)c->vm->tables.page_count*PAGE||
       !span(c->vm->tables.physical_pages,c->vm->tables.page_count*8)||
       !span(gc->firmware,gc->firmware_bytes)||!aarch64_device_cache_stride())return 0;
    const struct pvr_compute_program_config *p=c->job->programs;
    if(p->excluded_count<5||p->excluded_count>PVR_COMPUTE_PROGRAM_EXCLUSIONS||
       c->vm->context_cpu!=c->firmware[4].storage.cpu||
       c->job->firmware_context!=c->firmware_address[0]||c->job->firmware_ufo!=c->firmware_address[5])return 0;
    if(c->buffers[PVR_JOB_UFO].storage.cpu!=c->firmware[5].storage.cpu||
       c->buffers[PVR_JOB_UFO].storage.bytes!=c->firmware[5].storage.bytes||
       c->buffers[PVR_JOB_UFO].pages!=c->firmware[5].pages||
       c->buffers[PVR_JOB_UFO].page_count!=c->firmware[5].page_count)return 0;
    for(unsigned j=0;j<3+PVR_FW_RESOURCE_COUNT;++j)
        if(!descriptor(system_buffer(gc,j)))return 0;
    struct pvr_gpu_vm_requirements required;
    if(!pvr_gpu_vm_require(c->maps,c->map_count,&required)||required.table_bytes!=c->vm->tables.bytes)return 0;
    for(unsigned i=0;i<OWNED;++i)if(!descriptor(owned(c,i)))return 0;
    if(!metadata(s,g,gc,c))return 0;
    unsigned data_pages=0;
    for(unsigned i=0;i<OWNED;++i) {
        const struct pvr_mips_buffer *b=owned(c,i);const struct pvr_compute_program_span *l=logical(c,i);
        if(!descriptor(b)||l->cpu!=b->storage.cpu||!l->bytes||l->bytes>b->storage.bytes||
           b->storage.bytes!=((l->bytes+4095)&~(size_t)4095)||
           !private_range(s,g,gc,c,b->storage.cpu,b->storage.bytes)||
           !apart(b->storage.cpu,b->storage.bytes,c->vm->tables.cpu,c->vm->tables.bytes))return 0;
        if(i<6) {
            uint32_t address=c->firmware_address[i];
            if((address&4095)||address<0xc0000000u||address>=0xc1000000u||
               b->storage.bytes>0xc1000000u-address||(i<5&&l->gpu!=address))return 0;
            for(unsigned j=0;j<i;++j)
                if(address<c->firmware_address[j]+c->firmware[j].storage.bytes&&
                   c->firmware_address[j]<address+b->storage.bytes)return 0;
        } else if(l->gpu&4095)return 0;
        for(unsigned j=0;j<OWNED;++j) {
            const struct pvr_mips_buffer *a=owned(c,j);
            if(!descriptor(a)||!apart(b->storage.cpu,b->storage.bytes,a->pages,a->page_count*8)||
               (j<i&&!apart(b->storage.cpu,b->storage.bytes,a->storage.cpu,a->storage.bytes)))return 0;
        }
        for(unsigned j=0;j<3+PVR_FW_RESOURCE_COUNT;++j) {
            const struct pvr_mips_buffer *a=system_buffer(gc,j);
            if(!apart(b->storage.cpu,b->storage.bytes,a->storage.cpu,a->storage.bytes)||
               !apart(b->storage.cpu,b->storage.bytes,a->pages,a->page_count*8))return 0;
        }
        for(unsigned page=0;page<b->page_count;++page) {
            uint64_t physical=b->pages[page];if(!pa(physical)||!mapped(b,page,i<6?0x44:0xff))return 0;
            for(unsigned j=0;j<=i;++j) {
                const struct pvr_mips_buffer *a=owned(c,j);
                for(unsigned k=0;k<(i==j?page:a->page_count);++k)if(physical==a->pages[k])return 0;
            }
            for(unsigned j=0;j<3+PVR_FW_RESOURCE_COUNT;++j) {
                const struct pvr_mips_buffer *a=system_buffer(gc,j);
                for(unsigned k=0;k<a->page_count;++k)if(physical==a->pages[k])return 0;
            }
            for(unsigned j=0;j<c->vm->tables.page_count;++j)
                if(physical==c->vm->tables.physical_pages[j])return 0;
            if(i>=6){if(!map_matches(c,i,page))return 0;++data_pages;}
        }
    }
    if(data_pages!=c->map_count||c->vm->context_physical!=c->firmware[4].pages[0])return 0;
    /* The admitted builder proves exact table bytes and excludes unlisted
     * platform reservations, including opaque callback contexts. */
    return c->job->admit(c->job->admission_context,c->job)==1;
}
void pvr_compute_service_abort(struct pvr_compute_service *s,unsigned error)
{
    if(!s||s->phase==PVR_COMPUTE_SERVICE_FAILED)return;
    s->phase=PVR_COMPUTE_SERVICE_FAILED;s->error=error;
    if(s->client.phase!=PVR_CCCB_NEW)(void)pvr_cccb_abort(&s->client);
}
static int fail(struct pvr_compute_service *s,unsigned error)
{ pvr_compute_service_abort(s,error);return -1; }
/* The service clock orders this sample after every queue, controller, client
 * ring and job sample, because each of them also samples through it. */
static int clock_now(void *ctx,uint64_t *out)
{
    struct pvr_compute_service *s=ctx;uint64_t now;
    if(s->phase==PVR_COMPUTE_SERVICE_FAILED||pvr_gpu_service_now(s->service,&now)!=1)return 0;
    s->last_now=now;*out=now;return 1;
}
static int timely(struct pvr_compute_service *s)
{ uint64_t now;return clock_now(s,&now)&&now<s->deadline; }
static int bind(struct pvr_compute_service *s,struct pvr_gpu_service *g,
    struct pvr_gpu_service_config *gc,const struct pvr_compute_service_config *c,int rebuilding)
{
    if(!validate(s,g,gc,c,rebuilding))return 0;
    struct pvr_compute_service candidate={.config=c,.service=g,.phase=PVR_COMPUTE_SERVICE_BOUND};
    for(unsigned i=0;i<6;++i)candidate.objects[i]=(struct pvr_mips_object){
        c->firmware[i].pages,c->firmware[i].page_count,c->firmware_address[i],PVR_MIPS_UNCACHED};
    *s=candidate;gc->compute=s;return 1;
}
int pvr_compute_service_bind(struct pvr_compute_service *s,struct pvr_gpu_service *g,
    struct pvr_gpu_service_config *gc,const struct pvr_compute_service_config *c)
{ return bind(s,g,gc,c,0); }
int pvr_compute_service_rebind(struct pvr_compute_service *s,struct pvr_gpu_service *g,
    struct pvr_gpu_service_config *gc,const struct pvr_compute_service_config *c)
{ return g && g->phase==PVR_GPU_SERVICE_RECONSTRUCTING && bind(s,g,gc,c,1); }
int pvr_compute_service_prepare(struct pvr_compute_service *s,uint64_t deadline)
{
    if(!s||s->phase!=PVR_COMPUTE_SERVICE_BOUND||!s->service->epoch)return 0;
    s->deadline=deadline;s->epoch=s->service->epoch;
    if(!timely(s))return fail(s,PVR_COMPUTE_SERVICE_CLOCK);
    if(s->config->job->admit(s->config->job->admission_context,s->config->job)!=1)
        return fail(s,PVR_COMPUTE_SERVICE_ADMISSION);
    if(!timely(s))return fail(s,PVR_COMPUTE_SERVICE_CLOCK);
    s->buffer=0;s->page=0;s->phase=PVR_COMPUTE_SERVICE_DATA;return 1;
}
/* Reads need only the read translation of each touched page; writes check both. */
static int io_span(struct pvr_compute_service *s,unsigned object,uint32_t off,size_t n,int write)
{
    const struct pvr_mips_buffer *b=&s->config->firmware[object];
    const struct pvr_compute_program_span *l=logical(s->config,object);
    if(!n||off>l->bytes||n>l->bytes-off)return 0;
    for(unsigned i=off/PAGE;i<=(off+n-1)/PAGE;++i)if(!mapped_access(b,i,0x44,write))return 0;
    return 1;
}
static int read_client(void *ctx,enum pvr_cccb_region r,uint32_t off,void *out,size_t n)
{
    struct pvr_compute_service *s=ctx;
    if(r!=PVR_CCCB_CONTROL||n!=4||(off&3)||!io_span(s,2,off,n,0))return 0;
    uint32_t value=*(volatile uint32_t *)((uintptr_t)s->config->firmware[2].storage.cpu+off);
    __asm__ volatile("dsb sy":::"memory");memcpy(out,&value,4);return 1;
}
static int write_client(void *ctx,enum pvr_cccb_region r,uint32_t off,const void *in,size_t n)
{
    struct pvr_compute_service *s=ctx;
    unsigned id=r==PVR_CCCB_CONTROL?2:3;
    if((unsigned)r>PVR_CCCB_COMMANDS||!io_span(s,id,off,n,1)||
       (r==PVR_CCCB_CONTROL&&(off||n!=32||
        (s->phase!=PVR_COMPUTE_SERVICE_FORMAT&&
         !(s->phase==PVR_COMPUTE_SERVICE_CLEANUP&&s->client.phase==PVR_CCCB_FORMAT))))||
       (r==PVR_CCCB_COMMANDS&&s->phase!=PVR_COMPUTE_SERVICE_RUNNING))return 0;
    memcpy((unsigned char *)s->config->firmware[id].storage.cpu+off,in,n);
    __asm__ volatile("dsb sy":::"memory");return 1;
}
static int barrier(void *ctx,enum pvr_ccb_barrier kind)
{ (void)ctx;if((unsigned)kind>PVR_CCB_DEVICE_COMPLETE)return 0;__asm__ volatile("dsb sy":::"memory");return 1; }
static int read_fence(void *,uint32_t *);
static int published(void *ctx,const struct pvr_compute_job_config *job,uint64_t deadline)
{
    struct pvr_compute_service *s=ctx;
    if(job!=s->config->job||deadline!=s->deadline||s->config->vm->phase!=A733_GPU_VM_MEMORY_PUBLISHED||
       s->phase!=PVR_COMPUTE_SERVICE_RUNNING||!io_span(s,5,0,4,0))return 0;
    uint32_t previous;
    if(!read_fence(s,&previous)||previous!=s->job.sequence-1)return 0;
    const struct pvr_mips_buffer *output=&s->config->buffers[PVR_JOB_OUTPUT];
    if(!mapped(output,0,0xff))return 0;
    /* UPDATE follows the finished CDM task. The preceding acquired completion
     * owns this exclusive output again; preserve the immutable program/input. */
    uint32_t *words=output->storage.cpu;
    for(unsigned i=0;i<pvr_compute_job_region_bytes(job,PVR_JOB_OUTPUT)/4;++i)words[i]=PVR_COMPUTE_JOB_GUARD;
    /* Always use the complement of each expected result as its poison. */
    for(unsigned i=0;i<pvr_compute_job_result_words(job);++i)
        words[16+i]=job->package?UINT32_C(0xa5a5a5a5):~s->config->image->expected[i];
    if(!aarch64_device_cache(AARCH64_CACHE_CLEAN_INVALIDATE,(uintptr_t)output->storage.cpu,PAGE))return 0;
    __asm__ volatile("dsb sy":::"memory");return 1;
}
static int read_fence(void *ctx,uint32_t *out)
{
    struct pvr_compute_service *s=ctx;if(!io_span(s,5,0,4,0))return 0;
    uint32_t value=*(volatile uint32_t *)s->config->firmware[5].storage.cpu;
    __asm__ volatile("dsb sy":::"memory");*out=value;return 1;
}
static int read_output(void *ctx,void *out,size_t bytes)
{
    struct pvr_compute_service *s=ctx;const struct pvr_mips_buffer *b=&s->config->buffers[PVR_JOB_OUTPUT];
    if(bytes!=pvr_compute_job_region_bytes(s->config->job,PVR_JOB_OUTPUT)||!mapped(b,0,0xff)||
       !aarch64_device_cache(AARCH64_CACHE_INVALIDATE,(uintptr_t)b->storage.cpu,bytes))return 0;
    const volatile unsigned char *input=b->storage.cpu;unsigned char *output=out;
    for(size_t i=0;i<bytes;++i)output[i]=input[i];
    __asm__ volatile("dsb sy":::"memory");return 1;
}
int pvr_compute_service_admit_mode(struct pvr_compute_service *s)
{
    if(!s||(s->phase!=PVR_COMPUTE_SERVICE_READY&&s->phase!=PVR_COMPUTE_SERVICE_COMPLETE)||s->epoch!=s->service->epoch)return 0;
    if(!timely(s))return fail(s,PVR_COMPUTE_SERVICE_CLOCK);
    if(!s->service->config.compute_mode||s->service->config.compute_mode(s->service->config.context)!=1)
        return fail(s,PVR_COMPUTE_SERVICE_ADMISSION);
    if(!timely(s))return fail(s,PVR_COMPUTE_SERVICE_CLOCK);
    return 1;
}
int pvr_compute_service_begin(struct pvr_compute_service *s,uint64_t deadline)
{
    if(!s||(s->phase!=PVR_COMPUTE_SERVICE_READY&&s->phase!=PVR_COMPUTE_SERVICE_COMPLETE)||s->service->phase!=PVR_GPU_SERVICE_READY||
       s->epoch!=s->service->epoch)return 0;
    s->deadline=deadline;
    if(pvr_compute_service_admit_mode(s)!=1)return 0;
    if(s->config->job->admit(s->config->job->admission_context,s->config->job)!=1)
        return fail(s,PVR_COMPUTE_SERVICE_ADMISSION);
    if(!timely(s))return fail(s,PVR_COMPUTE_SERVICE_CLOCK);
    if(s->phase==PVR_COMPUTE_SERVICE_COMPLETE) {
        if(!pvr_compute_job_repeat(&s->job,deadline))return fail(s,PVR_COMPUTE_SERVICE_JOB);
        s->phase=PVR_COMPUTE_SERVICE_RUNNING;return 1;
    }
    const struct pvr_cccb_ops ops={clock_now,read_client,write_client,barrier};
    if(!pvr_cccb_init(&s->client,&ops,s,&s->service->queue,
        (uint32_t)s->config->job->programs->excluded[3].bytes,s->config->firmware_address[0],deadline))
        return fail(s,PVR_COMPUTE_SERVICE_QUEUE);
    s->phase=PVR_COMPUTE_SERVICE_FORMAT;return 1;
}
int pvr_compute_service_replace(struct pvr_compute_service *s,uint32_t sequence,
    const void *input,size_t bytes,uint64_t deadline)
{
    if(!s||!input||s->phase!=PVR_COMPUTE_SERVICE_COMPLETE||
       s->service->phase!=PVR_GPU_SERVICE_READY||s->epoch!=s->service->epoch||
       !s->config->job->package||bytes!=s->config->job->package->input_bytes||
       bytes<4||bytes>PVR_COMPUTE_PACKAGE_INPUT||sequence!=s->job.sequence||
       !pvr_compute_job_can_repeat(&s->job,deadline))return 0;
    if(!pvr_compute_package_input_valid(s->config->job->package,input,bytes))return 0;
    s->deadline=deadline;
    /* Admission reads the actual native owner/epoch/serial and acquired NC
     * timeline. Recheck at consumption, before modifying any device backing. */
    if(pvr_compute_service_admit_mode(s)!=1)return -1;
    if(s->config->job->admit(s->config->job->admission_context,s->config->job)!=1)
        return fail(s,PVR_COMPUTE_SERVICE_ADMISSION),-1;
    uint32_t completed;
    if(!read_fence(s,&completed)||completed!=sequence||!timely(s))
        return fail(s,PVR_COMPUTE_SERVICE_ADMISSION),-1;
    const struct pvr_mips_buffer *b=&s->config->buffers[PVR_JOB_INPUT];
    if(!mapped(b,0,0xff))return fail(s,PVR_COMPUTE_SERVICE_MAPPING),-1;
    memcpy(b->storage.cpu,input,bytes);
    if(!aarch64_device_cache(AARCH64_CACHE_CLEAN_INVALIDATE,(uintptr_t)b->storage.cpu,PAGE))
        return fail(s,PVR_COMPUTE_SERVICE_PUBLICATION),-1;
    __asm__ volatile("dsb sy":::"memory");
    if(!timely(s)||!pvr_compute_job_repeat(&s->job,deadline))
        return fail(s,PVR_COMPUTE_SERVICE_JOB),-1;
    s->phase=PVR_COMPUTE_SERVICE_RUNNING;return 1;
}
int pvr_compute_service_cleanup_begin(struct pvr_compute_service *s,uint64_t deadline)
{
    if(!s||!s->service||!s->config||s->service->phase!=PVR_GPU_SERVICE_READY||
       s->service->config.compute!=s||
       ((s->service->config.graphics||s->service->config.render||s->service->config.free_lists.count)&&
        !s->service->graphics_cleanup_count)||s->service->owner>=PVR_GPU_SERVICE_CLIENTS||
       (s->phase!=PVR_COMPUTE_SERVICE_READY&&s->phase!=PVR_COMPUTE_SERVICE_COMPLETE)||
       atomic_load_explicit(&s->input_state,memory_order_acquire))return 0;
    s->deadline=deadline;
    if(deadline!=s->service->deadline||pvr_compute_service_admit_mode(s)!=1||
       s->config->job->admit(s->config->job->admission_context,s->config->job)!=1||
       !timely(s))return fail(s,PVR_COMPUTE_SERVICE_ADMISSION);
    uint32_t expected=0,completed;
    if(s->phase==PVR_COMPUTE_SERVICE_COMPLETE) {
        if(s->job.phase!=PVR_JOB_COMPLETE||!s->job.sequence||s->client.phase!=PVR_CCCB_IDLE||
           s->job.transport!=&s->client||s->client.queue!=&s->service->queue||
           s->job.epoch!=s->epoch||s->client.epoch!=s->epoch||s->client.serial!=s->job.serial)
            return fail(s,PVR_COMPUTE_SERVICE_ADMISSION);
        expected=s->job.sequence;
    } else if(s->job.phase!=PVR_JOB_NEW||s->client.phase!=PVR_CCCB_NEW)
        return fail(s,PVR_COMPUTE_SERVICE_ADMISSION);
    if(s->service->queue.phase!=PVR_CCB_ACTIVE||s->service->queue.epoch!=s->epoch||
       s->config->job->firmware_context!=s->config->firmware_address[0]||
       !io_span(s,0,0,PVR_COMPUTE_CONTEXT_BYTES,1)||!read_fence(s,&completed)||completed!=expected||!timely(s))
        return fail(s,PVR_COMPUTE_SERVICE_ADMISSION);
    if(s->client.phase==PVR_CCCB_NEW) {
        const struct pvr_cccb_ops ops={clock_now,read_client,write_client,barrier};
        if(!pvr_cccb_init(&s->client,&ops,s,&s->service->queue,
            (uint32_t)s->config->job->programs->excluded[3].bytes,s->config->firmware_address[0],deadline))
            return fail(s,PVR_COMPUTE_SERVICE_QUEUE);
    }
    s->cleanup_context=s->config->firmware_address[0];s->cleanup_status=0;
    memset(&s->cleanup_token,0,sizeof(s->cleanup_token));
    s->phase=PVR_COMPUTE_SERVICE_CLEANUP;return 1;
}
int pvr_compute_service_cleanup_poll(struct pvr_compute_service *s)
{
    if(!s||s->phase!=PVR_COMPUTE_SERVICE_CLEANUP)return -1;
    if((s->service->phase!=PVR_GPU_SERVICE_COMPUTE_CLEANUP&&
        s->service->phase!=PVR_GPU_SERVICE_GRAPHICS_CLEANUP)||s->epoch!=s->service->epoch||
       s->epoch!=s->service->queue.epoch||s->cleanup_context!=s->config->firmware_address[0]||
       !timely(s))return fail(s,PVR_COMPUTE_SERVICE_CLEANUP_ERROR);
    if(s->client.phase==PVR_CCCB_FORMAT) {
        /* An unused context has not reached its first KICK. Publish the same
         * initialized CCCB control that pinned pvr_queue_create supplies. */
        if(pvr_cccb_poll(&s->client)!=PVR_CCCB_OK)return fail(s,PVR_COMPUTE_SERVICE_QUEUE);
        return 0;
    }
    if(s->client.phase!=PVR_CCCB_IDLE)return fail(s,PVR_COMPUTE_SERVICE_QUEUE);
    if(!s->cleanup_token.serial) {
        /* Pinned pvr_fw_structure_cleanup: KCCB106, flags0, cleanup type0
         * (FWCOMMONCONTEXT), then its exact firmware address. */
        uint32_t command[PVR_CCB_COMMAND_BYTES/4]={PVR_CCB_MAGIC|106u,0,0,s->cleanup_context};
        enum pvr_ccb_result r=pvr_ccb_submit(&s->service->queue,command,s->last_now,s->deadline,&s->cleanup_token);
        if(r==PVR_CCB_AGAIN)return 0;
        if(r!=PVR_CCB_OK)return fail(s,PVR_COMPUTE_SERVICE_CLEANUP_ERROR);
        return 0;
    }
    enum pvr_ccb_result r=pvr_ccb_collect(&s->service->queue,s->cleanup_token,&s->cleanup_status);
    if(r==PVR_CCB_AGAIN)return 0;
    /* BUSY and firmware POLL_FAILURE never authorize disposal. */
    if(r!=PVR_CCB_OK||s->cleanup_status!=1||!timely(s))return fail(s,PVR_COMPUTE_SERVICE_CLEANUP_ERROR);
    s->phase=PVR_COMPUTE_SERVICE_CLEANED;return 1;
}
int pvr_compute_service_poll(struct pvr_compute_service *s)
{
    if(!s||s->phase==PVR_COMPUTE_SERVICE_FAILED)return -1;
    if(s->phase==PVR_COMPUTE_SERVICE_READY||s->phase==PVR_COMPUTE_SERVICE_COMPLETE)return 1;
    if(s->phase<PVR_COMPUTE_SERVICE_DATA||!timely(s)||s->epoch!=s->service->epoch)
        return fail(s,PVR_COMPUTE_SERVICE_CLOCK);
    const struct pvr_compute_service_config *c=s->config;
    switch(s->phase) {
    case PVR_COMPUTE_SERVICE_DATA: {
        const struct pvr_mips_buffer *b=owned(c,s->buffer);
        if(!mapped(b,s->page,s->buffer<6?0x44:0xff)||
           (s->buffer>=6&&!aarch64_device_cache(AARCH64_CACHE_CLEAN_INVALIDATE,
              (uintptr_t)b->storage.cpu+(size_t)s->page*PAGE,PAGE)))return fail(s,PVR_COMPUTE_SERVICE_MAPPING);
        if(++s->page==b->page_count){s->page=0;++s->buffer;}
        if(s->buffer==OWNED) {
            if(a733_gpu_vm_memory_publish_begin(c->vm)!=A733_GPU_VM_MEMORY_AGAIN)
                return fail(s,PVR_COMPUTE_SERVICE_PUBLICATION);
            s->phase=PVR_COMPUTE_SERVICE_TABLES;
        }
        break;
    }
    case PVR_COMPUTE_SERVICE_TABLES: {
        enum a733_gpu_vm_memory_result r=a733_gpu_vm_memory_poll(c->vm);
        if(r==A733_GPU_VM_MEMORY_OK)s->phase=PVR_COMPUTE_SERVICE_READY;
        else if(r!=A733_GPU_VM_MEMORY_AGAIN)return fail(s,PVR_COMPUTE_SERVICE_PUBLICATION);
        break;
    }
    case PVR_COMPUTE_SERVICE_FORMAT: {
        enum pvr_cccb_result r=pvr_cccb_poll(&s->client);
        if(r==PVR_CCCB_OK) {
            const struct pvr_compute_job_ops ops={clock_now,published,read_fence,read_output};
            if(!pvr_compute_job_init(&s->job,c->job,c->image,&ops,s,&s->client,s->deadline))
                return fail(s,PVR_COMPUTE_SERVICE_JOB);
            s->phase=PVR_COMPUTE_SERVICE_RUNNING;
        } else if(r!=PVR_CCCB_AGAIN)return fail(s,PVR_COMPUTE_SERVICE_QUEUE);
        break;
    }
    case PVR_COMPUTE_SERVICE_RUNNING: {
        int r=pvr_compute_job_poll(&s->job);
        if(r==1)s->phase=PVR_COMPUTE_SERVICE_COMPLETE;
        else if(r<0)return fail(s,PVR_COMPUTE_SERVICE_JOB);
        break;
    }
    default:return fail(s,PVR_COMPUTE_SERVICE_LIFECYCLE);
    }
    if(!timely(s))return fail(s,PVR_COMPUTE_SERVICE_CLOCK);
    return s->phase==PVR_COMPUTE_SERVICE_READY||s->phase==PVR_COMPUTE_SERVICE_COMPLETE;
}
int pvr_compute_service_event(struct pvr_compute_service *s,const unsigned char command[88])
{
    if(!s||s->phase==PVR_COMPUTE_SERVICE_FAILED)return 0;
    struct pvr_gpu_event event;
    if(pvr_gpu_event_decode(command,88,&event)!=PVR_GPU_EVENT_DECODE_OK||event.kind==PVR_GPU_EVENT_RESET) {
        pvr_compute_service_abort(s,PVR_COMPUTE_SERVICE_RESET);return 0;
    }
    return 1;
}
