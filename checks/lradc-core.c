#include "a733_lradc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct cell {uintptr_t address;uint32_t value;};
struct fixture {
 struct a733_clock_leases clocks;struct a733_clock_token token;struct a733_lradc driver;
 struct cell cells[32];unsigned used,held,retained,reads,writes,deny_enter,ignore,reassert;
 uint64_t now;
};
static uint32_t *cell(struct fixture *f,uintptr_t a)
{for(unsigned i=0;i<f->used;++i)if(f->cells[i].address==a)return &f->cells[i].value;assert(f->used<32);f->cells[f->used]=(struct cell){a,0};return &f->cells[f->used++].value;}
static int now(void *v,uint64_t *n){struct fixture *f=v;*n=++f->now;return 1;}
static int current(void *v){(void)v;return 1;}
static int authorize(void *v,const struct a733_adc_config *c,const struct a733_clock_token *t)
{struct fixture *f=v;assert(c->owner==23&&c->handoff==91&&t->id==f->token.id);return 1;}
static int enter(void *v,uint64_t d){struct fixture *f=v;assert(!f->held&&f->now<d);if(f->deny_enter)return -1;f->held=1;return 1;}
static void leave(void *v){struct fixture *f=v;assert(f->held&&!f->retained);f->held=0;}
static void retain(void *v){struct fixture *f=v;assert(f->held);f->retained=1;}
static int read_reg(void *v,uintptr_t a,uint32_t *out)
{struct fixture *f=v;assert(f->held);++f->reads;if(a>=0x02524000&&a<=0x0252400c)assert((*cell(f,0x02003024)&0x10001)==0x10001);*out=*cell(f,a);return 1;}
static int write_reg(void *v,uintptr_t a,uint32_t x)
{
 struct fixture *f=v;assert(f->held);++f->writes;
 assert(a==0x02003024||a==0x02524000||a==0x02524004||a==0x02524008);
 if(f->ignore)return 1;
 /* reassert 1 models a stuck flag; 2 models one conversion landing after W1C. */
 if(a==0x02524008){assert(x==1);*cell(f,a)=f->reassert?1:0;if(f->reassert==2)f->reassert=0;}
 else *cell(f,a)=x;
 return 1;
}
static void setup(struct fixture *f)
{
 memset(f,0,sizeof(*f));assert(a733_clock_leases_init(&f->clocks,now,f));
 assert(a733_clock_acquire(&f->clocks,23,A733_CLOCK_OSC|A733_CLOCK_REF|A733_CLOCK_FABRIC|A733_CLOCK_RTC32K|A733_CLOCK_RC16M,A733_CLOCK_LRADC,100000,&f->token)==A733_CLOCK_OK);
 struct a733_adc_config c={23,91,100000,0,24000000,0};
 struct a733_adc_ops o={now,current,authorize,enter,leave,retain,read_reg,write_reg};
 assert(a733_lradc_init(&f->driver,&c,&o,f,&f->clocks,&f->token));
 *cell(f,0x07090160)=0x10000003;*cell(f,0x02002000)=0xf8636300;
 *cell(f,0x07090000)=0x11;*cell(f,0x07090004)=1;
 *cell(f,0x02003024)=0xa5000000;*cell(f,0x02524000)=0x01000168;*cell(f,0x0252400c)=63;
}
static enum a733_adc_result start(struct fixture *f)
{assert(a733_lradc_poll(&f->driver)==A733_ADC_PENDING);return a733_lradc_poll(&f->driver);}
int main(void)
{
 unsigned cases=0;struct fixture f;setup(&f);assert(start(&f)==A733_ADC_READY);
 assert(*cell(&f,0x02003024)==0xa5010001&&*cell(&f,0x02524004)==1);
 struct a733_adc_sample s,first;memset(&s,0xa5,sizeof(s));first=s;
 assert(a733_lradc_read(&f.driver,100000,&s)==A733_ADC_AGAIN&&!memcmp(&s,&first,sizeof(s)));
 for(unsigned i=0;i<2;++i){*cell(&f,0x02524008)=1;*cell(&f,0x0252400c)=i?63:0;
 assert(a733_lradc_read(&f.driver,100000,&s)==A733_ADC_SAMPLE&&s.valid==1&&s.sequence==i+1&&s.code[0]==(i?63:0));
 if(!i)first=s;else assert(s.observed_us[0]>first.observed_us[0]);}
 assert(a733_lradc_stop(&f.driver,100000)==A733_ADC_STOPPED&&!*cell(&f,0x02524004)&&!(*cell(&f,0x02524000)&1));
 unsigned io=f.reads+f.writes;assert(a733_lradc_stop(&f.driver,100000)==A733_ADC_STOPPED&&a733_lradc_poll(&f.driver)==A733_ADC_STOPPED&&io==f.reads+f.writes);++cases;
 const struct cell refuse[]={{0x02003024,1},{0x07090000,3},{0x03400108,32},{0x03400208,32},{0x03400308,32},{0x02524000,1},{0x02524004,2},{0x02524008,2}};
 for(unsigned i=0;i<sizeof(refuse)/sizeof(*refuse);++i){setup(&f);*cell(&f,refuse[i].address)=refuse[i].value;
 assert(start(&f)==A733_ADC_RETAINED&&f.driver.fault&&f.retained);io=f.reads+f.writes;
 assert(a733_lradc_poll(&f.driver)==A733_ADC_RETAINED&&io==f.reads+f.writes);++cases;}
 setup(&f);f.deny_enter=1;assert(start(&f)==A733_ADC_RETAINED&&!f.held&&!f.retained&&!f.writes);++cases;
 setup(&f);f.ignore=1;assert(start(&f)==A733_ADC_RETAINED&&f.retained);++cases;
 setup(&f);assert(start(&f)==A733_ADC_READY);*cell(&f,0x02524008)=1;f.reassert=1;memset(&s,0xa5,sizeof(s));first=s;
 assert(a733_lradc_read(&f.driver,100000,&s)==A733_ADC_RETAINED&&!memcmp(&s,&first,sizeof(s)));++cases;
 setup(&f);assert(start(&f)==A733_ADC_READY);*cell(&f,0x02524008)=1;*cell(&f,0x0252400c)=5;f.reassert=2;io=f.writes;
 assert(a733_lradc_read(&f.driver,100000,&s)==A733_ADC_SAMPLE&&s.sequence==1&&s.code[0]==5&&f.writes==io+2&&!f.driver.fault);
 assert(a733_lradc_read(&f.driver,100000,&s)==A733_ADC_AGAIN);*cell(&f,0x02524008)=1;*cell(&f,0x0252400c)=6;
 assert(a733_lradc_read(&f.driver,100000,&s)==A733_ADC_SAMPLE&&s.sequence==2&&s.code[0]==6);++cases;
 setup(&f);assert(a733_lradc_poll(&f.driver)==A733_ADC_PENDING);io=f.reads+f.writes;memset(&s,0xa5,sizeof(s));first=s;
 assert(a733_lradc_read(&f.driver,100000,&s)==A733_ADC_INVALID&&!f.driver.fault&&!f.retained&&io==f.reads+f.writes&&!memcmp(&s,&first,sizeof(s)));
 assert(a733_clock_token_status(&f.clocks,&f.token)==A733_CLOCK_OK&&a733_lradc_poll(&f.driver)==A733_ADC_READY);
 assert(a733_lradc_read(&f.driver,0,&s)==A733_ADC_INVALID&&!f.driver.fault);*cell(&f,0x02524008)=1;
 assert(a733_lradc_read(&f.driver,100000,&s)==A733_ADC_SAMPLE&&s.sequence==1);++cases;
 setup(&f);assert(start(&f)==A733_ADC_READY);*cell(&f,0x02524008)=1;*cell(&f,0x0252400c)=64;
 assert(a733_lradc_read(&f.driver,100000,&s)==A733_ADC_RETAINED);++cases;
 setup(&f);assert(start(&f)==A733_ADC_READY);f.now=100000;io=f.reads;
 assert(a733_lradc_read(&f.driver,100000,&s)==A733_ADC_RETAINED&&io==f.reads);++cases;
 setup(&f);*cell(&f,0x07090000)=0;*cell(&f,0x07090004)=0;*cell(&f,0x07090008)=15;
 assert(start(&f)==A733_ADC_READY);*cell(&f,0x07090008)=14;
 assert(a733_lradc_read(&f.driver,100000,&s)==A733_ADC_RETAINED);++cases;
 printf("PASS %u focused LRADC driver cases; registers/analog completion modeled\n",cases);
}
