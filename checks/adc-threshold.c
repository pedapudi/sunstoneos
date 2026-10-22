/* GPADC compare-window threshold mode with the actual driver and clock registry.
 * MMIO, time and task identity are modeled; no analog comparison is claimed. */
#include "a733_adc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define ADC 0x02521000u
struct cell {uintptr_t address;uint32_t value;};
struct fixture {
 struct a733_clock_leases clocks;struct a733_clock_token token;struct a733_adc driver;
 struct cell cells[64];unsigned cells_used,held,retained,reads,writes,owner,deny_write;
 uint64_t now;uintptr_t denied_address;
};
static uint32_t *cell(struct fixture *f,uintptr_t a)
{for(unsigned i=0;i<f->cells_used;++i)if(f->cells[i].address==a)return &f->cells[i].value;assert(f->cells_used<64);f->cells[f->cells_used]=(struct cell){a,0};return &f->cells[f->cells_used++].value;}
static int fixture_now(void *v,uint64_t *n){struct fixture *f=v;*n=++f->now;return 1;}
static int task_current(void *v){return ((struct fixture *)v)->owner;}
static int authorize(void *v,const struct a733_adc_config *c,const struct a733_clock_token *t){(void)v;(void)c;(void)t;return 1;}
static int enter(void *v,uint64_t d){struct fixture *f=v;assert(!f->held&&f->now<d);f->held=1;return 1;}
static void leave(void *v){struct fixture *f=v;assert(f->held&&!f->retained);f->held=0;}
static void retain(void *v){struct fixture *f=v;assert(f->held);f->held=0;f->retained=1;}
static int read_register(void *v,uintptr_t a,uint32_t *out){struct fixture *f=v;assert(f->held);++f->reads;*out=*cell(f,a);return 1;}
static int write_register(void *v,uintptr_t a,uint32_t value)
{
 struct fixture *f=v;assert(f->held);++f->writes;
 assert(a!=ADC+0xfc&&a!=ADC+0xdc&&a!=ADC+0x18&&a!=ADC+0x14&&a!=ADC+0x0c&&a!=ADC+0x10);
 if(a==ADC+0x30||a==ADC+0x34||a==ADC+0x38){assert(value&&!(value&~15u));*cell(f,a)&=~value;}
 else if(!(f->deny_write&&a==f->denied_address))*cell(f,a)=value;
 return !(f->deny_write&&a==f->denied_address);
}
static const struct a733_adc_ops ops={fixture_now,task_current,authorize,enter,leave,retain,read_register,write_register};
static struct a733_adc_thresholds windows(unsigned mask)
{
 struct a733_adc_thresholds t={.mask=mask};
 if(mask&1){t.low[0]=100;t.high[0]=3000;}
 if(mask&4){t.low[2]=10;t.high[2]=20;}
 return t;
}
static int setup(struct fixture *f,const struct a733_adc_thresholds *t)
{
 static const struct a733_adc_config c={.owner=23,.handoff=91,.startup_deadline_us=100000,.apb1_hz=24000000};
 memset(f,0,sizeof(*f));f->owner=1;
 assert(a733_clock_leases_init(&f->clocks,fixture_now,f));
 assert(a733_clock_acquire(&f->clocks,23,A733_CLOCK_OSC|A733_CLOCK_REF|A733_CLOCK_FABRIC|A733_CLOCK_GPADC24M,
  A733_CLOCK_GPADC,100000,&f->token)==A733_CLOCK_OK);
 *cell(f,0x07090160)=0x10000002;*cell(f,0x02002000)=0xf8636300;*cell(f,0x02002fc0)=0x81000000;
 *cell(f,ADC+4)=0x00800000;*cell(f,ADC+0x0c)=0x1f00;*cell(f,ADC+0xfc)=1;
 for(unsigned i=0;i<A733_ADC_CHANNELS;++i)*cell(f,ADC+0x40+4*i)=0x0bff0400;
 assert(a733_adc_init(&f->driver,&c,&ops,f,&f->clocks,&f->token));
 return !t->mask||a733_adc_configure_thresholds(&f->driver,t);
}
static enum a733_adc_result start(struct fixture *f)
{assert(a733_adc_poll(&f->driver)==A733_ADC_PENDING);return a733_adc_poll(&f->driver);}
static void conversion(struct fixture *f,uint32_t data,uint32_t low,uint32_t high)
{*cell(f,ADC+0x38)=data;*cell(f,ADC+0x30)=low;*cell(f,ADC+0x34)=high;for(unsigned i=0;i<4;++i)*cell(f,ADC+0x80+4*i)=50+i;}
int main(void)
{
 unsigned cases=0;struct fixture f;struct a733_adc_sample s;uint32_t low,high;
 /* Nominal: windows on channels 0 and 2 are programmed before ADC_EN. */
 {struct a733_adc_thresholds c=windows(5);assert(setup(&f,&c)&&start(&f)==A733_ADC_READY&&!f.held);
  assert(*cell(&f,ADC+0x40)==(3000u<<16|100)&&*cell(&f,ADC+0x48)==(20u<<16|10));
  assert(*cell(&f,ADC+0x44)==0x0bff0400&&*cell(&f,ADC+0x4c)==0x0bff0400);
  assert(*cell(&f,ADC+8)==(15u|5u<<16)&&*cell(&f,ADC+0x20)==5&&*cell(&f,ADC+0x24)==5&&*cell(&f,ADC+0x28)==15);
  assert(*cell(&f,ADC+4)==0x00890000);
  assert(a733_adc_thresholds(&f.driver,&low,&high)==A733_ADC_AGAIN&&!low&&!high);
  conversion(&f,15,1,4);assert(a733_adc_read(&f.driver,100000,&s)==A733_ADC_SAMPLE&&s.valid==15&&s.code[2]==52);
  assert(!*cell(&f,ADC+0x30)&&!*cell(&f,ADC+0x34)&&!*cell(&f,ADC+0x38));
  unsigned io=f.reads+f.writes;
  assert(a733_adc_thresholds(&f.driver,&low,&high)==A733_ADC_SAMPLE&&low==1&&high==4&&io==f.reads+f.writes);
  assert(a733_adc_thresholds(&f.driver,&low,&high)==A733_ADC_AGAIN&&!low&&!high);
  /* A compare status without fresh data is still acknowledged and latched. */
  conversion(&f,0,0,1);assert(a733_adc_read(&f.driver,100000,&s)==A733_ADC_AGAIN&&!*cell(&f,ADC+0x34));
  conversion(&f,1,4,0);assert(a733_adc_read(&f.driver,100000,&s)==A733_ADC_SAMPLE&&s.sequence==2);
  assert(a733_adc_thresholds(&f.driver,&low,&high)==A733_ADC_SAMPLE&&low==4&&high==1);
  /* Stop disables compare interrupts and enables; latched events stay readable. */
  conversion(&f,1,1,0);assert(a733_adc_read(&f.driver,100000,&s)==A733_ADC_SAMPLE);
  assert(a733_adc_stop(&f.driver,100000)==A733_ADC_STOPPED);
  assert(!*cell(&f,ADC+0x20)&&!*cell(&f,ADC+0x24)&&!*cell(&f,ADC+0x28)&&!*cell(&f,ADC+8)&&!(*cell(&f,ADC+4)&(1u<<16)));
  io=f.reads+f.writes;
  assert(a733_adc_thresholds(&f.driver,&low,&high)==A733_ADC_SAMPLE&&low==1&&!high&&io==f.reads+f.writes);++cases;}
 /* Window refusals: channel outside0..3, empty or inverted window, code
  * above4095, a stray window on an unselected channel, a second call, an
  * empty mask, a call after the first poll and a non-owner caller. */
 {struct a733_adc_thresholds c=windows(16);assert(!setup(&f,&c));
  c=windows(1);c.low[0]=3000;assert(!setup(&f,&c));
  c=windows(1);c.low[0]=c.high[0]=7;assert(!setup(&f,&c));
  c=windows(1);c.high[0]=4096;assert(!setup(&f,&c));
  c=windows(1);c.high[3]=9;assert(!setup(&f,&c));
  c=windows(1);assert(setup(&f,&c)&&!a733_adc_configure_thresholds(&f.driver,&c));
  struct a733_adc_thresholds empty=windows(0);
  assert(!a733_adc_configure_thresholds(&f.driver,&empty));
  assert(setup(&f,&empty)&&a733_adc_poll(&f.driver)==A733_ADC_PENDING&&!a733_adc_configure_thresholds(&f.driver,&c));
  assert(setup(&f,&empty));f.owner=0;assert(!a733_adc_configure_thresholds(&f.driver,&c)&&!f.driver.fault);++cases;}
 /* The passive profile has no threshold accessor and refuses compare state. */
 {struct a733_adc_thresholds c=windows(0);assert(setup(&f,&c)&&start(&f)==A733_ADC_READY&&*cell(&f,ADC+8)==15);
  assert(a733_adc_thresholds(&f.driver,&low,&high)==A733_ADC_INVALID);
  conversion(&f,1,1,0);assert(a733_adc_read(&f.driver,100000,&s)==A733_ADC_RETAINED&&f.retained);++cases;}
 /* A status bit for an unselected channel faults before any acknowledgement. */
 {struct a733_adc_thresholds c=windows(5);assert(setup(&f,&c)&&start(&f)==A733_ADC_READY);
  conversion(&f,15,2,0);assert(a733_adc_read(&f.driver,100000,&s)==A733_ADC_RETAINED&&f.retained&&*cell(&f,ADC+0x38)==15);
  assert(a733_adc_thresholds(&f.driver,&low,&high)==A733_ADC_RETAINED);++cases;}
 /* A changed compare window or interrupt enable is detected on the next read. */
 for(unsigned kind=0;kind<2;++kind){struct a733_adc_thresholds c=windows(5);assert(setup(&f,&c)&&start(&f)==A733_ADC_READY);
  if(kind)*cell(&f,ADC+0x24)=1;else *cell(&f,ADC+0x48)=(21u<<16|10);
  conversion(&f,1,0,0);assert(a733_adc_read(&f.driver,100000,&s)==A733_ADC_RETAINED&&f.retained);++cases;}
 /* An inherited compare enable or a failed window write retains at startup. */
 {struct a733_adc_thresholds c=windows(5);assert(setup(&f,&c));*cell(&f,ADC+0x20)=1;
  assert(start(&f)==A733_ADC_RETAINED&&f.retained&&*cell(&f,ADC+0x40)==0x0bff0400);++cases;}
 {struct a733_adc_thresholds c=windows(5);assert(setup(&f,&c));f.deny_write=1;f.denied_address=ADC+0x48;
  assert(start(&f)==A733_ADC_RETAINED&&f.retained&&!(*cell(&f,ADC+4)&(1u<<16)));++cases;}
 /* A non-owner threshold query faults and retains. */
 {struct a733_adc_thresholds c=windows(1);assert(setup(&f,&c)&&start(&f)==A733_ADC_READY);f.owner=0;
  assert(a733_adc_thresholds(&f.driver,&low,&high)==A733_ADC_RETAINED&&f.driver.fault);++cases;}
 printf("PASS %u GPADC threshold cases: programmed windows, latched low/high events, stop, refusals; MMIO modeled\n",cases);
 return 0;
}
