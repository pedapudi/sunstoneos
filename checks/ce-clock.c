/* The CE clock bring-up (drivers/crypto/a733_ce_clock.c) against a register model that enforces the native write rules. */
#include "a733_ce_clock.h"
#include "sunstone_crumb.h"
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
    uintptr_t stuck; /* A register whose writes are accepted but never take effect. */
    uintptr_t refuse; /* A register whose writes the callback refuses. */
    unsigned flip_after;uint32_t flip_bits; /* After that many writes, another writer flips these 0x5e4 bits. */
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
    if(a==m->refuse)return 0;
    assert(m->writes<32);m->log[m->writes][0]=(uint32_t)(a-CCU);m->log[m->writes][1]=value;++m->writes;
    if(a!=m->stuck)*reg(m,a)=value;
    if(m->flip_bits&&m->writes==m->flip_after)*reg(m,CCU+0x5e4)^=m->flip_bits;
    return 1;
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
    /* Retirement: a READY CE is taken down by the same reverse sequence, each step read back, and the lock left. */
    setup(&m,0x81000000,0x30003,4);
    assert(provision(&m,&clock)==1);
    unsigned before=m.writes,left=m.left;
    assert(a733_ce_clock_retire(&clock)==1&&clock.state==A733_CE_CLOCK_RETIRED&&!m.held&&!m.retained&&m.left==left+1);
    assert(m.writes==before+6&&*reg(&m,CCU+0xac0)==0&&*reg(&m,CCU+0xac4)==0&&*reg(&m,CCU+0x5e4)==0);
    for(unsigned i=0;i<6;++i)assert(m.log[before+i][0]==order[i][0]&&m.log[before+i][1]==order[i][1]);
    /* Retired is final: neither a second retirement nor a bring-up follows. */
    assert(!a733_ce_clock_retire(&clock)&&m.writes==before+6);
    /* Neighbors stay as found through the retirement too. */
    setup(&m,0,0,0x40302);
    assert(provision(&m,&clock)==1&&a733_ce_clock_retire(&clock)==1&&*reg(&m,CCU+0x5e4)==0x40302);
    /* A CE that is not in the state provision left (here its bus gate was closed behind the owner's back) is refused
     * before any write: the lock left, the clock still READY and the token unretained. */
    setup(&m,0,0,0);
    assert(provision(&m,&clock)==1);
    *reg(&m,CCU+0xac4)=0x30001;before=m.writes;left=m.left;
    assert(!a733_ce_clock_retire(&clock)&&clock.state==A733_CE_CLOCK_READY&&m.writes==before&&!m.held&&m.left==left+1&&!m.retained);
    assert(a733_clock_token_status(&m.registry,&m.token)==A733_CLOCK_OK);
    /* A write that does not read back (the module gate stuck on) retains the lock and the token: never released. */
    setup(&m,0,0,0);
    assert(provision(&m,&clock)==1);
    m.stuck=CCU+0xac0;before=m.writes;
    assert(!a733_ce_clock_retire(&clock)&&clock.state==A733_CE_CLOCK_RETAINED&&m.held&&m.retained==1);
    assert(a733_clock_token_status(&m.registry,&m.token)!=A733_CLOCK_OK);
    /* The first failure record's words. Another writer sets 0x5e4 bit 8 after the bring-up's last write: the bring-up
     * fails at its own 0x5e4 check line with both 0x5e4 words recorded, and the record's aux holds CE_CLK and CE_BGR. */
    setup(&m,0x81000000,0x30003,4);m.flip_after=12;m.flip_bits=0x100;
    assert(!provision(&m,&clock)&&clock.fail_line==CE_LINE_MBUS_UP&&clock.state==A733_CE_CLOCK_RETAINED&&m.retained==1);
    assert(clock.entry_mbus==4&&clock.seen_mbus==0x104&&clock.seen_clk==0x81000000&&clock.seen_bgr==0x30003&&!clock.write_refused);
    assert(a733_ce_clock_fail_aux(&clock)==(3u|12u|1u<<9|1u<<12)); /* BGR bits 0, 1, 16, 17; CLK bits 24 and 31 */
    /* The words apps/os/ce_probe.c gives board_fail_note, for the decoder case in checks/ce-clock.py. */
    printf("RECORD %#x %#x %#x\n",SUNSTONE_CRUMB_FAIL_CE<<12|(clock.fail_line&0xfffu)|a733_ce_clock_fail_aux(&clock)<<16,
        clock.seen_mbus,clock.entry_mbus);
    /* A bring-up write that does not read back fails at that step's own line, and is not a refused write. */
    setup(&m,0x81000000,0x30003,4);m.stuck=CCU+0x5e4;
    assert(!provision(&m,&clock)&&!clock.write_refused);
    assert(clock.fail_line==CE_LINE_CYCLE_MBUS&&clock.seen_mbus==4); /* the cycle's MBUS write, the first 0x5e4 write */
    /* A write the callback refuses is marked as refused, at the same step's line. */
    setup(&m,0,0,0);m.refuse=CCU+0xac4;
    assert(!provision(&m,&clock)&&clock.write_refused&&clock.fail_line==CE_LINE_BGR_RESETS);
    assert(a733_ce_clock_fail_aux(&clock)>>13==1u&&(a733_ce_clock_fail_aux(&clock)>>4&0x1ffu)==1u<<5);
    puts("PASS CE clock bring-up and retirement: the board's enabled entry state is cycled in the vendor order, neighbors preserved, held-in-reset brought up, other states refused without a write; a ready CE retires by the reverse sequence, a moved state is refused without a write, a failed readback retains; each failing step has its own line and the CE words for the first failure record");
    return 0;
}
