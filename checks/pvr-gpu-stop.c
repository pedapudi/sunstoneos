#include "pvr_gpu_stop.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct event { unsigned write, offset, width; uint64_t value; };
static struct {
    unsigned char memory[5][128*88], commands[2][88];
    uint64_t now;
    unsigned calls, fail_at, sync, clears, sent, no_ack, no_return, status;
    unsigned hold_register, register_reads, isolation, late_isolation, irq, irq_cleared;
    unsigned events_count, frozen;
    int isolation_result;
    unsigned isolation_polls, isolation_pending;
    int poll_result;
    struct event events[32];
} m;
static struct pvr_ccb q;
static struct pvr_gpu_control c;
static struct pvr_gpu_stop s;
static unsigned char *image, *variant;
static size_t image_bytes;
static unsigned faults;
static uint32_t word(const unsigned char *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void put(unsigned char *p,uint32_t v)
{ for(unsigned i=0;i<4;++i)p[i]=(unsigned char)(v>>(i*8)); }
static int operation(void) { if(!m.frozen)++m.now; return ++m.calls!=m.fail_at; }
static int read_memory(void *ctx,enum pvr_ccb_region r,uint32_t off,void *out,size_t n)
{
    assert(ctx==&m && off<=pvr_ccb_region_bytes(r) && n<=pvr_ccb_region_bytes(r)-off);
    if(!operation())return 0; memcpy(out,m.memory[r]+off,n); return 1;
}
static int write_memory(void *ctx,enum pvr_ccb_region r,uint32_t off,const void *in,size_t n)
{
    assert(ctx==&m && off<=pvr_ccb_region_bytes(r) && n<=pvr_ccb_region_bytes(r)-off);
    if(!operation())return 0; memcpy(m.memory[r]+off,in,n); return 1;
}
static int barrier(void *ctx,enum pvr_ccb_barrier kind)
{ assert(ctx==&m); (void)kind; return operation(); }
static int kick(void *ctx,uint32_t off,uint32_t v)
{
    assert(ctx==&m && off==0xb00 && !v);
    if(!operation())return 0;
    assert(m.sent<2 && m.clears==m.sent+1 && m.sync==0);
    unsigned next=word(m.memory[0]),slot=(next-1)&127;
    memcpy(m.commands[m.sent],m.memory[1]+slot*88,88);
    assert(word(m.commands[m.sent])==(PVR_CCB_MAGIC|107));
    assert(word(m.commands[m.sent]+8)==(m.sent?1u:2u) && word(m.commands[m.sent]+12)==1);
    ++m.sent; put(m.memory[0]+128,next);
    if(!m.no_return)put(m.memory[2]+slot*4,m.status);
    if(!m.no_ack)m.sync=0x55;
    return 1;
}
static const struct pvr_ccb_ops qops={read_memory,write_memory,barrier,kick};
static int now(void *ctx,uint64_t *v) { assert(ctx==&m); if(!operation())return 0; *v=m.now; return 1; }
static int irq_read(void *ctx,uint32_t off,uint32_t *v)
{ assert(ctx==&m && off==0x8a8); if(!operation())return 0; *v=0; return 1; }
static int irq_write(void *ctx,uint32_t off,uint32_t v) { (void)ctx;(void)off;(void)v;assert(0);return 0; }
static int compat(void *ctx,uint32_t off,void *out,size_t n)
{ (void)ctx;(void)off;(void)out;(void)n;assert(0);return 0; }
static const struct pvr_gpu_control_ops cops={now,irq_read,irq_write,compat,NULL};
static void event(unsigned write,uint32_t off,uint32_t width,uint64_t v)
{
    assert(m.events_count<32 && (s.unacknowledged ? c.phase==PVR_GPU_CONTROL_FAILED && q.phase==PVR_CCB_FAILED :
                                 c.phase==PVR_GPU_CONTROL_RETIRED && q.phase==PVR_CCB_RETIRED));
    m.events[m.events_count++]=(struct event){write,off,width,v};
}
static int read_reg(void *ctx,uint32_t off,uint32_t width,uint64_t *v)
{
    assert(ctx==&m); if(!operation())return 0;
    ++m.register_reads;
    *v=off==0x3c8?0x7f:off==0x3898?0xff:off==0x8a8?m.irq:0;
    if(m.hold_register==off) { *v=off==0x3c8||off==0x3898?0:1; return 1; }
    event(0,off,width,*v);
    return 1;
}
static int write_reg(void *ctx,uint32_t off,uint32_t width,uint64_t v)
{
    assert(ctx==&m); if(!operation())return 0; event(1,off,width,v);
    /* The interrupt clear follows a pending status read, with its event bit. */
    if(off==0x8b0) { assert(m.irq&1 && v==1 && !m.irq_cleared); m.irq_cleared=1; }
    return 1;
}
static int sync_write(void *ctx,uint32_t v)
{ assert(ctx==&m && !v); if(!operation())return 0; m.sync=v; ++m.clears; return 1; }
static int sync_read(void *ctx,uint32_t *v)
{ assert(ctx==&m); if(!operation())return 0; *v=m.sync; return 1; }
static int isolate(void *ctx)
{
    assert(ctx==&m && s.index==s.count &&
           (s.unacknowledged || (m.sent==2 && s.acknowledgement[0] && s.acknowledgement[1])));
    if(!operation())return 0; ++m.isolation;
    if(m.late_isolation)m.now=s.deadline;
    return m.isolation_result;
}
static int isolation_poll(void *ctx)
{
    assert(ctx==&m && m.isolation==1);
    if(!operation())return -1;
    ++m.isolation_polls;
    return m.isolation_polls<=m.isolation_pending ? 0 : m.poll_result;
}
static const struct pvr_gpu_stop_ops ops={now,read_reg,write_reg,sync_write,sync_read,isolate,NULL};
static void prepare(const void *fw)
{
    memset(&m,0,sizeof(m)); memset(&q,0,sizeof(q)); memset(&c,0,sizeof(c)); memset(&s,0,sizeof(s));
    assert(pvr_ccb_init(&q,&qops,&m,21)==PVR_CCB_OK);
    assert(pvr_ccb_start(&q,m.now)==PVR_CCB_OK);
    c.ops=cops; c.context=&m; c.queue=&q; c.phase=PVR_GPU_CONTROL_READY; c.last_now=m.now;
    m.status=1; m.sync=0xfeed; m.calls=0; m.isolation_result=1; m.irq=1;
    assert(pvr_gpu_stop_init(&s,&c,fw,image_bytes,&ops,&m,10000));
}
static enum pvr_gpu_stop_result run(void)
{
    for(unsigned i=0;i<100;++i) {
        enum pvr_gpu_stop_result r=pvr_gpu_stop_poll(&s);
        if(r!=PVR_GPU_STOP_AGAIN)return r;
    }
    assert(0); return PVR_GPU_STOP_INVALID;
}
static void stopped(unsigned reason)
{
    assert(s.phase==PVR_GPU_STOP_FAILED && s.error==reason &&
           c.phase==PVR_GPU_CONTROL_FAILED && q.phase==PVR_CCB_FAILED);
    unsigned calls=m.calls;
    assert(pvr_gpu_stop_poll(&s)==PVR_GPU_STOP_FAULT);
    assert(pvr_gpu_control_poll(&c,4)==PVR_GPU_CONTROL_FAULT);
    assert(pvr_ccb_poll(&q,m.now)==PVR_CCB_FAULT && calls==m.calls); ++faults;
}
static FILE *output(const char *dir,const char *name)
{
    char path[4096]; int n=snprintf(path,sizeof(path),"%s/%s",dir,name);
    assert(n>0 && (size_t)n<sizeof(path)); FILE *f=fopen(path,"wb"); assert(f); return f;
}
static void save(const char *dir,unsigned variant_id)
{
    char name[64]; snprintf(name,sizeof(name),"trace-%u.json",variant_id);
    FILE *f=output(dir,name); fprintf(f,"{\"plan\":[");
    for(unsigned i=0;i<s.count;++i) {
        const struct pvr_gpu_stop_step *p=&s.steps[i];
        fprintf(f,"%s{\"kind\":%u,\"offset\":%u,\"width\":%u,\"mask\":%llu,\"value\":%llu}",
            i?",":"",p->kind,p->offset,p->width,(unsigned long long)p->mask,(unsigned long long)p->value);
    }
    fprintf(f,"],\"accesses\":[");
    for(unsigned i=0;i<m.events_count;++i) {
        const struct event *p=&m.events[i];
        fprintf(f,"%s{\"write\":%u,\"offset\":%u,\"width\":%u,\"value\":%llu}",
            i?",":"",p->write,p->offset,p->width,(unsigned long long)p->value);
    }
    fprintf(f,"]}\n"); assert(!fclose(f));
}
int main(int argc,char **argv)
{
    assert(argc==3); FILE *f=fopen(argv[1],"rb"); assert(f && !fseek(f,0,SEEK_END));
    long n=ftell(f); assert(n>0 && !fseek(f,0,SEEK_SET)); image_bytes=(size_t)n;
    image=malloc(image_bytes); variant=malloc(image_bytes); assert(image && variant);
    assert(fread(image,1,image_bytes,f)==image_bytes && !fclose(f));
    assert(pvr_gpu_stop_poll(&s)==PVR_GPU_STOP_INVALID);
    prepare(image); assert(s.count==14 && run()==PVR_GPU_STOP_OK && m.isolation==1 && m.irq_cleared);
    assert(c.phase==PVR_GPU_CONTROL_RETIRED && q.phase==PVR_CCB_RETIRED);
    unsigned calls=m.calls,successful_calls=m.calls;
    assert(pvr_gpu_stop_poll(&s)==PVR_GPU_STOP_OK);
    assert(pvr_gpu_control_poll(&c,4)==PVR_GPU_CONTROL_INVALID);
    assert(pvr_ccb_poll(&q,m.now)==PVR_CCB_FAULT && calls==m.calls);
    save(argv[2],0); f=output(argv[2],"commands.bin"); assert(fwrite(m.commands,1,176,f)==176 && !fclose(f));
    memcpy(variant,image,image_bytes); variant[122880+48+4]&=0xfe; variant[122880+48+8]|=0x20;
    prepare(variant); assert(s.count==13 && run()==PVR_GPU_STOP_OK); save(argv[2],1);
    f=output(argv[2],"variant.fw"); assert(fwrite(variant,1,image_bytes,f)==image_bytes && !fclose(f));
    for(unsigned i=1;i<=successful_calls;++i) {
        prepare(image); m.fail_at=i;
        assert(run()==PVR_GPU_STOP_FAULT && m.calls==i); stopped(s.error);
    }
    prepare(image); m.no_ack=1;
    while(s.phase!=PVR_GPU_STOP_WAIT_IDLE)assert(pvr_gpu_stop_poll(&s)==PVR_GPU_STOP_AGAIN);
    uint64_t waited=m.now;
    assert(pvr_gpu_stop_poll(&s)==PVR_GPU_STOP_AGAIN && !m.events_count && !m.isolation);
    assert(s.phase==PVR_GPU_STOP_WAIT_IDLE && m.now-waited>=A733_GPU_SPIN_US);
    m.now=s.deadline; assert(run()==PVR_GPU_STOP_FAULT); stopped(PVR_GPU_STOP_TIMEOUT);
    prepare(image);
    while(s.phase!=PVR_GPU_STOP_CLEAR_OFF)assert(pvr_gpu_stop_poll(&s)==PVR_GPU_STOP_AGAIN);
    m.no_ack=1;
    while(s.phase!=PVR_GPU_STOP_WAIT_OFF)assert(pvr_gpu_stop_poll(&s)==PVR_GPU_STOP_AGAIN);
    assert(m.sync==0 && pvr_gpu_stop_poll(&s)==PVR_GPU_STOP_AGAIN && !m.events_count);
    m.now=s.deadline; assert(run()==PVR_GPU_STOP_FAULT); stopped(PVR_GPU_STOP_TIMEOUT);
    prepare(image); m.now=s.last_now-2;
    assert(run()==PVR_GPU_STOP_FAULT && !m.clears); stopped(PVR_GPU_STOP_CLOCK);
    prepare(image); m.status=5; assert(run()==PVR_GPU_STOP_FAULT && !m.events_count); stopped(PVR_GPU_STOP_COMMAND);
    const unsigned held[]={0x3c8,0x1358,0x1350,0x1320,0x1338,0x3870,0x3898};
    for(unsigned i=0;i<sizeof(held)/sizeof(held[0]);++i) {
        prepare(image); m.hold_register=held[i];
        while(s.phase!=PVR_GPU_STOP_REGISTERS || s.steps[s.index].offset!=held[i])
            assert(pvr_gpu_stop_poll(&s)==PVR_GPU_STOP_AGAIN);
        unsigned index=s.index,reads=m.register_reads; uint64_t start=m.now;
        assert(pvr_gpu_stop_poll(&s)==PVR_GPU_STOP_AGAIN && s.index==index && !m.isolation);
        /* The held register is re-read for A733_GPU_SPIN_US within one poll. */
        assert(m.register_reads-reads>1 && m.now-start>=A733_GPU_SPIN_US && m.now-start<=3*A733_GPU_SPIN_US);
        m.now=s.deadline; assert(run()==PVR_GPU_STOP_FAULT); stopped(PVR_GPU_STOP_TIMEOUT);
    }
    /* A stopped clock cannot end a re-read: the poll returns AGAIN without
     * advancing, and shutdown completes once the clock runs again. */
    prepare(image); m.no_ack=1;
    while(s.phase!=PVR_GPU_STOP_WAIT_IDLE)assert(pvr_gpu_stop_poll(&s)==PVR_GPU_STOP_AGAIN);
    m.frozen=1; assert(pvr_gpu_stop_poll(&s)==PVR_GPU_STOP_AGAIN && s.phase==PVR_GPU_STOP_WAIT_IDLE);
    m.frozen=0; m.no_ack=0; m.sync=0x55; assert(run()==PVR_GPU_STOP_OK);
    prepare(image); m.hold_register=held[0];
    while(s.phase!=PVR_GPU_STOP_REGISTERS || s.steps[s.index].offset!=held[0])
        assert(pvr_gpu_stop_poll(&s)==PVR_GPU_STOP_AGAIN);
    m.frozen=1; unsigned held_index=s.index;
    assert(pvr_gpu_stop_poll(&s)==PVR_GPU_STOP_AGAIN && s.index==held_index && !m.isolation);
    m.frozen=0; m.hold_register=0; assert(run()==PVR_GPU_STOP_OK);
    prepare(image); m.late_isolation=1;
    assert(run()==PVR_GPU_STOP_FAULT && s.isolation_observed && m.isolation==1); stopped(PVR_GPU_STOP_TIMEOUT);
    /* Negative errors and unexpected positive values cannot establish drain. */
    const int isolation_failures[]={-1,2};
    for(unsigned i=0;i<sizeof(isolation_failures)/sizeof(isolation_failures[0]);++i) {
        prepare(image); m.isolation_result=isolation_failures[i];
        assert(run()==PVR_GPU_STOP_FAULT && !s.isolation_observed && m.isolation==1);
        stopped(PVR_GPU_STOP_IO);
    }
    prepare(image); s.ops.isolate_poll=isolation_poll; m.isolation_pending=2; m.poll_result=1;
    assert(run()==PVR_GPU_STOP_OK && m.isolation==1 && m.isolation_polls==3);
    calls=m.calls; assert(pvr_gpu_stop_poll(&s)==PVR_GPU_STOP_OK && calls==m.calls);
    const int poll_failures[]={-1,2};
    for(unsigned i=0;i<sizeof(poll_failures)/sizeof(poll_failures[0]);++i) {
        prepare(image);s.ops.isolate_poll=isolation_poll;m.poll_result=poll_failures[i];
        assert(run()==PVR_GPU_STOP_FAULT && m.isolation==1 && m.isolation_polls==1);
        assert(!s.isolation_observed);stopped(PVR_GPU_STOP_IO);
    }
    prepare(image);s.ops.isolate_poll=isolation_poll;m.isolation_pending=100;
    while(!m.isolation_polls)assert(pvr_gpu_stop_poll(&s)==PVR_GPU_STOP_AGAIN);
    m.now=s.deadline;calls=m.calls;
    assert(run()==PVR_GPU_STOP_FAULT && m.calls==calls+1 && m.isolation==1 && m.isolation_polls==1);
    stopped(PVR_GPU_STOP_TIMEOUT);
    /* No pending interrupt: the status is read and nothing is cleared. */
    prepare(image); m.irq=0; assert(run()==PVR_GPU_STOP_OK && !m.irq_cleared && m.events_count==s.count-1);
    /* A power acknowledgement is independent of the queue return-slot flag. */
    prepare(image); m.no_return=1; assert(run()==PVR_GPU_STOP_OK);
    uint32_t value=0; calls=m.calls;
    assert(pvr_ccb_collect(&q,s.tokens[0],&value)==PVR_CCB_FAULT && calls==m.calls);
    /* Already recorded results survive normal retirement. */
    prepare(image);
    while(s.phase!=PVR_GPU_STOP_WAIT_IDLE)assert(pvr_gpu_stop_poll(&s)==PVR_GPU_STOP_AGAIN);
    assert(pvr_ccb_poll(&q,m.now)==PVR_CCB_OK);
    assert(pvr_gpu_control_retire(&c)==PVR_GPU_CONTROL_OK);
    calls=m.calls; assert(pvr_ccb_collect(&q,s.tokens[0],&value)==PVR_CCB_OK && value==1 && calls==m.calls);
    assert(pvr_gpu_control_abort(&c)==PVR_GPU_CONTROL_FAULT && q.phase==PVR_CCB_FAILED && calls==m.calls);
    /* External failures are noticed without another callback. */
    prepare(image); assert(pvr_gpu_control_abort(&c)==PVR_GPU_CONTROL_FAULT);
    calls=m.calls; assert(run()==PVR_GPU_STOP_FAULT && calls==m.calls); stopped(PVR_GPU_STOP_CONTROL);
    prepare(image); struct pvr_gpu_stop before=s;
    assert(!pvr_gpu_stop_init(&s,&c,image,image_bytes,&ops,&m,10000) && !memcmp(&s,&before,sizeof(s)));
    memset(&s,0,sizeof(s)); before=s; struct pvr_gpu_stop_ops bad=ops; bad.isolate=NULL;
    assert(!pvr_gpu_stop_init(&s,&c,image,image_bytes,&bad,&m,10000) && !memcmp(&s,&before,sizeof(s)));
    /* Without the acknowledgements (upstream's hard reset): refused while
     * the control is READY, admitted once the control and queue are latched
     * failed, from NEW or from a stop that failed before its first register
     * step; the same register plan and isolation follow, and no command. */
    prepare(image); before=s;
    assert(!pvr_gpu_stop_init_unacknowledged(&s,&c,image,image_bytes,&ops,&m,10000) && !memcmp(&s,&before,sizeof(s)));
    m.no_ack=1;
    while(s.phase!=PVR_GPU_STOP_WAIT_IDLE)assert(pvr_gpu_stop_poll(&s)==PVR_GPU_STOP_AGAIN);
    m.now=s.deadline; assert(run()==PVR_GPU_STOP_FAULT); stopped(PVR_GPU_STOP_TIMEOUT);
    unsigned sent=m.sent; m.events_count=0;
    assert(pvr_gpu_stop_init_unacknowledged(&s,&c,image,image_bytes,&ops,&m,m.now+10000));
    assert(s.phase==PVR_GPU_STOP_REGISTERS && s.unacknowledged && !s.index && s.count==14);
    assert(run()==PVR_GPU_STOP_OK && m.isolation==1 && m.sent==sent && m.irq_cleared && m.events_count==s.count);
    assert(c.phase==PVR_GPU_CONTROL_FAILED && q.phase==PVR_CCB_FAILED);
    /* From NEW, after an external control failure. */
    prepare(image); memset(&s,0,sizeof(s)); assert(pvr_gpu_control_abort(&c)==PVR_GPU_CONTROL_FAULT);
    assert(pvr_gpu_stop_init_unacknowledged(&s,&c,image,image_bytes,&ops,&m,m.now+10000));
    assert(run()==PVR_GPU_STOP_OK && m.isolation==1 && !m.sent);
    /* Not after a register step: the stop failed on a held idle poll. */
    prepare(image); m.hold_register=held[0];
    while(s.phase!=PVR_GPU_STOP_REGISTERS || s.steps[s.index].offset!=held[0])
        assert(pvr_gpu_stop_poll(&s)==PVR_GPU_STOP_AGAIN);
    m.now=s.deadline; assert(run()==PVR_GPU_STOP_FAULT); stopped(PVR_GPU_STOP_TIMEOUT); before=s;
    assert(!pvr_gpu_stop_init_unacknowledged(&s,&c,image,image_bytes,&ops,&m,m.now+10000) &&
           !memcmp(&s,&before,sizeof(s)));
    /* A held idle poll ends the unacknowledged stop at its deadline, before
     * isolation: the GPU stays powered. */
    prepare(image); memset(&s,0,sizeof(s)); assert(pvr_gpu_control_abort(&c)==PVR_GPU_CONTROL_FAULT);
    m.hold_register=held[0];
    assert(pvr_gpu_stop_init_unacknowledged(&s,&c,image,image_bytes,&ops,&m,m.now+10000));
    while(s.steps[s.index].offset!=held[0])assert(pvr_gpu_stop_poll(&s)==PVR_GPU_STOP_AGAIN);
    m.now=s.deadline; assert(run()==PVR_GPU_STOP_FAULT && !m.isolation && s.error==PVR_GPU_STOP_TIMEOUT); ++faults;
    free(image); free(variant);
    printf("PASS GPU shutdown: two acknowledged commands, interrupt acknowledgement, idle/isolation ordering, retirement, "
           "the unacknowledged stop, and %u retained faults\n",faults);
}
