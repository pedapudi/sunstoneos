/* Accepted controller fixture setup; only configured-stop cases run here. */
#include "a733_thermal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CCU 0x02002000u
#define THS 0x02522000u
#define SID 0x03006244u
struct model {
    uint32_t ccu[0x2000/4],ths[0x100/4],sid[3],osc;
    uint64_t time;
    unsigned drop_write,held,retained,reads,writes,authorized,fail_write,retention_busy,expire_leave,reenter;
    uintptr_t addresses[64];uint32_t values[64];
    struct a733_thermal driver;
    struct a733_clock_leases registry;
};
static int now(void *v,uint64_t *out) {struct model *m=v;*out=++m->time;return 1;}
static uint32_t *reg(struct model *m,uintptr_t a)
{
    if (!(a&3) && a>=CCU && a<CCU+0x2000) return &m->ccu[(a-CCU)/4];
    if (!(a&3) && a>=THS && a<THS+0x100) return &m->ths[(a-THS)/4];
    if (!(a&3) && a>=SID && a<SID+12) return &m->sid[(a-SID)/4];
    if (a==0x07090160) return &m->osc;
    assert(!"unexpected MMIO");return NULL;
}
static int read32(void *v,uintptr_t a,uint32_t *out)
{
    struct model *m=v;assert(m->held && !m->retained);++m->reads;
    if (m->reenter) {m->reenter=0;assert(a733_thermal_poll(&m->driver)==A733_THERMAL_BUSY);}
    *out=*reg(m,a);return 1;
}
static int write32(void *v,uintptr_t a,uint32_t value)
{
    struct model *m=v;assert(m->held && !m->retained && m->authorized && m->writes<64);
    assert(a==CCU+0xfc0 || a==CCU+0xfe4 || (a>=THS && a<=THS+0xa8));
    m->addresses[m->writes]=a;m->values[m->writes]=value;++m->writes;
    uint32_t *r=reg(m,a);
    if (a>=THS+0x20 && a<=THS+0x2c) {assert(!(value&~31u));*r&=~value;}
    else if(!m->drop_write) *r=value;
    if (m->writes==m->fail_write) {
        if (m->retention_busy) atomic_store(&m->registry.busy,1);
        return 0;
    }
    return 1;
}
static int enter(void *v,uint64_t deadline)
{struct model *m=v;assert(!m->held && deadline>m->time);m->held=1;return 1;}
static void leave(void *v)
{
    struct model *m=v;assert(m->held && !m->retained);m->held=0;
    if (m->expire_leave) {m->time=m->driver.deadline;atomic_store(&m->registry.busy,1);}
}
static void retain(void *v) {struct model *m=v;assert(m->held && !m->retained);m->retained=1;}
static int authorize(void *v,const struct a733_thermal_config *c,const struct a733_clock_token *t)
{
    struct model *m=v;assert(c->owner==23 && c->handoff==91 && t->id);
    assert(a733_clock_token_status(&m->registry,t)==A733_CLOCK_OK);m->authorized=1;return 1;
}
static void pack(uint32_t words[3],unsigned ft,const unsigned code[5])
{
    words[0]=ft;words[1]=0;words[2]=0xabcd1200;
    for (unsigned i=0;i<5;++i) {
        unsigned bit=12+12*i;
        for (unsigned j=0;j<12;++j) if (code[i]&(1u<<j)) words[(bit+j)/32]|=1u<<((bit+j)%32);
    }
}
static void setup(struct model *m,unsigned cold,unsigned peri)
{
    memset(m,0,sizeof(*m));
    m->ccu[0]=0xf8000000u|(99u<<8)|(99u<<16);
    m->ccu[0xa0/4]=0xfe000000u|(99u<<8)|(1u<<20);
    m->ccu[0x1908/4]=0x0fff0fff;m->ccu[0x1988/4]=0x0fff0000;
    m->ccu[0x518/4]=peri ? 0x03000003 : 1;
    m->ccu[0xfc0/4]=cold ? 0x40 : 0x81000040;
    m->ccu[0xfe4/4]=0x20;
    m->ths[0xa0/4]=m->ths[0xa4/4]=0xf000f000;m->ths[0xa8/4]=0xabcdf000;
    m->ths[0x80/4]=m->ths[0x84/4]=0xf000f000;m->ths[0x88/4]=0x1234f000;
    m->ths[8/4]=0xabc;m->ths[0x30/4]=0x80;
    for (unsigned i=0;i<5;++i) m->ths[(0x40+4*i)/4]=0xf000f000;
    const unsigned codes[]={2420,2400,2500,1769,1770};pack(m->sid,250,codes);
    assert(a733_clock_leases_init(&m->registry,now,m));
    a733_clock_mask_t stable=A733_CLOCK_FABRIC|(peri ? A733_CLOCK_PERI0 : 0),exclusive=A733_CLOCK_THS;
    if (cold) exclusive|=A733_CLOCK_GPADC24M;else stable|=A733_CLOCK_GPADC24M;
    struct a733_clock_token token;
    assert(a733_clock_acquire(&m->registry,23,stable,exclusive,100000,&token)==A733_CLOCK_OK);
    struct a733_thermal_config c={.owner=23,.handoff=91,.startup_deadline_us=100000,
        .apb1=m->ccu[0x518/4],.apb1_hz=peri ? 150000000 : 12000000,
        .configure_adc_clock=(uint8_t)cold,.alarm_mask=31,.critical_mask=31};
    for (unsigned i=0;i<5;++i) c.limits[i]=(struct a733_thermal_limits){1400,1600,1000};
    const struct a733_thermal_ops ops={now,authorize,enter,leave,retain,read32,write32};
    assert(a733_thermal_init(&m->driver,&c,&ops,m,&m->registry,&token));
}
static enum a733_thermal_result start(struct model *m)
{
    assert(a733_thermal_poll(&m->driver)==A733_THERMAL_PENDING);
    return a733_thermal_poll(&m->driver);
}
int main(void)
{
 unsigned cases=0;
 for(unsigned mode=0;mode<7;++mode){struct model m;setup(&m,0,0);assert(start(&m)==A733_THERMAL_READY);
  unsigned before=m.reads+m.writes;
  if(mode==1){atomic_store(&m.registry.busy,1);assert(a733_thermal_stop(&m.driver,100000)==A733_THERMAL_BUSY&&!m.driver.fault&&before==m.reads+m.writes);atomic_store(&m.registry.busy,0);}
  if(mode>=2&&mode<=4){m.fail_write=m.writes+mode-1;if(mode==4)m.retention_busy=1;}
  if(mode==5)m.time=100000;
  if(mode==6)m.drop_write=1;
  enum a733_thermal_result r=a733_thermal_stop(&m.driver,100000);
  if(mode<2){assert(r==A733_THERMAL_STOPPED&&!m.driver.fault&&!m.held&&!m.retained);
   assert(!(m.ths[4/4]&31)&&!(m.ths[0x14/4]&31)&&!(m.ths[0x18/4]&31));
   assert(m.ccu[0xfc0/4]==0x81000040&&(m.ccu[0xfe4/4]&0x10001)==0x10001);
   unsigned io=m.reads+m.writes;uint64_t time=m.time;struct a733_thermal_sample s,saved;memset(&s,0xa5,sizeof(s));saved=s;
   assert(a733_thermal_stop(&m.driver,1)==A733_THERMAL_STOPPED&&a733_thermal_poll(&m.driver)==A733_THERMAL_STOPPED&&a733_thermal_read(&m.driver,1,&s)==A733_THERMAL_STOPPED);
   assert(!memcmp(&s,&saved,sizeof(s))&&io==m.reads+m.writes&&time==m.time);
  }else{assert(m.driver.fault&&r==(mode==4?A733_THERMAL_BUSY:A733_THERMAL_RETAINED));unsigned io=m.reads+m.writes;atomic_store(&m.registry.busy,0);
   assert(a733_thermal_stop(&m.driver,100000)==A733_THERMAL_RETAINED&&io==m.reads+m.writes);}
  ++cases;
 }
 printf("PASS %u configured-disable thermal cases; MMIO modeled\n",cases);
}
