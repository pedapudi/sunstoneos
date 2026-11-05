/* The CE probe's state machine against fake operations (include/a733_ce_probe.h). */
#include "a733_ce_probe.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct fake {
    uint64_t now;
    uint32_t clk,bgr,mbus,status,error,lpc;
    int bring_up_steps,alloc_steps,configure_steps,request_steps,poll_steps,fail_at;
    unsigned retained,bring_ups,polls;
    uint8_t block[32];
    int fail_poll;
};
static int now(void *v,uint64_t *n){struct fake *f=v;*n=f->now;return 1;}
static int read(void *v,uintptr_t a,uint32_t *out)
{
    struct fake *f=v;
    switch(a){
    case 0x02002ac0:*out=f->clk;return 1;case 0x02002ac4:*out=f->bgr;return 1;case 0x020025e4:*out=f->mbus;return 1;
    case 0x0460300c:*out=f->status;return 1;case 0x04603018:*out=f->error;return 1;case 0x046030d0:*out=f->lpc;return 1;
    }
    return 0;
}
static int bring_up(void *v)
{struct fake *f=v;++f->bring_ups;if(f->fail_at==1)return -1;if(f->bring_up_steps-->0)return 0;f->clk=0x81000000;f->bgr=0x30003;return 1;}
static int alloc(void *v,uint64_t *d,uint64_t *o){struct fake *f=v;if(f->fail_at==2)return -1;if(f->alloc_steps-->0)return 0;*d=0x41000000;*o=0x41000100;return 1;}
static int configure(void *v){struct fake *f=v;if(f->fail_at==3)return -1;return f->configure_steps-->0?0:1;}
static int request(void *v,uint64_t d){struct fake *f=v;(void)d;if(f->fail_at==4)return -1;return f->request_steps-->0?0:1;}
static int poll(void *v,uint8_t b[32])
{
    struct fake *f=v;++f->polls;f->now+=1000;
    if(f->fail_poll)return -1;
    if(f->poll_steps-->0)return 0;
    memcpy(b,f->block,32);return 1;
}
static void retain(void *v){((struct fake *)v)->retained++;}
static int run(struct a733_ce_probe *p,struct fake *f,unsigned limit)
{
    struct a733_ce_probe_ops ops={f,now,read,bring_up,alloc,configure,request,poll,retain};
    a733_ce_probe_init(p,&ops);
    for(unsigned i=0;i<limit;++i){f->now+=100;int r=a733_ce_probe_step(p);if(r)return r;}
    return 0;
}
int main(int argc,char **argv)
{
    struct a733_ce_probe p;struct fake f;
    /* The board's entry state (CE enabled), two pending polls, a nonzero block. */
    memset(&f,0,sizeof(f));f.now=1000;f.clk=0x81000000;f.bgr=0x30003;f.mbus=0x40306;f.lpc=2;f.bring_up_steps=1;f.poll_steps=2;
    for(unsigned i=0;i<32;++i)f.block[i]=(uint8_t)(i+1);
    assert(run(&p,&f,40)==1);
    const struct a733_ce_probe_record *r=&p.record;
    assert(r->magic==A733_CE_PROBE_MAGIC&&r->version==1&&r->bytes==sizeof(*r)&&!r->failed&&r->stage==A733_CE_PROBE_DONE);
    assert(r->entry_clk==0x81000000&&r->entry_bgr==0x30003&&r->entry_mbus==0x40306);
    assert(r->clocked_clk==0x81000000&&r->clocked_bgr==0x30003);
    assert((r->flags&1)&&(r->flags&2)&&(r->flags&4)&&(r->flags&8)&&(r->flags&16)); /* defaults, found enabled, done, nonzero, source set */
    assert(r->bytes_returned==32&&r->task_us>=2000&&r->task_us<=3000&&r->step_limit_us==2000000);
    assert(r->descriptor_physical==0x41000000&&r->output_physical==0x41000100&&!f.retained);
    if(argc==2){FILE *out=fopen(argv[1],"wb");assert(out&&fwrite(r,1,sizeof(*r),out)==sizeof(*r));fclose(out);}
    /* The record holds no random byte. */
    const uint8_t *raw=(const uint8_t *)r;
    for(size_t i=0;i+8<=sizeof(*r);++i)assert(memcmp(raw+i,f.block,8)); /* no eight-byte run of the block */
    /* Held in reset at entry: no "found enabled" flag; an all-zero block clears the nonzero flag. */
    memset(&f,0,sizeof(f));f.now=1000;f.lpc=2;
    assert(run(&p,&f,40)==1&&!(p.record.flags&2)&&!(p.record.flags&8)&&(p.record.flags&4));
    /* Failure at each stage retains once and names the stage that did not complete. */
    for(int at=1;at<=4;++at){
        memset(&f,0,sizeof(f));f.now=1000;f.fail_at=at;
        assert(run(&p,&f,40)<0&&f.retained==1&&p.record.failed&&p.record.failure_stage==(unsigned)at+1);
        assert(a733_ce_probe_step(&p)<0&&f.retained==1); /* it stays failed and does not retain again */
    }
    memset(&f,0,sizeof(f));f.now=1000;f.fail_poll=1;
    assert(run(&p,&f,40)<0&&f.retained==1&&p.record.failure_stage==A733_CE_PROBE_REQUESTED+1);
    /* A task that never completes ends at the 2 s bound. */
    memset(&f,0,sizeof(f));f.now=1000;f.poll_steps=1000000;
    int result=0;
    for(unsigned i=0;i<10000&&!result;++i){if(!i){struct a733_ce_probe_ops ops={&f,now,read,bring_up,alloc,configure,request,poll,retain};a733_ce_probe_init(&p,&ops);}
        f.now+=500000;result=a733_ce_probe_step(&p);}
    assert(result<0&&f.retained==1&&p.record.failed&&p.record.stage==A733_CE_PROBE_REQUESTED);
    puts("PASS CE probe state machine: found-enabled entry, vendor stage order, no random byte kept, failure at each stage, 2 s bound");
    return 0;
}
