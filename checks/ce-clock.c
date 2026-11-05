/* The CE clock bring-up (drivers/crypto/a733_ce_clock.c) against a register model that enforces the native write rules. */
#include "a733_ce_clock.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CCU UINT32_C(0x02002000)
struct reg {uintptr_t a;uint32_t v;};
struct model {
    struct a733_clock_leases registry;
    struct a733_clock_token token;
    struct reg regs[40];unsigned count;
    uint64_t now;
    unsigned held,retained,left,writes;
    uint32_t log[32][2];
};
static uint32_t *reg(struct model *m,uintptr_t a)
{
    for(unsigned i=0;i<m->count;++i)if(m->regs[i].a==a)return &m->regs[i].v;
    assert(m->count<40);m->regs[m->count]=(struct reg){a,0};return &m->regs[m->count++].v;
}
static int now(void *v,uint64_t *n){struct model *m=v;*n=++m->now;return 1;}
static int authorize(void *v){(void)v;return 1;}
static int enter(void *v,uint64_t d){struct model *m=v;(void)d;assert(!m->held);m->held=1;return 1;}
static void leave(void *v){struct model *m=v;assert(m->held);m->held=0;++m->left;}
static void retain(void *v){struct model *m=v;assert(m->held);++m->retained;}
static int read_reg(void *v,uintptr_t a,uint32_t *out){struct model *m=v;assert(m->held);*out=*reg(m,a);return 1;}
/* The native allowlist (a733_ce_native.c a733_ce_clock_native_write): the same transitions, up and down. */
static int write_reg(void *v,uintptr_t a,uint32_t value)
{
    struct model *m=v;assert(m->held);
    uint32_t old=*reg(m,a),mask;
    if(a==CCU+0xac0){
        mask=0x8700001f;uint32_t from=old&mask,to=value&mask;
        if(!((from==0&&to==0x01000000)||(from==0x01000000&&to==0x81000000)||
             (from==0x81000000&&to==0x01000000)||(from==0x01000000&&to==0)))return 0;
    }else if(a==CCU+0xac4){
        mask=0x30003;uint32_t from=old&mask,to=value&mask;
        if(!((from==0&&to==0x30000)||(from==0x30000&&to==0x30001)||(from==0x30001&&to==0x30003)||
             (from==0x30003&&to==0x30001)||(from==0x30001&&to==0x30000)||(from==0x30000&&to==0)))return 0;
    }else if(a==CCU+0x5e4){
        mask=4;if((old&4)==(value&4))return 0;
    }else return 0;
    if((old^value)&~mask)return 0;
    assert(m->writes<32);m->log[m->writes][0]=(uint32_t)(a-CCU);m->log[m->writes][1]=value;++m->writes;
    *reg(m,a)=value;return 1;
}
static void setup(struct model *m,uint32_t clk,uint32_t bgr,uint32_t mbus)
{
    memset(m,0,sizeof(*m));
    assert(a733_clock_leases_init(&m->registry,now,m));
    a733_clock_mask_t shared=A733_CLOCK_PERI0|A733_CLOCK_REF|A733_CLOCK_OSC|A733_CLOCK_FABRIC|A733_CLOCK_RC16M;
    assert(a733_clock_acquire(&m->registry,5,shared,A733_CLOCK_CE,1000000,&m->token)==A733_CLOCK_OK);
    *reg(m,0x07090160)=0x10000003;*reg(m,CCU)=0xf8000000|79u<<8|79u<<16;
    *reg(m,CCU+0xa0)=0xfe000000|99u<<8|1u<<20;
    *reg(m,CCU+0x1908)=0x0a060a06;*reg(m,CCU+0x1988)=0x0a060000;
    *reg(m,CCU+0xac0)=clk;*reg(m,CCU+0xac4)=bgr;*reg(m,CCU+0x5e4)=mbus;
}
static int provision(struct model *m,struct a733_ce_clock *clock)
{
    struct a733_ce_config c={.owner=5,.handoff=7,.deadline_us=1000000,.profile=A733_CE_TRNG_ONLY,
        .fabric={0x03000001,0x83000000,0x84000000},.fabric_hz={300000000,600000000,400000000}};
    for(unsigned i=0;i<3;++i)*reg(m,CCU+(unsigned[]){0x500,0x580,0x588}[i])=c.fabric[i];
    const struct a733_ce_clock_ops ops={authorize,now,enter,leave,retain,read_reg,write_reg};
    memset(clock,0,sizeof(*clock));
    return a733_ce_clock_provision(clock,&c,&ops,m,&m->registry,&m->token);
}
int main(void)
{
    struct model m;struct a733_ce_clock clock;
    /* The board's entry state: enabled and out of reset, the MBUS word with only the CE bit (queue entry of the first
     * entropy run: CE_CLK 0x81000000, CE_BGR 0x30003, CCU 0x5e4 = 4). Taken down, then brought up, in the vendor order. */
    setup(&m,0x81000000,0x30003,4);
    assert(provision(&m,&clock)==1&&clock.state==A733_CE_CLOCK_READY&&!m.held&&!m.retained);
    assert(*reg(&m,CCU+0xac0)==0x81000000&&*reg(&m,CCU+0xac4)==0x30003&&*reg(&m,CCU+0x5e4)==4);
    static const uint32_t order[][2]={
        {0xac4,0x30001},{0x5e4,0},{0xac0,0x01000000},{0xac4,0x30000},{0xac4,0},{0xac0,0},   /* the cycle, reversed bring-up */
        {0xac0,0x01000000},{0xac4,0x30000},{0xac4,0x30001},{0xac0,0x81000000},{0x5e4,4},{0xac4,0x30003}};
    assert(m.writes==12);
    for(unsigned i=0;i<12;++i)assert(m.log[i][0]==order[i][0]&&m.log[i][1]==order[i][1]);
    /* Neighbors are preserved wherever they stand: the older capture had bits 1, 8 and 18 set as well. */
    setup(&m,0x81000000,0x30003,0x40306);
    assert(provision(&m,&clock)==1&&*reg(&m,CCU+0x5e4)==0x40306&&m.writes==12);
    /* Held in reset and gated: brought up directly, six writes. */
    setup(&m,0,0,0x40302);
    assert(provision(&m,&clock)==1&&*reg(&m,CCU+0xac0)==0x81000000&&*reg(&m,CCU+0xac4)==0x30003&&
           *reg(&m,CCU+0x5e4)==0x40306&&m.writes==6);
    /* Half-enabled (resets released, gate off, clock source set): neither state; refused before any write, the lock
     * left and the token kept, and the failure names its source line. */
    setup(&m,0x01000000,0x30000,0);
    assert(!provision(&m,&clock)&&m.writes==0&&!m.held&&!m.retained&&m.left==1&&clock.fail_line);
    assert(a733_clock_token_status(&m.registry,&m.token)==A733_CLOCK_OK);
    puts("PASS CE clock bring-up: the board's enabled entry state is cycled in the vendor order, neighbors preserved, held-in-reset brought up, other states refused without a write");
    return 0;
}
