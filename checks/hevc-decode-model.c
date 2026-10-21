/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native HEVC parser, storage, controller and decoder binding on the host.
 * Reuses the decoder resource model; the preserved JPEG cases do not run.
 * Register values at each decode trigger go to a JSON trace that
 * checks/hevc-decode-model.py compares with independent references. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#define main preserved_jpeg_cases_not_executed
#include "a733-video-platform.c"
#undef main
#pragma clang diagnostic pop
#include "a733_video_hevc.h"
#include "storage.h"
#include "a733_storage_digest.h"
#define DEADLINE 100000u
static _Alignas(16) uint8_t workspace[A733_HEVC_PARSER_BYTES];
struct hevc_model {
    struct model common;
    unsigned variant,picture,flush_reads,decoding,busy,copies,writes_at_copied;
    uint32_t status,flushed,header_bits[2],sram[1024],sram_offset;
    uint32_t image[2][1024],frames[2][1024];unsigned triggered[2],events_at_trigger[2];
};
static int admit_hevc(void *v,const struct a733_video_platform_config *c,
 const struct a733_hevc_job *j,uint64_t d,uint64_t *t)
{assert(j&&j->buffers[HEVC_NEIGHBOR].bytes==A733_HEVC_NEIGHBOR_SLOT);return admit(v,c,NULL,d,t);}
static int memory_hevc(void *v,uint64_t t,const struct a733_hevc_job *j,uint64_t d)
{struct hevc_model *m=v;(void)d;assert(t==42&&j&&!m->common.lock);++m->common.memory;return 1;}
static int copied_hevc(void *v,uint64_t t,unsigned picture,uint64_t d)
{
    struct hevc_model *m=v;assert(t==42&&d==DEADLINE&&!m->common.lock&&picture==m->picture);
    /* Completion has been acknowledged and the engine interrupt disabled. */
    assert(m->triggered[picture]&&!m->decoding&&!(m->status&7)&&m->common.ve[0x530/4]==0x80000000);
    ++m->copies;return m->variant==6?-1:(m->copies&1)==0;
}
static int resource_read(void *v,uintptr_t address,uint32_t *out)
{if(address==0x07090160){*out=0x10000002;return 1;}return read_any(v,address,out);}
static int read_hevc(void *v,uintptr_t address,uint32_t *out)
{
    struct hevc_model *m=v;
    if(address==0x07090160){*out=0x10000002;return 1;}
    if(address==VE+0x538){
        assert(m->common.lock);
        if(m->flush_reads){--m->flush_reads;*out=m->variant==7?2:0x100;return 1;}
        if(m->decoding&&m->busy){--m->busy;*out=m->status|(1u<<17);
            if(m->variant==4)m->common.now=DEADLINE;return 1;}
        if(m->decoding){m->decoding=0;m->status|=m->variant==1?3:m->variant==2?5:1;}
        *out=m->status|(m->variant==8&&m->picture?(1u<<20):0);return 1;
    }
    if(address==VE+0x53c){assert(m->common.lock&&m->triggered[m->picture]);*out=m->variant==3?79:80;return 1;}
    return read_reg(v,address,out);
}
static int write_hevc(void *v,uintptr_t address,unsigned width,uint32_t value)
{
    struct hevc_model *m=v;
    assert(width==4);
    if(address==VE+0x534){
        assert(!m->decoding&&!m->flush_reads);
        if(value==7)m->flushed=0;
        else if((value&255)==3){assert(value>>8&&value>>8<=32);m->flushed+=value>>8;m->flush_reads=1;}
        else{assert(value==8&&m->flushed==m->header_bits[m->picture]&&!m->triggered[m->picture]);
            m->triggered[m->picture]=1;m->decoding=1;m->busy=2;m->events_at_trigger[m->picture]=m->common.writes;
            memcpy(m->image[m->picture],m->common.ve,sizeof(m->common.ve));memcpy(m->frames[m->picture],m->sram,sizeof(m->sram));}
    }
    if(address==VE+0x538){m->status&=~value;}
    if(address==VE+0x5e0)m->sram_offset=value/4;
    if(address==VE+0x5e4){assert(m->sram_offset<1024);m->sram[m->sram_offset++]=value;}
    return write_reg(v,address,width,value);
}
static uint8_t *load(const char *path,size_t *n)
{
    FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long bytes=ftell(f);
    assert(bytes>0&&bytes<=524288&&!fseek(f,0,SEEK_SET));
    uint8_t *p=malloc((size_t)bytes);assert(p&&fread(p,1,(size_t)bytes,f)==(size_t)bytes&&!fclose(f));
    *n=(size_t)bytes;return p;
}
/* Refusal leaves the output untouched. */
static void refused(const char *name,const uint8_t *b,size_t n,const uint64_t r[4],void *w,size_t wb)
{
    struct a733_hevc_plan plan,before;memset(&before,0xa5,sizeof(before));plan=before;
    if(a733_hevc_parse(b,n,r,w,wb,&plan)||memcmp(&plan,&before,sizeof(plan))){fprintf(stderr,"accepted %s\n",name);abort();}
    printf("PASS parse refusal: %s\n",name);
}
/* First two zero bytes followed by a byte above 3: an unescaped pair. */
static size_t zero_pair(const uint8_t *b,size_t from,size_t to)
{
    for(size_t i=from;i+3<to;++i)if(!b[i]&&!b[i+1]&&b[i+2]>3)return i;
    return 0;
}
static void parse_cases(const uint8_t *clip,size_t n,const uint64_t r[4])
{
    uint8_t *b=malloc(n+16);assert(b);struct a733_hevc_plan plan;
    assert(a733_hevc_parse(clip,n,r,workspace,sizeof(workspace),&plan));
    const struct a733_hevc_plan clean=plan;
    size_t idr=plan.pictures[0].offset,idr_end=idr+plan.pictures[0].bytes,p=plan.pictures[1].offset;
    /* Slice data past the header may carry a valid escape, but not 03 04. */
    memcpy(b,clip,n);size_t at=idr+100;b[at-1]=0x55;b[at]=b[at+1]=0;b[at+2]=3;b[at+3]=1;
    assert(a733_hevc_parse(b,n,r,workspace,sizeof(workspace),&plan)&&!memcmp(&plan,&clean,sizeof(plan)));
    printf("PASS parse admission: escaped slice data\n");
    b[at+3]=4;refused("emulation-prevention byte followed by 4",b,n,r,workspace,sizeof(workspace));
    memcpy(b,clip,n);at=zero_pair(b,idr+8,idr_end);assert(at);
    b[at+2]=2;refused("unescaped 0x000002 in slice data",b,n,r,workspace,sizeof(workspace));
    memcpy(b,clip,n);b[idr_end-1]=0;refused("zero final slice byte",b,n,r,workspace,sizeof(workspace));
    memcpy(b,clip,n);b[4]|=0x80;refused("forbidden zero bit",b,n,r,workspace,sizeof(workspace));
    memcpy(b,clip,n);b[p+1]|=0x08;refused("nonzero P layer",b,n,r,workspace,sizeof(workspace));
    memcpy(b,clip,n);b[p+1]=(uint8_t)((b[p+1]&~7u)|2);refused("P temporal layer",b,n,r,workspace,sizeof(workspace));
    memcpy(b,clip,n);b[idr]=(uint8_t)((b[idr]&0x81)|(19<<1));
    assert(a733_hevc_parse(b,n,r,workspace,sizeof(workspace),&plan)&&plan.pictures[0].nal_type==19);
    printf("PASS parse admission: IDR_W_RADL\n");
    memcpy(b,clip,n);b[idr]=(uint8_t)((b[idr]&0x81)|(1<<1));refused("non-IRAP first slice",b,n,r,workspace,sizeof(workspace));
    const uint8_t aud[]={0,0,1,0x46,1,0x50};memcpy(b,clip,n);memcpy(b+n,aud,sizeof(aud));
    uint64_t extra[4]={r[0],r[1],r[2],r[3]+sizeof(aud)};refused("trailing access unit delimiter",b,n+sizeof(aud),extra,workspace,sizeof(workspace));
    uint64_t shifted[4]={0,r[1]+1,r[1]+1,r[3]-1};refused("picture boundary inside the P start code",clip,n,shifted,workspace,sizeof(workspace));
    uint64_t gap[4]={0,r[1],r[1]+1,r[3]-1};refused("noncontiguous ranges",clip,n,gap,workspace,sizeof(workspace));
    uint64_t cut[4]={0,r[1],r[2],8};refused("P slice header truncated",clip,(size_t)(r[2]+8),cut,workspace,sizeof(workspace));
    refused("short workspace",clip,n,r,workspace,a733_hevc_parser_bytes()-1);
    refused("misaligned workspace",clip,n,r,workspace+1,sizeof(workspace)-1);
    free(b);
}
/* Storage maps allocator physical addresses that differ from CPU addresses. */
static void storage_cases(struct hevc_storage *s,const uint8_t *clip,size_t n,const struct a733_hevc_plan *plan)
{
    static struct device_memory_pool wb,nc;
    uint8_t *wbm=aligned_alloc(16384,4u<<20),*ncm=aligned_alloc(16384,4u<<20);assert(wbm&&ncm);
    assert(device_memory_pool_init(&wb,(uintptr_t)wbm,0x48000000,4u<<20));
    assert(device_memory_pool_init(&nc,(uintptr_t)ncm,0x78000000,4u<<20));
    assert(hevc_storage_init(s,&wb,&nc,99)&&!hevc_storage_guards(s));
    for(unsigned i=0;i<HEVC_BUFFERS;++i){
        struct device_memory_extent e;enum device_memory_state state;uint64_t pa;
        assert(device_memory_describe(&nc,s->nc_leases[i+1],&e,&state)&&state==DEVICE_MEMORY_CPU);
        assert(s->tables.spans[i].physical==e.physical+4096&&s->tables.spans[i].physical!=(uintptr_t)s->buffers[i]);
        assert(a733_iommu_translate(&s->tables,s->job.buffers[i].iova,A733_IOMMU_READ,&pa)==1&&pa==e.physical+4096);
        assert((a733_iommu_translate(&s->tables,s->job.buffers[i].iova,A733_IOMMU_WRITE,&pa)==1)==(i>=HEVC_OUTPUT0));
    }
    for(unsigned i=0;i<3;++i){struct device_memory_extent e;enum device_memory_state state;
        assert(device_memory_describe(&wb,s->wb_leases[i],&e,&state)&&state==DEVICE_MEMORY_RETAINED);}
    memcpy(s->upload,clip,n);uint8_t digest[32];storage_digest_bytes(clip,n,digest);
    digest[0]^=1;assert(!hevc_storage_publish(s,n,plan,digest)&&!s->published);digest[0]^=1;
    assert(hevc_storage_publish(s,n,plan,digest)&&hevc_storage_guards(s));
    for(unsigned i=0;i<2;++i){const struct a733_hevc_picture *p=&plan->pictures[i];
        assert(!memcmp(s->buffers[i],clip+p->offset,p->bytes));
        for(size_t k=p->bytes;k<A733_HEVC_INPUT_SLOT;++k)assert(!s->buffers[i][k]);}
    assert(!hevc_storage_publish(s,n,plan,digest));
    s->buffers[HEVC_OUTPUT1][A733_HEVC_PICTURE_BYTES]^=1;assert(!hevc_storage_guards(s));
    s->buffers[HEVC_OUTPUT1][A733_HEVC_PICTURE_BYTES]^=1;
    s->buffers[HEVC_ENTRY][A733_HEVC_ENTRY_SLOT]^=1;assert(!hevc_storage_guards(s));
    s->buffers[HEVC_ENTRY][A733_HEVC_ENTRY_SLOT]^=1;assert(hevc_storage_guards(s));
    puts("PASS storage: physical-address tables, read-only inputs, retained write-back extents, digest-bound publish and guards");
}
static void json_words(FILE *f,const char *name,const uint32_t *words,unsigned count)
{fprintf(f,"\"%s\":[",name);for(unsigned i=0;i<count;++i)fprintf(f,"%s%u",i?",":"",words[i]);fprintf(f,"]");}
int main(int argc,char **argv)
{
    assert(argc==4);size_t n;uint8_t *clip=load(argv[1],&n);
    uint64_t r[4]={0,strtoull(argv[2],NULL,0),strtoull(argv[2],NULL,0),0};r[3]=n-r[1];
    assert(a733_hevc_parser_bytes()<=sizeof(workspace));
    struct a733_hevc_plan plan;assert(a733_hevc_parse(clip,n,r,workspace,sizeof(workspace),&plan));
    parse_cases(clip,n,r);
    static struct hevc_storage storage;storage_cases(&storage,clip,n,&plan);
    static const char *const names[]={"nominal IDR then P after ACK","finish with error retained",
        "data request retained","short CTB count retained","deadline while busy retained",
        "unsupported identity before memory","copy refusal retained","flush error retained",
        "P picture busy at entry retained"};
    FILE *out=NULL;
    for(unsigned variant=0;variant<sizeof(names)/sizeof(*names);++variant){
        static struct hevc_model m;memset(&m,0,sizeof(m));m.variant=variant;
        setup(&m.common,variant==5?UINT64_C(0x3332100021321):UINT64_C(0x3331000021320));
        m.header_bits[0]=plan.pictures[0].header_bits;m.header_bits[1]=plan.pictures[1].header_bits;
        struct a733_clock_leases clocks={0};assert(a733_clock_leases_init(&clocks,now,&m));
        struct a733_gpu_resource_manager power={0};struct a733_gpu_resource_ops po={now,resource_read,unused_update,unused_rail};
        assert(a733_gpu_resources_init(&power,&po,&m));uint64_t pt=0;struct a733_gpu_power_resources ready;
        assert(a733_gpu_resources_acquire(&power,2,9,DEADLINE,&pt,&ready)==A733_GPU_RESOURCE_OK);
        struct a733_video_platform_config c={.clocks=&clocks,.power=&power,.power_client=2,.owner=99,.handoff=123,
            .pck={pt,9,A733_GPU_RESOURCE_STABLE},.module=0x80000001,.module_hz=234000000,
            .fabric={0,0x80000000,0x80000000},.fabric_hz={24000000,24000000,24000000}};
        a733_clock_mask_t scopes;assert(a733_video_platform_scopes(&c,&scopes));
        assert(a733_clock_acquire(&clocks,99,scopes,A733_CLOCK_VE_DEC,DEADLINE,&c.clock)==A733_CLOCK_OK);
        struct a733_hevc_platform_ops ops={now,admit_hevc,memory_hevc,copied_hevc,enter,leave,read_hevc,write_hevc};
        struct a733_hevc_platform platform;memset(&platform,0,sizeof(platform));
        assert(a733_hevc_platform_init(&platform,&c,&ops,&m));
        struct a733_hevc_run run;
        struct a733_hevc_job bad=storage.job;bad.buffers[HEVC_MV1].iova=bad.buffers[HEVC_MV0].iova+4096;
        assert(!a733_hevc_begin(&run,a733_video_platform_hevc_ops(),&platform,&bad,DEADLINE));
        bad=storage.job;bad.plan.pictures[1].poc=0;
        assert(!a733_hevc_begin(&run,a733_video_platform_hevc_ops(),&platform,&bad,DEADLINE));
        assert(a733_hevc_begin(&run,a733_video_platform_hevc_ops(),&platform,&storage.job,DEADLINE));
        assert(!a733_hevc_next(&run));
        int result=0;
        for(unsigned poll=0;poll<8000&&result>=0;++poll){
            result=a733_hevc_poll(&run);
            if(result==1&&!m.picture){
                /* The copied IDR waits for its ACK: polling programs nothing. */
                unsigned writes=m.common.writes;
                for(unsigned i=0;i<4;++i)assert(a733_hevc_poll(&run)==1);
                assert(m.common.writes==writes);
                m.picture=1;assert(a733_hevc_next(&run)==1&&!a733_hevc_next(&run));result=0;
            }else if(result==1)break;
        }
        if(!variant){
            assert(result==1&&run.state==HEVC_COPIED&&m.copies==4&&m.triggered[0]&&m.triggered[1]&&!a733_hevc_next(&run));
            out=fopen(argv[3],"w");assert(out);
            fprintf(out,"{\"clip_bytes\":%zu,\"events\":[",n);
            for(unsigned i=0;i<m.common.writes;++i)fprintf(out,"%s[%u,%u]",i?",":"",m.common.events[i].address,m.common.events[i].value);
            fprintf(out,"],\"pictures\":[");
            for(unsigned i=0;i<2;++i){const struct a733_hevc_picture *p=&plan.pictures[i];
                fprintf(out,"%s{\"offset\":%u,\"bytes\":%u,\"header_bits\":%u,\"poc\":%u,\"type\":%u,\"nal_type\":%u,\"trigger_event\":%u,",
                    i?",":"",p->offset,p->bytes,p->header_bits,p->poc,p->type,p->nal_type,m.events_at_trigger[i]);
                json_words(out,"registers",m.image[i],1024);fputc(',',out);json_words(out,"sram",m.frames[i],1024);fputc('}',out);}
            fprintf(out,"],\"buffers\":[");
            for(unsigned i=0;i<HEVC_BUFFERS;++i)fprintf(out,"%s[%llu,%llu]",i?",":"",
                (unsigned long long)storage.job.buffers[i].iova,(unsigned long long)storage.job.buffers[i].bytes);
            fprintf(out,"]}\n");assert(!fclose(out));
        }else{
            assert(result==-1&&run.state==HEVC_FAILED);
            unsigned accesses=m.common.reads+m.common.writes;
            assert(a733_hevc_poll(&run)==-1&&m.common.reads+m.common.writes==accesses&&!a733_hevc_next(&run));
            if(variant==5)assert(!m.common.memory&&!m.common.writes&&!m.triggered[0]);
            if(variant>=1&&variant<=4)assert(m.triggered[0]&&!m.copies&&!m.triggered[1]);
            if(variant==6)assert(m.copies==1&&!m.triggered[1]);
            if(variant==7)assert(!m.triggered[0]);
            if(variant==8)assert(m.triggered[0]&&m.copies==2&&!m.triggered[1]);
        }
        assert(a733_clock_token_status(&clocks,&c.clock)==A733_CLOCK_OK);
        assert(a733_gpu_resources_status(&power,2,&c.pck)==A733_GPU_RESOURCE_OK);
        printf("PASS controller: %s\n",names[variant]);
    }
    free(clip);return 0;
}
