/* Accepted controller fixture setup; only configured-stop cases run here. */
#include "a733_adc.h"
#include "../drivers/power/a733_pmic.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct cell {uintptr_t address;uint32_t value;};
struct fixture {
 struct a733_clock_leases clocks;struct a733_clock_token token;
 struct a733_adc driver;struct cell cells[40];unsigned cells_used,held,retained,reads,writes,acks,data_reads;
 unsigned drop_write,authorized,deny_read,deny_write,reassert,block_retention;
 uint64_t now;uintptr_t denied_address;uint32_t last_ack;
};
static uint32_t *cell(struct fixture *f,uintptr_t a)
{for(unsigned i=0;i<f->cells_used;++i)if(f->cells[i].address==a)return &f->cells[i].value;assert(f->cells_used<40);f->cells[f->cells_used]=(struct cell){a,0};return &f->cells[f->cells_used++].value;}
static int fixture_now(void *v,uint64_t *n){struct fixture *f=v;*n=++f->now;return 1;}
static int task_current(void *v){(void)v;return 1;}
static int authorize_adc(void *v,const struct a733_adc_config *c,const struct a733_clock_token *t)
{struct fixture *f=v;assert(c->owner==23&&c->handoff==91&&t->id==f->token.id);return f->authorized;}
static int register_enter(void *v,uint64_t d){struct fixture *f=v;assert(!f->held&&f->now<d);f->held=1;return 1;}
static void register_leave(void *v){struct fixture *f=v;assert(f->held&&!f->retained);f->held=0;}
static void register_retain(void *v){struct fixture *f=v;assert(f->held);f->held=0;f->retained=1;if(f->block_retention)atomic_store(&f->clocks.busy,1);}
static int read_register(void *v,uintptr_t a,uint32_t *out)
{
 struct fixture *f=v;assert(f->held);++f->reads;
 if(a>=0x02521000&&a<0x02522000)assert((*cell(f,0x02002fc4)&0x10001)==0x10001);
 if(f->deny_read&&a==f->denied_address)return 0;
 if(a>=0x02521080&&a<=0x0252108c){assert(*cell(f,0x02521038)&(1u<<((a-0x02521080)/4)));++f->data_reads;}
 *out=*cell(f,a);return 1;
}
static int write_register(void *v,uintptr_t a,uint32_t value)
{
 struct fixture *f=v;assert(f->held);++f->writes;
 assert(a!=0x025210fc&&a!=0x025210dc&&a!=0x02521018&&a!=0x02521014);
 if(a==0x02521038){assert(value&&!(value&~15u));f->last_ack=value;++f->acks;*cell(f,a)&=~value;
  if(f->reassert){assert(f->data_reads);*cell(f,a)|=1;*cell(f,0x02521080)=77;f->reassert=0;}
 }else if(!f->drop_write) *cell(f,a)=value;
 return !(f->deny_write&&a==f->denied_address);
}
static void initialize(struct fixture *f,unsigned cold)
{
 memset(f,0,sizeof(*f));f->authorized=1;
 assert(a733_clock_leases_init(&f->clocks,fixture_now,f));
 a733_clock_mask_t shared=A733_CLOCK_OSC|A733_CLOCK_REF|A733_CLOCK_FABRIC;
 a733_clock_mask_t exclusive=A733_CLOCK_GPADC;
 if(cold)exclusive|=A733_CLOCK_GPADC24M;else shared|=A733_CLOCK_GPADC24M;
 assert(a733_clock_acquire(&f->clocks,23,shared,exclusive,100000,&f->token)==A733_CLOCK_OK);
 struct a733_adc_config config={23,91,100000,0,24000000,cold};
 struct a733_adc_ops ops={fixture_now,task_current,authorize_adc,register_enter,register_leave,register_retain,read_register,write_register};
 assert(a733_adc_init(&f->driver,&config,&ops,f,&f->clocks,&f->token));
 *cell(f,0x07090160)=0x10000002;*cell(f,0x02002000)=0xf8636300;
 *cell(f,0x02002fc0)=cold?0:0x81000000;*cell(f,0x02521004)=0x00800000;*cell(f,0x0252100c)=0x1f00;
 *cell(f,0x025210fc)=1;
}
static enum a733_adc_result start(struct fixture *f)
{assert(a733_adc_poll(&f->driver)==A733_ADC_PENDING);return a733_adc_poll(&f->driver);}
int main(void)
{
 unsigned cases=0;
 for(unsigned mode=0;mode<6;++mode){struct fixture f;initialize(&f,0);assert(start(&f)==A733_ADC_READY);
  unsigned before=f.reads+f.writes;
  if(mode==1){atomic_store(&f.clocks.busy,1);assert(a733_adc_stop(&f.driver,100000)==A733_ADC_BUSY&&!f.driver.fault&&f.reads+f.writes==before);atomic_store(&f.clocks.busy,0);}
  if(mode==2){f.deny_write=1;f.denied_address=0x02521004;}
  if(mode==3){f.deny_write=1;f.denied_address=0x02521028;f.block_retention=1;}
  if(mode==4)f.now=100000;
  if(mode==5)f.drop_write=1;
  enum a733_adc_result r=a733_adc_stop(&f.driver,100000);
  if(mode<2){assert(r==A733_ADC_STOPPED&&!f.driver.fault&&!f.retained&&!f.held);
   assert(!(*cell(&f,0x02521004)&(1u<<16))&&!(*cell(&f,0x02521008)&0x007f007f)&&!(*cell(&f,0x02521028)&127));
   assert((*cell(&f,0x02002fc4)&0x10001)==0x10001&&*cell(&f,0x02002fc0)==0x81000000);
   unsigned io=f.reads+f.writes;uint64_t now=f.now;struct a733_adc_sample s,saved;memset(&s,0xa5,sizeof(s));saved=s;
   assert(a733_adc_stop(&f.driver,1)==A733_ADC_STOPPED&&a733_adc_poll(&f.driver)==A733_ADC_STOPPED&&a733_adc_read(&f.driver,1,&s)==A733_ADC_STOPPED);
   assert(!memcmp(&s,&saved,sizeof(s))&&io==f.reads+f.writes&&now==f.now&&a733_clock_token_status(&f.clocks,&f.token)==A733_CLOCK_OK);
  }else{assert(f.driver.fault&&r==(mode==3?A733_ADC_BUSY:A733_ADC_RETAINED));unsigned io=f.reads+f.writes;atomic_store(&f.clocks.busy,0);
   assert(a733_adc_stop(&f.driver,100000)==A733_ADC_RETAINED&&io==f.reads+f.writes);}
  ++cases;
 }
 /* Validate both consumers against one atomically published stable parent. */
 struct fixture f;memset(&f,0,sizeof(f));struct a733_clock_token parent,stable,ths,adc,conflict;
 assert(a733_clock_leases_init(&f.clocks,fixture_now,&f));
 assert(a733_clock_acquire(&f.clocks,25,A733_CLOCK_REF|A733_CLOCK_OSC|A733_CLOCK_FABRIC,A733_CLOCK_GPADC24M,100000,&parent)==A733_CLOCK_OK);
 assert(a733_clock_publish_stable(&f.clocks,&parent,A733_CLOCK_GPADC24M,100000,&stable)==A733_CLOCK_OK);
 assert(a733_clock_token_status(&f.clocks,&parent)!=A733_CLOCK_OK);
 assert(!stable.exclusive&&(stable.shared&A733_CLOCK_GPADC24M));
 assert(a733_clock_acquire(&f.clocks,23,A733_CLOCK_GPADC24M|A733_CLOCK_REF|A733_CLOCK_OSC|A733_CLOCK_FABRIC,A733_CLOCK_GPADC,100000,&adc)==A733_CLOCK_OK);
 assert(a733_clock_acquire(&f.clocks,24,A733_CLOCK_GPADC24M|A733_CLOCK_REF|A733_CLOCK_OSC|A733_CLOCK_FABRIC,A733_CLOCK_THS,100000,&ths)==A733_CLOCK_OK);
 assert(a733_clock_acquire(&f.clocks,26,0,A733_CLOCK_GPADC24M,100000,&conflict)==A733_CLOCK_BUSY);
 struct a733_adc driver={0};struct a733_adc_config config={23,91,100000,0,24000000,0};
 struct a733_adc_ops ops={fixture_now,task_current,authorize_adc,register_enter,register_leave,register_retain,read_register,write_register};
 assert(a733_adc_init(&driver,&config,&ops,&f,&f.clocks,&adc));
 printf("PASS %u configured-disable ADC cases and stable-parent coexistence; MMIO modeled\n",cases);
}
