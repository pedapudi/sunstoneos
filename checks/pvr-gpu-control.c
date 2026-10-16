#include "pvr_gpu_control.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_BYTES = 128 * 88 };
struct irq_event { unsigned write, offset, value; };
struct model {
    unsigned char memory[5][MAX_BYTES], compat[64];
    uint64_t now;
    unsigned calls, fail_at, queue_calls, irq, ack_count, irq_reads, handler_count;
    unsigned auto_complete, status, change_updated, fail_handler, arrive_on_ack;
    unsigned irq_event_count;
    struct irq_event irq_events[128];
};
static struct model m;
static struct pvr_ccb q;
static struct pvr_gpu_start start;
static struct pvr_gpu_control control;
static unsigned faults;
static void put(unsigned char *p, uint64_t v, unsigned n)
{ for (unsigned i=0;i<n;++i) p[i]=(unsigned char)(v>>(i*8)); }
static uint32_t word(const unsigned char *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static int operation(void) { ++m.now; return ++m.calls != m.fail_at; }
static int read_memory(void *ctx, enum pvr_ccb_region r, uint32_t off, void *out, size_t n)
{
    assert(ctx==&m && off<=pvr_ccb_region_bytes(r) && n<=pvr_ccb_region_bytes(r)-off);
    ++m.queue_calls; if(!operation())return 0; memcpy(out,m.memory[r]+off,n); return 1;
}
static int write_memory(void *ctx, enum pvr_ccb_region r, uint32_t off, const void *in, size_t n)
{
    assert(ctx==&m && off<=pvr_ccb_region_bytes(r) && n<=pvr_ccb_region_bytes(r)-off);
    ++m.queue_calls; if(!operation())return 0; memcpy(m.memory[r]+off,in,n); return 1;
}
static int barrier(void *ctx, enum pvr_ccb_barrier kind)
{ assert(ctx==&m); (void)kind; ++m.queue_calls; return operation(); }
static int kick(void *ctx,uint32_t reg,uint32_t value)
{
    assert(ctx==&m && reg==0xb00 && value==0); ++m.queue_calls;
    if(!operation())return 0;
    unsigned next=word(m.memory[PVR_CCB_HOST_CONTROL]), slot=(next-1)&127;
    assert(word(m.memory[PVR_CCB_HOST_COMMANDS]+slot*88)==PVR_CCB_HEALTH);
    for(unsigned i=4;i<88;++i)assert(m.memory[PVR_CCB_HOST_COMMANDS][slot*88+i]==0);
    if(m.auto_complete) {
        put(m.memory[PVR_CCB_HOST_CONTROL]+128,next,4);
        put(m.memory[PVR_CCB_RETURNS]+slot*4,m.status,4); m.irq=1;
    }
    return 1;
}
static const struct pvr_ccb_ops qops={read_memory,write_memory,barrier,kick};
static int now(void *ctx,uint64_t *value)
{ assert(ctx==&m); if(!operation())return 0; *value=m.now; return 1; }
static void irq_event(unsigned write,unsigned reg,unsigned value)
{
    assert(m.irq_event_count<128);
    m.irq_events[m.irq_event_count++]=(struct irq_event){write,reg,value};
}
static int irq_read(void *ctx,uint32_t reg,uint32_t *value)
{
    assert(ctx==&m && reg==0x8a8); if(!operation())return 0;
    ++m.irq_reads; *value=m.irq; irq_event(0,reg,*value); return 1;
}
static void event(unsigned type,unsigned tag)
{
    unsigned w=word(m.memory[PVR_CCB_FIRMWARE_CONTROL]);
    assert(((w+1)&31)!=word(m.memory[PVR_CCB_FIRMWARE_CONTROL]+128));
    unsigned char *p=m.memory[PVR_CCB_FIRMWARE_COMMANDS]+w*88;
    memset(p,0,88); put(p,PVR_CCB_MAGIC|type,4); put(p+8,tag,4);
    put(m.memory[PVR_CCB_FIRMWARE_CONTROL],(w+1)&31,4); m.irq=1;
}
static int irq_write(void *ctx,uint32_t reg,uint32_t value)
{
    assert(ctx==&m && reg==0x8b0 && value==1); if(!operation())return 0;
    irq_event(1,reg,value); ++m.ack_count; m.irq=0;
    if(m.arrive_on_ack) { m.arrive_on_ack=0; event(106,0xaabb); }
    return 1;
}
static int compat(void *ctx,uint32_t off,void *out,size_t n)
{
    assert(ctx==&m && ((off==56 && n==1)||(off==0 && n==64)));
    if(!operation())return 0;
    if(m.change_updated && !off)m.compat[56]=0;
    memcpy(out,m.compat+off,n); return 1;
}
static int handle_event(void *ctx,const unsigned char *data)
{
    assert(ctx==&m); if(!operation())return 0;
    assert(word(data)==(PVR_CCB_MAGIC|106)); ++m.handler_count;
    return !m.fail_handler;
}
static const struct pvr_gpu_control_ops ops={now,irq_read,irq_write,compat,handle_event};
static void prepare(void)
{
    memset(&m,0,sizeof(m)); memset(&q,0,sizeof(q)); memset(&control,0,sizeof(control));
    memset(&start,0,sizeof(start)); start.phase=PVR_GPU_START_BOOTED; start.last_now=100;
    assert(pvr_ccb_init(&q,&qops,&m,17)==PVR_CCB_OK);
    m.calls=m.queue_calls=0; m.now=100; m.auto_complete=1; m.status=1;
    put(m.compat,3,4); put(m.compat+8,PVR_FW_A733_BVNC,8);
    put(m.compat+16,3,4); put(m.compat+24,PVR_FW_A733_BVNC,8);
    put(m.compat+32,0x12345678,4); put(m.compat+36,0x01020304,4);
    put(m.compat+40,6976702,4); put(m.compat+44,0x55667788,4);
    m.compat[48]=1; m.compat[56]=1;
    assert(pvr_gpu_control_init(&control,&start,&q,&ops,&m,5000));
}
static enum pvr_gpu_control_result run(void)
{
    for(unsigned i=0;i<200;++i) {
        enum pvr_gpu_control_result r=pvr_gpu_control_poll(&control,4);
        if(r!=PVR_GPU_CONTROL_AGAIN)return r;
    }
    assert(0); return PVR_GPU_CONTROL_INVALID;
}
static void stopped(unsigned reason)
{
    assert(control.phase==PVR_GPU_CONTROL_FAILED && control.error==reason && q.phase==PVR_CCB_FAILED);
    unsigned calls=m.calls;
    assert(pvr_gpu_control_poll(&control,4)==PVR_GPU_CONTROL_FAULT);
    assert(pvr_ccb_poll(&q,m.now)==PVR_CCB_FAULT);
    assert(pvr_ccb_receive(&q,control.last_event)==PVR_CCB_FAULT);
    assert(pvr_ccb_start(&q,m.now)==PVR_CCB_FAULT);
    assert(pvr_ccb_abort(&q)==PVR_CCB_FAULT && m.calls==calls); ++faults;
}
static FILE *output(const char *dir,const char *name)
{
    char path[4096]; int n=snprintf(path,sizeof(path),"%s/%s",dir,name);
    assert(n>0 && (size_t)n<sizeof(path)); FILE *f=fopen(path,"wb"); assert(f); return f;
}
int main(int argc,char **argv)
{
    assert(argc==2);
    assert(pvr_ccb_abort(&q)==PVR_CCB_INVALID);
    prepare(); assert(pvr_gpu_control_poll(&control,0)==PVR_GPU_CONTROL_INVALID && !m.calls);
    assert(pvr_gpu_control_poll(&control,32)==PVR_GPU_CONTROL_INVALID && !m.calls);
    assert(run()==PVR_GPU_CONTROL_OK && control.phase==PVR_GPU_CONTROL_READY && control.health_status==1);
    unsigned successful_calls=m.calls;
    assert(!memcmp(control.compatibility,m.compat,64));
    FILE *f=output(argv[1],"compatibility.bin"); assert(fwrite(control.compatibility,1,64,f)==64 && !fclose(f));
    f=output(argv[1],"health-command.bin"); assert(fwrite(m.memory[PVR_CCB_HOST_COMMANDS],1,88,f)==88 && !fclose(f));
    f=output(argv[1],"irq.json"); fprintf(f,"[");
    for(unsigned i=0;i<m.irq_event_count;++i)fprintf(f,"%s{\"write\":%u,\"offset\":%u,\"value\":%u}",
        i?",":"",m.irq_events[i].write,m.irq_events[i].offset,m.irq_events[i].value);
    fprintf(f,"]\n"); assert(!fclose(f));
    for(unsigned i=1;i<=successful_calls;++i) {
        prepare(); m.fail_at=i; assert(run()==PVR_GPU_CONTROL_FAULT && m.calls==i);
        assert(control.error==PVR_GPU_CONTROL_IO || control.error==PVR_GPU_CONTROL_QUEUE);
        stopped(control.error);
    }
    prepare(); m.compat[56]=0;
    assert(pvr_gpu_control_poll(&control,4)==PVR_GPU_CONTROL_AGAIN && !m.queue_calls && !m.irq_reads);
    m.now=5000; assert(run()==PVR_GPU_CONTROL_FAULT); stopped(PVR_GPU_CONTROL_TIMEOUT);
    const unsigned invalid[]={0,8,16,24,56};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        prepare(); m.compat[invalid[i]]^=3;
        assert(run()==PVR_GPU_CONTROL_FAULT && !m.queue_calls && !m.irq_reads);
        stopped(PVR_GPU_CONTROL_INCOMPATIBLE);
    }
    prepare(); m.change_updated=1; assert(run()==PVR_GPU_CONTROL_FAULT); stopped(PVR_GPU_CONTROL_INCOMPATIBLE);
    prepare(); m.now=98; assert(run()==PVR_GPU_CONTROL_FAULT && !m.queue_calls); stopped(PVR_GPU_CONTROL_CLOCK);
    prepare(); m.auto_complete=0;
    assert(pvr_gpu_control_poll(&control,4)==PVR_GPU_CONTROL_AGAIN);
    assert(pvr_gpu_control_poll(&control,4)==PVR_GPU_CONTROL_AGAIN && control.phase==PVR_GPU_CONTROL_WAIT_HEALTH);
    m.now=5000; assert(run()==PVR_GPU_CONTROL_FAULT); stopped(PVR_GPU_CONTROL_TIMEOUT);
    prepare(); m.status=5; assert(run()==PVR_GPU_CONTROL_FAULT); stopped(PVR_GPU_CONTROL_HEALTH);
    prepare(); m.irq=0xffffffff; assert(run()==PVR_GPU_CONTROL_FAULT); stopped(PVR_GPU_CONTROL_IO);

    /* A collected health result is retained while an event backlog is drained. */
    prepare();
    assert(pvr_gpu_control_poll(&control,4)==PVR_GPU_CONTROL_AGAIN);
    assert(pvr_gpu_control_poll(&control,4)==PVR_GPU_CONTROL_AGAIN);
    event(106,1); event(106,2); event(106,3);
    assert(pvr_gpu_control_poll(&control,1)==PVR_GPU_CONTROL_AGAIN && control.health_done);
    assert(control.phase==PVR_GPU_CONTROL_WAIT_HEALTH);
    assert(pvr_gpu_control_poll(&control,4)==PVR_GPU_CONTROL_OK && m.handler_count==3);

    /* Bounded event batches, one IRQ acknowledgement, and work arriving during it. */
    prepare(); assert(run()==PVR_GPU_CONTROL_OK);
    event(106,1); event(106,2); event(106,3);
    unsigned acks=m.ack_count,handlers=m.handler_count,reads=m.irq_reads;
    assert(pvr_gpu_control_poll(&control,2)==PVR_GPU_CONTROL_AGAIN);
    assert(m.handler_count==handlers+2 && m.ack_count==acks+1 && m.irq_reads==reads+2);
    assert(pvr_gpu_control_poll(&control,2)==PVR_GPU_CONTROL_OK && m.handler_count==handlers+3);
    event(106,4); m.arrive_on_ack=1; handlers=m.handler_count;
    assert(pvr_gpu_control_poll(&control,1)==PVR_GPU_CONTROL_AGAIN);
    assert(pvr_gpu_control_poll(&control,2)==PVR_GPU_CONTROL_OK && m.handler_count==handlers+2);
    /* No IRQ is required to drain a queued event or notice a command deadline. */
    event(106,5); m.irq=0; handlers=m.handler_count;
    assert(pvr_gpu_control_poll(&control,2)==PVR_GPU_CONTROL_OK && m.handler_count==handlers+1);
    /* READY retains service after the initial boot deadline. */
    m.now=6000; assert(pvr_gpu_control_poll(&control,2)==PVR_GPU_CONTROL_OK);

    for(unsigned i=0;i<2;++i) {
        prepare(); assert(run()==PVR_GPU_CONTROL_OK); event(i?112:109,0xface);
        assert(run()==PVR_GPU_CONTROL_FAULT && !m.handler_count);
        assert(word(control.last_event+8)==0xface); stopped(PVR_GPU_CONTROL_RESTART);
    }
    prepare(); assert(run()==PVR_GPU_CONTROL_OK); event(110,0xabcd);
    assert(run()==PVR_GPU_CONTROL_FAULT && !m.handler_count); stopped(PVR_GPU_CONTROL_EVENT);
    prepare(); assert(run()==PVR_GPU_CONTROL_OK); event(106,1); m.fail_handler=1;
    assert(run()==PVR_GPU_CONTROL_FAULT && m.handler_count==1); stopped(PVR_GPU_CONTROL_EVENT);
    prepare(); control.ops.event=NULL; assert(run()==PVR_GPU_CONTROL_OK); event(106,1);
    assert(run()==PVR_GPU_CONTROL_FAULT); stopped(PVR_GPU_CONTROL_EVENT);
    prepare(); assert(run()==PVR_GPU_CONTROL_OK); unsigned calls=m.calls;
    assert(pvr_ccb_abort(&q)==PVR_CCB_FAULT && run()==PVR_GPU_CONTROL_FAULT && calls==m.calls);
    stopped(PVR_GPU_CONTROL_QUEUE);

    /* A pre-recorded command result survives an unrelated service fault. */
    prepare(); assert(pvr_gpu_control_poll(&control,4)==PVR_GPU_CONTROL_AGAIN);
    assert(pvr_gpu_control_poll(&control,4)==PVR_GPU_CONTROL_AGAIN);
    assert(pvr_ccb_poll(&q,m.now)==PVR_CCB_OK); m.irq=2;
    assert(run()==PVR_GPU_CONTROL_FAULT); stopped(PVR_GPU_CONTROL_IO);
    calls=m.calls; uint32_t status=0;
    assert(pvr_ccb_collect(&q,control.health_token,&status)==PVR_CCB_OK && status==1 && m.calls==calls);
    prepare(); struct pvr_gpu_control before=control;
    assert(!pvr_gpu_control_init(&control,&start,&q,&ops,&m,5000) && !memcmp(&before,&control,sizeof(control)));
    printf("PASS GPU compatibility, bounded IRQ/event service, health completion, and %u retained-failure cases\n",faults);
}
