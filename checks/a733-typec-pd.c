/* SPDX-License-Identifier: GPL-2.0-only */
/* Executed real byte/block TWI, CC monitor, PD service and token APIs.
 * MMIO, cable, electrical qualification and task authority are modeled. */
#include "a733_typec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct model {
    uint64_t now;uint32_t control,status,data;
    unsigned address,reg,stage,receiving,pending,restart,starts,stops,acks,nacks;
    unsigned rxhead,rxtail,txlen,rxid,txcount,wrong_rdo,nak,drop,off,pin,corrupt,duplicates,malformed,time_reads,jump_after,jump_us;
    uint8_t regs[256],rx[8192],tx[40];
};
static uint32_t word(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void enqueue(struct model *m,unsigned h,const uint32_t *v,unsigned n)
{
    assert(m->rxtail+35<sizeof(m->rx));m->rx[m->rxtail++]=0xe0;
    m->rx[m->rxtail++]=(uint8_t)h;m->rx[m->rxtail++]=(uint8_t)(h>>8);
    for(unsigned i=0;i<n;i++)for(unsigned b=0;b<4;b++)m->rx[m->rxtail++]=(uint8_t)(v[i]>>(8*b));
    uint32_t crc=UINT32_MAX;unsigned begin=m->rxtail-2-4*n;
    for(unsigned i=begin;i<m->rxtail;i++){crc^=m->rx[i];for(unsigned bit=0;bit<8;bit++)crc=(crc>>1)^((crc&1)?0xedb88320u:0);}
    crc=~crc;for(unsigned b=0;b<4;b++)m->rx[m->rxtail++]=(uint8_t)(crc>>(8*b));
}
static void peer(struct model *m)
{
    assert(m->txlen>=11&&m->tx[0]==0x12&&m->tx[1]==0x12&&m->tx[2]==0x12&&m->tx[3]==0x13);
    unsigned h=m->tx[5]|(unsigned)m->tx[6]<<8,n=(h>>12)&7;
    assert(m->txlen==11+4*n&&m->tx[4]==(0x82u+4*n));
    assert((h&0x1e0)==0x160);++m->txcount;
    if(m->drop)return;
    enqueue(m,1|0x40|(h&0xe00),NULL,0);
    if((h&31)==1){uint32_t v=0x10000000u|(50u<<10)|(m->wrong_rdo?51u:50u);enqueue(m,2|0x40|((m->rxid++&7)<<9)|(1<<12),&v,1);}
    if((h&31)==15){
        uint32_t v[4]={word(m->tx+7)|0x40u,0,0,0};unsigned cmd=v[0]&31,k=1;
        if(m->nak){v[0]=(v[0]&~0xc0u)|0x80u;}
        else switch(cmd){
        case 1:k=4;v[1]=1u<<26;break;
        case 2:k=2;v[1]=0xff010000u;break;
        case 3:k=2;v[1]=((m->pin?m->pin:4u)<<16)|0x45u;break;
        case 16:k=2;v[1]=0x82;break;
        case 17:assert(word(m->tx+11)==(((m->pin?m->pin:4u)<<8)|6u));break;
        default:assert(cmd==4);break;
        }
        if(m->malformed)v[0]^=1u<<16;
        if(m->duplicates){for(unsigned i=0;i<150;i++)enqueue(m,15|0x40|(((m->rxid-1)&7)<<9)|(k<<12),v,k);}
        else enqueue(m,15|0x40|((m->rxid++&7)<<9)|(k<<12),v,k);
        if(m->corrupt)m->rx[m->rxtail-1]^=1;

    }
}
static unsigned fusbread(struct model *m)
{
    if(m->reg==0x43){assert(m->rxhead<m->rxtail);return m->rx[m->rxhead++];}
    if(m->reg==0x41)return 8u|(m->rxhead==m->rxtail?0x20u:0u);
    if(m->reg==0x40)return 0x80;
    return m->regs[m->reg];
}
static void fusbwrite(struct model *m,unsigned v)
{
    if(m->reg!=0x43){m->regs[m->reg]=(uint8_t)v;return;}
    assert(m->txlen<sizeof(m->tx));m->tx[m->txlen++]=(uint8_t)v;
    if(m->txlen>=5&&m->txlen==9u+(m->tx[4]&31u)){peer(m);m->txlen=0;}
}
static int now(void *c,uint64_t *n){struct model *m=c;if(++m->time_reads==m->jump_after)m->now+=m->jump_us;*n=m->now;return 1;}
static int yes(void *c){(void)c;return 1;}
static int enter(void *c,uint64_t d){(void)c;(void)d;return 1;}
static void nothing(void *c){(void)c;}
static int readreg(void *c,uintptr_t a,uint32_t *v)
{
    struct model *m=c;unsigned off=(unsigned)(a-0x07084000u);
    switch(off){
    case 8:*v=m->data;break;case 0xc:*v=m->control;break;case 0x10:*v=m->status;break;
    case 0x14:*v=0x58;break;case 0x20:*v=0x30;break;default:*v=0;break;
    }return 1;
}
static int writereg(void *c,uintptr_t a,uint32_t v)
{
    struct model *m=c;unsigned off=(unsigned)(a-0x07084000u);
    if(off==8){m->pending=v;return 1;}assert(off==0xc);
    m->control=0x48u|(v&4u);
    if(v&0x20){m->status=m->stage?0x10:8;m->stage=1;m->receiving=0;++m->starts;return 1;}
    if(v&0x10){m->control=0x40;m->status=0xf8;m->stage=0;m->receiving=0;++m->stops;return 1;}
    if(m->receiving){m->data=fusbread(m);m->status=(v&4)?0x50:0x58;if(v&4)++m->acks;else ++m->nacks;return 1;}
    if(m->stage==1){m->address=m->pending>>1;assert(m->address==0x22);m->receiving=m->pending&1;m->status=m->receiving?0x40:0x18;m->stage=2;return 1;}
    if(m->stage==2){m->reg=m->pending;m->stage=3;}else fusbwrite(m,m->pending);
    m->status=0x28;return 1;
}
static int outside(void *c,const void *p,unsigned long n){(void)c;(void)p;(void)n;return 1;}
static int prepare(void *c,uint64_t d){(void)c;(void)d;return 1;}
static int vbus(void *c,unsigned on,uint64_t d){(void)d;if(!on)((struct model*)c)->off=1;return 1;}
static int irq(void *c,unsigned *v,uint64_t d){(void)c;(void)d;*v=1;return 1;}
static void setup(struct model *m,struct a733_twi *b,struct a733_typec *s,unsigned polarity)
{
    memset(m,0,sizeof(*m));memset(b,0,sizeof(*b));memset(s,0,sizeof(*s));
    m->now=1;m->control=0x40;m->status=0xf8;
    struct a733_twi_ops bo={now,yes,enter,nothing,nothing,readreg,writereg,yes};
    assert(a733_twi_init_controller(b,&bo,m,0x58,A733_TWI_R1,1)==1);
    struct a733_typec_config c={.ops={outside,now,yes,prepare,yes,vbus,irq},.context=m,.bus=b,
        .owner=1,.generation=2,.qualification=3,.recovery=4,.source_min_mv=4750,.source_max_mv=5250,
        .source_ma=500,.zero_threshold_max_mv=800,.device_id=0x80,.displayport=1};
    assert(a733_typec_init(s,&c)==1);
    /* Attachment/electrical state is modeled here; unchanged native physical
     * CC adapter has separate retained evidence. Execute actual monitor onward. */
    s->state=A733_TYPEC_MONITOR;s->prepared=1;s->polarity=polarity;s->last_service=1;s->startup_deadline=1000000;
}
static void run(struct model *m,struct a733_typec *s,unsigned ready)
{
    for(unsigned i=0;i<200000;i++){
        int r=a733_typec_poll(s);m->now+=5;
        if(r<0|| (ready&&s->dp_published.provider))return;
    }
    assert(!"bounded poll allowance exhausted");
}
static void attention(struct model *m,unsigned status)
{
    uint32_t v[2]={0xff018106u,status};enqueue(m,15|0x40|((m->rxid++&7)<<9)|(2<<12),v,2);
}
int main(int argc,char **argv)
{
    (void)argv;setbuf(stdout,NULL);
    struct model m;struct a733_twi b;struct a733_typec s;
    if(argc==1){
    for(unsigned polarity=1;polarity<=2;polarity++){
        setup(&m,&b,&s,polarity);m.pin=polarity==1?4:16;run(&m,&s,1);
        assert(s.dp_published.provider&&!atomic_load(&s.revoked));
        assert(m.regs[3]==(0xb4u|polarity)&&s.pd.pin_assignment==m.pin&&s.pd.hpd);
        struct a733_typec_dp_token t;assert(a733_typec_dp_snapshot(&s,&t,m.now+100)==1);
        assert(t.polarity==polarity&&a733_typec_dp_validate(&s,&t,m.now+100)==1);
        unsigned previous=m.rxid;attention(&m,0x182);
        while(s.dp_published.hpd_irq!=1){assert(a733_typec_poll(&s)>=0);m.now+=5;}
        assert(m.rxid==previous+1);
        assert(a733_typec_dp_validate(&s,&t,m.now+100)==-1&&!atomic_load(&s.revoked));
        assert(m.acks&&m.nacks&&m.starts>m.stops); /* Repeated START reads. */
        printf("orientation %u: contract, discovery/configure, HPD IRQ and stale-token refusal pass (%u transmissions)\n",polarity,m.txcount);
    }
    setup(&m,&b,&s,1);m.wrong_rdo=1;run(&m,&s,0);assert(atomic_load(&s.revoked)&&!s.pd.ready);
    puts("over-budget request retained refusal pass");
    setup(&m,&b,&s,1);m.nak=1;run(&m,&s,0);assert(atomic_load(&s.revoked)&&!s.pd.ready);
    puts("VDM NAK retained refusal pass");
    setup(&m,&b,&s,1);m.drop=1;run(&m,&s,0);assert(atomic_load(&s.revoked)&&!s.pd.ready&&m.now<100000);
    puts("missing GoodCRC bounded retained refusal pass");
    setup(&m,&b,&s,1);s.pd.deadline=m.now+100;run(&m,&s,0);assert(atomic_load(&s.revoked)&&!s.pd.ready);
    puts("original negotiation deadline retained refusal pass");
    }
    if(argc>1&&strcmp(argv[1],"original-deadline")&&strcmp(argv[1],"refresh")&&strcmp(argv[1],"hpd-aba")&&strcmp(argv[1],"validation-preempt")){
    if(argc==2){
    setup(&m,&b,&s,1);m.corrupt=1;run(&m,&s,0);assert(atomic_load(&s.revoked)&&!s.pd.ready);
    puts("corrupted VDM CRC retained refusal pass");
    setup(&m,&b,&s,1);m.malformed=1;run(&m,&s,0);assert(atomic_load(&s.revoked)&&!s.pd.ready);
    puts("wrong VDM SVID with valid CRC retained refusal pass");
    }
    for(unsigned stage=argc>2?2:0;stage<3;stage++){
        setup(&m,&b,&s,1);
        for(unsigned i=0;i<200000;i++){
            assert(a733_typec_poll(&s)>=0);m.now+=5;
            if((stage==0&&s.pd.tx_wait)||(stage==1&&s.pd.state==1&&!s.pd.tx_wait)||
               (stage==2&&s.pd.state==12&&!s.pd.tx_wait))break;
            assert(i<199999);
        }
        s.pd.wait_until=m.now+100;run(&m,&s,0);
        assert(atomic_load(&s.revoked)&&!s.pd.ready);
        printf("late response %u retained refusal pass\n",stage);
    }
    setup(&m,&b,&s,1);m.duplicates=1;run(&m,&s,0);
    assert(atomic_load(&s.revoked)&&!s.pd.ready&&m.now<150000);
    puts("duplicate VDM traffic cannot extend response deadline pass");
    }

    if(argc>1&&!strcmp(argv[1],"original-deadline")){
        setup(&m,&b,&s,1);s.startup_deadline=m.now+1000;run(&m,&s,0);
        assert(atomic_load(&s.revoked)&&!s.pd.ready&&s.pd.deadline==s.startup_deadline);
        puts("PD cannot extend original CC begin deadline pass");
    }
    if(argc>1&&!strcmp(argv[1],"refresh")&&strcmp(argv[1],"hpd-aba")&&strcmp(argv[1],"validation-preempt")){
        setup(&m,&b,&s,1);run(&m,&s,1);
        struct a733_typec_dp_token t;assert(a733_typec_dp_snapshot(&s,&t,m.now+100)==1);
        uint64_t until=m.now+15000;
        while(m.now<until){assert(a733_typec_poll(&s)>=0);m.now+=5;}
        assert(m.now-t.observed_us>A733_TYPEC_FRESH_US);
        assert(a733_typec_dp_validate(&s,&t,m.now+100)==1);
        m.now+=A733_TYPEC_FRESH_US;
        assert(a733_typec_dp_validate(&s,&t,m.now+100)==-1&&atomic_load(&s.revoked));
        puts("immutable mode token follows current CC freshness; stalled provider revokes pass");
    }
    if(argc>1&&!strcmp(argv[1],"hpd-aba")){
        setup(&m,&b,&s,1);run(&m,&s,1);
        struct a733_typec_dp_token t;assert(a733_typec_dp_snapshot(&s,&t,m.now+100)==1);
        attention(&m,2);
        while(s.dp_published.hpd){assert(a733_typec_poll(&s)>=0);m.now+=5;}
        attention(&m,0x82);
        while(!s.dp_published.hpd){assert(a733_typec_poll(&s)>=0);m.now+=5;}
        assert(s.dp_published.hpd_irq==t.hpd_irq+2);
        assert(a733_typec_dp_validate(&s,&t,m.now+100)==-1&&!atomic_load(&s.revoked));
        puts("HPD low/high without IRQ permanently invalidates old mode snapshot pass");
    }
    if(argc>1&&!strcmp(argv[1],"validation-preempt")){
        setup(&m,&b,&s,1);run(&m,&s,1);
        struct a733_typec_dp_token t;assert(a733_typec_dp_snapshot(&s,&t,m.now+100)==1);
        m.jump_after=m.time_reads+3;m.jump_us=A733_TYPEC_FRESH_US;
        assert(a733_typec_dp_validate(&s,&t,m.now+20000)==-1&&atomic_load(&s.revoked));
        puts("preemption after snapshot cannot bypass current CC freshness pass");
    }
    return 0;
}
