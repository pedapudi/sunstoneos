/* SPDX-License-Identifier: Apache-2.0 */
/* Actual firmware CCB/controller with modeled ordered firmware responses. */
#include "pvr_gpu_clock.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct {unsigned char mem[5][128*88];uint64_t now;unsigned sync,state,sent,clears,rate;
    unsigned no_ack,no_read,bad_ack,bad_state,fail_publish,late_publish,status;} m;
static struct pvr_ccb q;
static struct pvr_gpu_control c;
static struct pvr_gpu_clock clock;
static uint32_t word(const unsigned char *p){uint32_t v;memcpy(&v,p,4);return v;}
static void put(unsigned char *p,uint32_t v){memcpy(p,&v,4);}
static int read_mem(void *v,enum pvr_ccb_region r,uint32_t off,void *out,size_t n)
{assert(v==&m && off+n<=pvr_ccb_region_bytes(r));memcpy(out,m.mem[r]+off,n);return 1;}
static int write_mem(void *v,enum pvr_ccb_region r,uint32_t off,const void *in,size_t n)
{assert(v==&m && off+n<=pvr_ccb_region_bytes(r));memcpy(m.mem[r]+off,in,n);return 1;}
static int barrier(void *v,enum pvr_ccb_barrier b){assert(v==&m);(void)b;return 1;}
static int kick(void *v,uint32_t off,uint32_t value)
{
    assert(v==&m && off==0xb00 && !value && m.sent<3);
    unsigned next=word(m.mem[0]),slot=(next-1)&127;
    unsigned char *cmd=m.mem[1]+slot*88;
    if(m.sent==1)assert(word(cmd)==(PVR_CCB_MAGIC|NATIVE_CORECLK_COMMAND) && word(cmd+8)==m.rate && m.rate==600000000 && m.state==2);
    else {
        assert(word(cmd)==(PVR_CCB_MAGIC|NATIVE_POWER_COMMAND) && word(cmd+8)==2 && word(cmd+12)==(m.sent?2u:1u));
        assert(!m.sync && m.clears==(m.sent?2u:1u));
        if(!m.no_ack)m.sync=m.bad_ack?7:1;
        m.state=m.sent?3:(m.bad_state?1:2); /* IDLE after cancel is allowed. */
    }
    if(!m.no_read)put(m.mem[0]+128,next);
    put(m.mem[2]+slot*4,m.status);++m.sent;return 1;
}
static int now(void *v,uint64_t *t){assert(v==&m);*t=m.now++;return 1;}
static int irq(void *v,uint32_t off,uint32_t *out){assert(v==&m && off==0x8a8);*out=0;return 1;}
static int irq_write(void *v,uint32_t off,uint32_t value){(void)v;(void)off;(void)value;assert(0);return 0;}
static int compat(void *v,uint32_t off,void *out,size_t n){(void)v;(void)off;(void)out;(void)n;assert(0);return 0;}
static int sync_write(void *v,uint32_t value){assert(v==&m && !value);m.sync=0;++m.clears;return 1;}
static int sync_read(void *v,uint32_t *value){assert(v==&m);*value=m.sync;return 1;}
static int state(void *v,uint32_t *value){assert(v==&m);*value=m.state;return 1;}
static int publish(void *v,uint32_t hz){assert(v==&m && m.state==2);m.rate=hz;if(m.late_publish)m.now=clock.deadline;return !m.fail_publish;}
static const struct pvr_ccb_ops qops={read_mem,write_mem,barrier,kick};
static const struct pvr_gpu_control_ops cops={now,irq,irq_write,compat,0};
static const struct pvr_gpu_clock_ops ops={now,sync_write,sync_read,state,publish};
static void setup(void)
{
    memset(&m,0,sizeof(m));memset(&q,0,sizeof(q));memset(&c,0,sizeof(c));memset(&clock,0,sizeof(clock));
    assert(pvr_ccb_init(&q,&qops,&m,21)==PVR_CCB_OK && pvr_ccb_start(&q,0)==PVR_CCB_OK);
    c.ops=cops;c.context=&m;c.queue=&q;c.phase=PVR_GPU_CONTROL_READY;m.sync=1;m.state=1;m.status=1;
    assert(pvr_gpu_clock_hold(&clock,&c,&ops,&m,1000,2000));
}
static int run(void){for(unsigned i=0;i<2000;++i){int r=pvr_gpu_clock_poll(&clock);if(r)return r;}assert(0);return 0;}
static void retained(void){assert(clock.phase==PVR_GPU_CLOCK_FAILED && q.phase==PVR_CCB_FAILED && c.phase==PVR_GPU_CONTROL_FAILED);}
int main(void)
{
    unsigned cases=0;
    setup();assert(run()==1 && clock.phase==PVR_GPU_CLOCK_HELD && m.sent==1);
    assert(pvr_gpu_clock_resume(&clock,600000000,1500) && run()==1 && clock.phase==PVR_GPU_CLOCK_READY && m.sent==3 && m.clears==2 && !clock.published);++cases;
    setup();m.no_ack=1;assert(run()==-1 && m.sent==1 && clock.published==1);retained();++cases;
    setup();m.no_read=1;m.status=0;assert(run()==-1 && m.sent==1 && clock.published==1);retained();++cases;
    setup();m.bad_ack=1;assert(run()==-1);retained();++cases;
    setup();m.bad_state=1;assert(run()==-1);retained();++cases;
    setup();assert(run()==1);m.no_ack=1;assert(pvr_gpu_clock_resume(&clock,600000000,1500) && run()==-1 && m.sent==3);retained();++cases;
    setup();assert(run()==1);m.fail_publish=1;assert(pvr_gpu_clock_resume(&clock,600000000,1500) && run()==-1 && m.sent==1);retained();++cases;
    setup();assert(run()==1);m.late_publish=1;assert(pvr_gpu_clock_resume(&clock,600000000,1500) && run()==-1 && m.sent==1);retained();++cases;
    setup();assert(run()==1);m.now=2000;assert(pvr_gpu_clock_poll(&clock)==-1);retained();++cases;
    printf("PASS GPU clock protocol: %u cases; fresh idle/cancel acknowledgements, ordered completion, post-clock publication and retained failures\n",cases);return 0;
}
