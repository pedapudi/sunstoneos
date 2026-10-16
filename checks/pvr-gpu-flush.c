#include "pvr_gpu_flush.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct model {
    unsigned char memory[5][128*88];
    uint64_t now;
    unsigned calls, fail_at, publishes, kicks, events, automatic, status, irq;
    unsigned host_table, device_table, cached_translation, late_publish, rewind_after_irq, irq_reads;
};
static struct model m;
static struct pvr_ccb q;
static struct pvr_gpu_control c;
static struct pvr_gpu_flush f;
static unsigned faults;
static uint32_t word(const unsigned char *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void put(unsigned char *p,uint32_t v)
{ for(unsigned i=0;i<4;++i)p[i]=(unsigned char)(v>>(8*i)); }
static int operation(void)
{ ++m.now; return ++m.calls!=m.fail_at; }
static int read_memory(void *ctx,enum pvr_ccb_region r,uint32_t off,void *out,size_t n)
{
    assert(ctx==&m && off<=pvr_ccb_region_bytes(r) && n<=pvr_ccb_region_bytes(r)-off);
    if(!operation())return 0;
    memcpy(out,m.memory[r]+off,n); return 1;
}
static int write_memory(void *ctx,enum pvr_ccb_region r,uint32_t off,const void *in,size_t n)
{
    assert(ctx==&m && off<=pvr_ccb_region_bytes(r) && n<=pvr_ccb_region_bytes(r)-off);
    /* An unsuccessful write can already have changed the shared allocation. */
    memcpy(m.memory[r]+off,in,n); return operation();
}
static int barrier(void *ctx,enum pvr_ccb_barrier kind)
{ assert(ctx==&m); (void)kind; return operation(); }
static void complete(void)
{
    unsigned next=word(m.memory[PVR_CCB_HOST_CONTROL]),slot=(next-1)&127;
    put(m.memory[PVR_CCB_HOST_CONTROL]+128,next);
    m.cached_translation=m.device_table;
    put(m.memory[PVR_CCB_RETURNS]+slot*4,m.status); m.irq=1;
}
static int kick(void *ctx,uint32_t reg,uint32_t value)
{
    assert(ctx==&m && reg==0xb00 && !value && m.publishes==1);
    assert(m.device_table==m.host_table);
    unsigned next=word(m.memory[PVR_CCB_HOST_CONTROL]);
    const unsigned char *command=m.memory[PVR_CCB_HOST_COMMANDS]+((next-1)&127)*88;
    assert(!memcmp(command,f.command,88)); ++m.kicks;
    if(m.automatic)complete();
    return operation();
}
static int now(void *ctx,uint64_t *out)
{ assert(ctx==&m); if(!operation())return 0; *out=m.now; return 1; }
static int publish(void *ctx)
{
    assert(ctx==&m && !m.publishes && !m.kicks);
    ++m.publishes; m.device_table=m.host_table;
    if(m.late_publish)m.now=f.deadline;
    return operation();
}
static int irq_read(void *ctx,uint32_t reg,uint32_t *out)
{
    assert(ctx==&m && reg==0x8a8); if(!operation())return 0; *out=m.irq;
    if(++m.irq_reads==2 && m.rewind_after_irq)m.now=f.last_now;
    return 1;
}
static int irq_write(void *ctx,uint32_t reg,uint32_t value)
{ assert(ctx==&m && reg==0x8b0 && value==1); m.irq=0; return operation(); }
static int event(void *ctx,const unsigned char *data)
{ assert(ctx==&m && word(data)==(PVR_CCB_MAGIC|106)); ++m.events; return operation(); }
static const struct pvr_ccb_ops qops={read_memory,write_memory,barrier,kick};
static const struct pvr_gpu_flush_ops ops={now,publish};
static void prepare(unsigned level)
{
    memset(&m,0,sizeof(m)); memset(&q,0,sizeof(q)); memset(&c,0,sizeof(c)); memset(&f,0,sizeof(f));
    assert(pvr_ccb_init(&q,&qops,&m,29)==PVR_CCB_OK);
    assert(pvr_ccb_start(&q,100)==PVR_CCB_OK);
    /* Startup/compatibility is an established precondition, checked elsewhere. */
    c.queue=&q; c.context=&m; c.phase=PVR_GPU_CONTROL_READY; c.last_now=100;
    c.ops=(struct pvr_gpu_control_ops){.now=now,.read32=irq_read,.write32=irq_write,.event=event};
    m.now=100; m.calls=0; m.automatic=1; m.status=1;
    m.host_table=0x2222; m.device_table=m.cached_translation=0x1111;
    assert(pvr_gpu_flush_init(&f,&c,&ops,&m,(enum pvr_gpu_flush_level)level,5000));
    assert(!m.calls);
}
static enum pvr_gpu_flush_result run(void)
{
    for(unsigned i=0;i<200;++i) {
        enum pvr_gpu_flush_result r=pvr_gpu_flush_poll(&f);
        if(r!=PVR_GPU_FLUSH_AGAIN)return r;
    }
    assert(0); return PVR_GPU_FLUSH_INVALID;
}
static void retained(void)
{
    assert(f.phase==PVR_GPU_FLUSH_FAILED && c.phase==PVR_GPU_CONTROL_FAILED && q.phase==PVR_CCB_FAILED);
    unsigned calls=m.calls;
    assert(pvr_gpu_flush_poll(&f)==PVR_GPU_FLUSH_FAULT);
    assert(pvr_gpu_control_poll(&c,4)==PVR_GPU_CONTROL_FAULT);
    assert(pvr_ccb_poll(&q,m.now)==PVR_CCB_FAULT && calls==m.calls);
    struct pvr_gpu_flush before=f;
    assert(!pvr_gpu_flush_init(&f,&c,&ops,&m,PVR_GPU_FLUSH_LEAF,9000));
    assert(!memcmp(&f,&before,sizeof(f))); ++faults;
}
int main(int argc,char **argv)
{
    assert(argc==2 || (argc==3 && !strcmp(argv[2],"clock-check")));
    if(argc==3) {
        prepare(2); m.rewind_after_irq=1;
        assert(run()==PVR_GPU_FLUSH_FAULT && f.error==PVR_GPU_FLUSH_CLOCK);
        retained(); puts("PASS clock regression after control service cannot report flush completion");
        return 0;
    }
    char path[4096];
    assert(snprintf(path,sizeof(path),"%s/commands.bin",argv[1])<(int)sizeof(path));
    FILE *out=fopen(path,"wb"); assert(out);
    unsigned successful_calls=0;
    for(unsigned level=0;level<3;++level) {
        prepare(level); assert(fwrite(f.command,1,88,out)==88);
        assert(run()==PVR_GPU_FLUSH_OK && m.publishes==1 && m.kicks==1 && f.status==1);
        assert(m.cached_translation==0x2222); successful_calls=m.calls;
        unsigned calls=m.calls; assert(pvr_gpu_flush_poll(&f)==PVR_GPU_FLUSH_OK && m.calls==calls);
    }
    assert(!fclose(out));
    /* Queue read progress without a return word cannot complete invalidation. */
    prepare(2); m.automatic=0;
    assert(pvr_gpu_flush_poll(&f)==PVR_GPU_FLUSH_AGAIN);
    assert(pvr_gpu_flush_poll(&f)==PVR_GPU_FLUSH_AGAIN && m.kicks==1);
    put(m.memory[PVR_CCB_HOST_CONTROL]+128,1);
    assert(pvr_gpu_flush_poll(&f)==PVR_GPU_FLUSH_AGAIN && f.phase==PVR_GPU_FLUSH_WAIT);
    assert(m.cached_translation==0x1111);
    complete(); assert(run()==PVR_GPU_FLUSH_OK && m.kicks==1);

    /* A full ring makes bounded progress without re-publishing the tables. */
    prepare(1); q.host_write=127; put(m.memory[PVR_CCB_HOST_CONTROL],127);
    assert(pvr_gpu_flush_poll(&f)==PVR_GPU_FLUSH_AGAIN);
    assert(pvr_gpu_flush_poll(&f)==PVR_GPU_FLUSH_AGAIN && !m.kicks && m.publishes==1);
    put(m.memory[PVR_CCB_HOST_CONTROL]+128,127);
    assert(run()==PVR_GPU_FLUSH_OK && m.kicks==1 && m.publishes==1 && q.host_write==0);

    /* Firmware events are serviced with the same bounded control worker. */
    prepare(0);
    for(unsigned i=0;i<9;++i)put(m.memory[PVR_CCB_FIRMWARE_COMMANDS]+88*i,PVR_CCB_MAGIC|106);
    put(m.memory[PVR_CCB_FIRMWARE_CONTROL],9);
    assert(run()==PVR_GPU_FLUSH_OK && m.events==4);
    assert(word(m.memory[PVR_CCB_FIRMWARE_CONTROL]+128)==4);

    for(unsigned at=1;at<=successful_calls;++at) {
        prepare(2); m.fail_at=at;
        assert(run()==PVR_GPU_FLUSH_FAULT && m.calls==at); retained();
    }
    for(unsigned status=3;status<=5;status+=2) {
        prepare(2); m.status=status; assert(run()==PVR_GPU_FLUSH_FAULT);
        assert(f.error==PVR_GPU_FLUSH_COMMAND && f.status==status); retained();
    }
    prepare(0); m.now=98; assert(run()==PVR_GPU_FLUSH_FAULT && f.error==PVR_GPU_FLUSH_CLOCK); retained();
    prepare(0); m.late_publish=1; assert(run()==PVR_GPU_FLUSH_FAULT && !m.kicks);
    assert(f.error==PVR_GPU_FLUSH_TIMEOUT); retained();
    prepare(0); m.automatic=0;
    assert(pvr_gpu_flush_poll(&f)==PVR_GPU_FLUSH_AGAIN);
    assert(pvr_gpu_flush_poll(&f)==PVR_GPU_FLUSH_AGAIN); m.now=f.deadline;
    assert(run()==PVR_GPU_FLUSH_FAULT && f.error==PVR_GPU_FLUSH_TIMEOUT); retained();
    prepare(0); ++q.epoch; assert(run()==PVR_GPU_FLUSH_FAULT && !m.calls); retained();
    prepare(0); c.phase=PVR_GPU_CONTROL_RETIRED;
    assert(run()==PVR_GPU_FLUSH_FAULT && !m.calls); retained();
    prepare(0); put(m.memory[PVR_CCB_FIRMWARE_COMMANDS],PVR_CCB_MAGIC|109);
    put(m.memory[PVR_CCB_FIRMWARE_CONTROL],1);
    assert(run()==PVR_GPU_FLUSH_FAULT && f.error==PVR_GPU_FLUSH_CONTROL); retained();
    prepare(0); struct pvr_gpu_flush unchanged=f;
    assert(!pvr_gpu_flush_init(&f,&c,&ops,&m,PVR_GPU_FLUSH_LEAF,5000));
    assert(!memcmp(&f,&unchanged,sizeof(f)));
    memset(&f,0,sizeof(f)); unchanged=f;
    assert(!pvr_gpu_flush_init(&f,&c,&ops,&m,(enum pvr_gpu_flush_level)3,5000));
    assert(!pvr_gpu_flush_init(&f,&c,&ops,&m,PVR_GPU_FLUSH_LEAF,100));
    struct pvr_gpu_flush_ops incomplete=ops; incomplete.publish=NULL;
    assert(!pvr_gpu_flush_init(&f,&c,&incomplete,&m,PVR_GPU_FLUSH_LEAF,5000));
    assert(!memcmp(&f,&unchanged,sizeof(f)) && !m.calls);
    assert(pvr_gpu_flush_poll(&f)==PVR_GPU_FLUSH_INVALID);
    printf("PASS translation-cache publication/completion, queue progress, bounded events, and %u retained faults\n",faults);
}
