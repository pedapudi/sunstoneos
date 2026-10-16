#include "a733_spi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct model {
    uintptr_t base;
    uint32_t r[0x404/4];
    uint8_t tx[64],rx[192],wire[1024];
    unsigned tn,rn,ti,ri,wire_n,burst,tx_target,total,received;
    unsigned enabled,admit,stalled,buffer_held,early_done,corrupt_rx;
    unsigned uncertain_tx,uncertain_rx,uncertain_start,late_tx;
    unsigned cs_asserted,cs_completed,expected_high;
    uint64_t now,cs_at,done_at;
    unsigned setup_us,hold_us;
};
static void tick(struct model *m)
{
    if(m->r[1]&0x80000000u) { m->r[1]=0x80;m->r[6]&=~0x80008000u; }
    m->r[6]&=~0x80008000u;
    if(!(m->r[2]&0x80000000u)||m->stalled)return;
    if(m->rn==192)return;
    if(m->burst<m->tx_target && !m->tn)return;
    assert(m->cs_asserted);
    uint8_t b=0;
    if(m->burst<m->tx_target) {
        b=m->tx[m->ti++%64];m->tn--;
        assert(m->wire_n<sizeof(m->wire));m->wire[m->wire_n++]=b;
    }
    if(!(m->r[2]&0x100u)||m->burst>=m->tx_target) {
        m->rx[(m->ri+m->rn)%192]=(uint8_t)(b^0xa5u^(m->received++));m->rn++;
    }
    m->burst++;
    if(m->burst==m->total) {m->r[2]&=~0x80000000u;m->r[5]|=0x1000;m->done_at=m->now;}
}
static int now(void *p,uint64_t *v) {struct model *m=p;tick(m);*v=++m->now;return 1;}
static int admit(void *p,const struct a733_spi_format *f,const struct a733_spi_plan *plan)
{ assert(f&&plan);return ((struct model*)p)->admit; }
static int rd(void *p,uintptr_t a,uint32_t *v)
{
    struct model *m=p;assert(a>=m->base&&a-m->base<=0x400);
    unsigned o=(unsigned)(a-m->base);
    if(o==0x1c)*v=(m->tn<<16)|(m->corrupt_rx?193:m->rn);
    else if(o==0x400)*v=m->buffer_held?0x1000000:0;
    else *v=m->r[o/4];
    return 1;
}
static int wr(void *p,uintptr_t a,uint32_t v)
{
    struct model *m=p;unsigned o=(unsigned)(a-m->base);assert(o<0x44);
    if(o==0x14){m->r[5]&=~v;return 1;}
    if(o==0x08) {
        unsigned cs=!!(v&0x80u)==m->expected_high;
        if(m->cs_asserted&&!cs) {
            assert(m->burst==m->total&&!(m->r[2]&0x80000000u)&&!m->tn&&!m->rn&&!m->buffer_held);
            assert(m->now-m->done_at>=m->hold_us);m->cs_completed++;
        }
        if(!m->cs_asserted&&cs)m->cs_at=m->now;
        m->cs_asserted=cs;
        if(v&0x80000000u) {
            assert(m->now-m->cs_at>=m->setup_us);
            assert(!(m->r[2]&0x80000000u));m->total=m->r[0x30/4];
            m->tx_target=m->r[0x34/4];m->burst=0;m->received=0;
            if(m->uncertain_start){m->r[o/4]=v;return 0;}
        }
    }
    m->r[o/4]=v;return 1;
}
static int rb(void *p,uintptr_t a,uint8_t *v)
{
    struct model *m=p;assert(a==m->base+0x300&&m->rn);
    *v=m->rx[m->ri++%192];m->rn--;return !m->uncertain_rx;
}
static int wb(void *p,uintptr_t a,uint8_t v)
{
    struct model *m=p;assert(a==m->base+0x200&&m->tn<64);
    m->tx[(m->ti+m->tn)%64]=v;m->tn++;
    if(m->late_tx)m->now=100000;
    return !m->uncertain_tx;
}
static const struct a733_spi_ops ops={admit,rd,wr,rb,wb,now};
static void start(struct a733_spi *s,struct model *m,unsigned port,unsigned mode,unsigned high,unsigned lsb)
{
    memset(s,0,sizeof(*s));memset(m,0,sizeof(*m));m->admit=1;m->expected_high=high;m->setup_us=3;m->hold_us=5;
    struct a733_spi_format f={port,port==3?1u:0u,mode,lsb,high,500000,3,5};
    struct a733_spi_plan plan;assert(!a733_spi_plan(port,f.max_hz,&plan));m->base=plan.base;
    assert(a733_spi_begin(s,&ops,m,&f,100000)==A733_SPI_RESETTING);
    for(unsigned i=0;i<8&&s->state!=A733_SPI_READY;i++)a733_spi_poll(s,7);
    assert(s->state==A733_SPI_READY);
    assert((m->r[2]&3)==mode&&!!(m->r[2]&0x1000)==lsb);
}
static void finish(struct a733_spi *s)
{
    for(unsigned i=0;i<4000&&s->state!=A733_SPI_COMPLETE&&s->state!=A733_SPI_FAULT;i++)a733_spi_poll(s,7);
    assert(s->state==A733_SPI_COMPLETE);
}
static void transfers(void)
{
    uint8_t tx[193],rx[193];for(unsigned i=0;i<193;i++)tx[i]=(uint8_t)(i*17+3);
    for(unsigned kind=0;kind<4;kind++) {
        struct a733_spi s;struct model m;start(&s,&m,3,kind,kind&1,kind>>1);
        struct a733_spi_transfer t={kind,tx,rx,193,193};
        if(kind==A733_SPI_WRITE){t.rx=0;t.rx_bytes=0;}
        if(kind==A733_SPI_READ){t.tx=0;t.tx_bytes=0;}
        if(kind==A733_SPI_WRITE_READ){t.tx_bytes=7;t.rx_bytes=180;}
        memset(rx,0,sizeof(rx));assert(a733_spi_submit(&s,&t,100000)==A733_SPI_SETUP);finish(&s);
        assert(s.sent==t.tx_bytes&&s.received==t.rx_bytes&&m.cs_completed==1);
        assert(!memcmp(m.wire,tx,t.tx_bytes));
        for(unsigned i=0;i<t.rx_bytes;i++)assert(rx[i]==(uint8_t)((kind==0?tx[i]:0)^0xa5u^i));
        assert(a733_spi_reap(&s)==A733_SPI_READY&&!s.transfer.tx&&!s.transfer.rx);
        /* Reuse the owned controller without reset or per-message FIFO discard. */
        assert(a733_spi_submit(&s,&t,100000)==A733_SPI_SETUP);finish(&s);
        assert(m.cs_completed==2);assert(a733_spi_reap(&s)==A733_SPI_READY);
        assert(a733_spi_close(&s,100000)==A733_SPI_CLOSED);
    }
}
static void receive_capacity(void)
{
    struct a733_spi s;struct model m;start(&s,&m,3,0,0,0);
    uint8_t rx[192];struct a733_spi_transfer t={A733_SPI_READ,0,rx,0,192};
    assert(a733_spi_submit(&s,&t,100000)==A733_SPI_SETUP);
    while(s.state==A733_SPI_SETUP)a733_spi_poll(&s,7);
    for(unsigned i=0;i<192;i++)tick(&m);
    assert(m.rn==192);finish(&s);
    for(unsigned i=0;i<192;i++)assert(rx[i]==(uint8_t)(0xa5u^i));
}
static void faults(void)
{
    uint8_t tx[140]={0},rx[140]={0};
    for(unsigned k=0;k<8;k++) {
        struct a733_spi s;struct model m;start(&s,&m,3,0,0,0);
        struct a733_spi_transfer t={A733_SPI_FULL_DUPLEX,tx,rx,140,140};
        assert(a733_spi_submit(&s,&t,100000)==A733_SPI_SETUP);
        if(k==0)m.uncertain_tx=1;
        if(k==1)m.uncertain_rx=1;
        if(k==2)m.uncertain_start=1;
        if(k==3)m.late_tx=1;
        if(k==4)m.admit=0;
        if(k==5)m.corrupt_rx=1;
        if(k==6)m.r[5]=0x1100; /* completion and error together */
        if(k==7)m.buffer_held=1;
        for(unsigned i=0;i<4000&&s.state!=A733_SPI_FAULT;i++) {
            if(k==7&&s.received==140)m.now=99999;
            a733_spi_poll(&s,7);
        }
        assert(s.state==A733_SPI_FAULT&&s.transfer.tx==tx&&s.transfer.rx==rx);
        assert(!m.cs_completed);unsigned sent=s.sent,got=s.received;
        assert(a733_spi_poll(&s,64)==A733_SPI_FAULT&&s.sent==sent&&s.received==got);
        assert(a733_spi_reap(&s)==-1);
    }
    struct a733_spi s;struct model m;start(&s,&m,0,0,0,0);
    struct a733_spi_transfer t={A733_SPI_FULL_DUPLEX,tx,tx,1,1};
    uint64_t before=m.now;assert(a733_spi_submit(&s,&t,100000)==-1&&m.now==before);
    t.rx=rx;t.tx_bytes=0xffffff;t.rx_bytes=1;
    assert(a733_spi_submit(&s,&t,100000)==-1&&m.now==before);
    t.tx_bytes=1;t.rx_bytes=1;assert(a733_spi_submit(&s,&t,1)==A733_SPI_FAULT);
}
static void clocks(void)
{
    struct a733_spi_plan invalid;assert(a733_spi_plan(3,24000,&invalid)==-1);
    for(unsigned p=0;p<6;p++) {
        struct a733_spi s;struct model m;start(&s,&m,p,0,0,0);
        assert(s.plan.intid==(p==5?242:56+p));
        assert(a733_spi_close(&s,100000)==A733_SPI_CLOSED);
    }
    const uint32_t rates[]={24194,500000,999999,12000000,24000000};
    for(unsigned i=0;i<sizeof(rates)/sizeof(rates[0]);i++) {
        struct a733_spi_plan p;assert(!a733_spi_plan(3,rates[i],&p));
        assert(24000000ull<=(uint64_t)rates[i]*p.divisor);
        for(unsigned m=1;m<=32;m++)for(unsigned n=1;n<=32;n++)
            assert((uint64_t)rates[i]*m*n<24000000ull||m*n>=p.divisor);
    }
}
int main(void){clocks();transfers();receive_capacity();faults();puts("SPI clock, wire transfers, retained faults and FIFO completion checks passed");}
