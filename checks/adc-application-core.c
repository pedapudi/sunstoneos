#include "a733_adc.h"
#include "../drivers/power/a733_pmic.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct cell {uintptr_t address;uint32_t value;};
struct fixture {
 struct a733_clock_leases clocks;struct a733_clock_token token;
 struct a733_adc driver;struct cell cells[40];unsigned cells_used,held,retained,reads,writes,acks,data_reads;
 unsigned authorized,deny_read,deny_write,reassert,block_retention,deny_enter;
 uint64_t now;uintptr_t denied_address;uint32_t last_ack;
};
static uint32_t *cell(struct fixture *f,uintptr_t a)
{for(unsigned i=0;i<f->cells_used;++i)if(f->cells[i].address==a)return &f->cells[i].value;assert(f->cells_used<40);f->cells[f->cells_used]=(struct cell){a,0};return &f->cells[f->cells_used++].value;}
static int fixture_now(void *v,uint64_t *n){struct fixture *f=v;*n=++f->now;return 1;}
static int task_current(void *v){(void)v;return 1;}
static int authorize_adc(void *v,const struct a733_adc_config *c,const struct a733_clock_token *t)
{struct fixture *f=v;assert(c->owner==23&&c->handoff==91&&t->id==f->token.id);return f->authorized;}
static int register_enter(void *v,uint64_t d){struct fixture *f=v;assert(!f->held&&f->now<d);if(f->deny_enter)return -1;f->held=1;return 1;}
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
 }else *cell(f,a)=value;
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
#ifndef ADC_DENIED_ENTRY_ONLY
static void sample(struct fixture *f,uint32_t mask)
{*cell(f,0x02521038)=mask;for(unsigned i=0;i<4;++i)*cell(f,0x02521080+4*i)=i?4095-i:0;}
#endif
#ifdef ADC_DENIED_ENTRY_ONLY
int main(void)
{
 struct fixture f;initialize(&f,0);f.deny_enter=1;
 assert(start(&f)==A733_ADC_RETAINED&&f.driver.fault&&!f.held&&!f.retained&&!f.reads&&!f.writes);
 puts("PASS GPADC denied entry retains token without retaining unowned register mutex");return 0;
}
#else
int main(int argc,char **argv)
{
 assert(argc==2);unsigned cases=0;
 for(unsigned cold=0;cold<2;++cold){struct fixture f;initialize(&f,cold);assert(start(&f)==A733_ADC_READY&&!f.held);
  assert(*cell(&f,0x02521000)==0x5dbf095f&&*cell(&f,0x02521004)==0x00890000&&*cell(&f,0x02521008)==15);
  struct a733_adc_sample first,second;sample(&f,15);assert(a733_adc_read(&f.driver,100000,&first)==A733_ADC_SAMPLE);
  assert(first.valid==15&&first.sequence==1&&!first.code[0]&&first.code[1]==4094&&f.last_ack==15);
  sample(&f,15);assert(a733_adc_read(&f.driver,100000,&second)==A733_ADC_SAMPLE&&second.sequence==2);
  for(unsigned i=0;i<4;++i)assert(first.observed_us[i]&&second.observed_us[i]>first.observed_us[i]);
  memset(&second,0xa5,sizeof(second));struct a733_adc_sample saved=second;
  assert(a733_adc_read(&f.driver,100000,&second)==A733_ADC_AGAIN&&!memcmp(&second,&saved,sizeof(second)));++cases;
 }
 {struct fixture f;initialize(&f,0);f.authorized=0;assert(a733_adc_poll(&f.driver)==A733_ADC_PENDING&&!f.writes);f.authorized=1;assert(start(&f)==A733_ADC_READY);++cases;}
 const struct {uintptr_t a;uint32_t v;} refusals[]={{0x02521004,0x00810000},{0x0252100c,0x41f00},{0x02521010,0x100},{0x025210fc,0},{0x025210dc,1},{0x07090160,0},{0x02002fc0,0x82000000},{0x03400108,64}};
 for(unsigned i=0;i<sizeof(refusals)/sizeof(*refusals);++i){struct fixture f;initialize(&f,0);*cell(&f,refusals[i].a)=refusals[i].v;assert(start(&f)==A733_ADC_RETAINED&&f.driver.fault&&f.retained);unsigned io=f.reads+f.writes;assert(a733_adc_poll(&f.driver)==A733_ADC_RETAINED&&io==f.reads+f.writes);++cases;}
 {struct fixture f;initialize(&f,0);assert(start(&f)==A733_ADC_READY);sample(&f,5);f.reassert=1;struct a733_adc_sample s;
  assert(a733_adc_read(&f.driver,100000,&s)==A733_ADC_SAMPLE&&s.valid==5&&f.last_ack==5&&s.code[0]==0);
  assert(*cell(&f,0x02521038)==1);assert(a733_adc_read(&f.driver,100000,&s)==A733_ADC_SAMPLE&&s.valid==1&&s.code[0]==77&&s.sequence==2);++cases;}
 for(unsigned kind=0;kind<4;++kind){struct fixture f;initialize(&f,0);assert(start(&f)==A733_ADC_READY);sample(&f,15);
  if(kind==0)*cell(&f,0x02521038)=16;if(kind==1)*cell(&f,0x02521084)=4096;
  if(kind==2){f.deny_read=1;f.denied_address=0x02521084;}
  if(kind==3){f.deny_write=1;f.denied_address=0x02521038;}
  struct a733_adc_sample s,saved;memset(&s,0xa5,sizeof(s));saved=s;
  assert(a733_adc_read(&f.driver,100000,&s)==A733_ADC_RETAINED&&f.driver.fault&&!memcmp(&s,&saved,sizeof(s)));++cases;}
 {struct fixture f;initialize(&f,0);assert(start(&f)==A733_ADC_READY);f.now=100000;struct a733_adc_sample s;unsigned reads=f.reads;
  assert(a733_adc_read(&f.driver,100000,&s)==A733_ADC_RETAINED&&f.reads==reads);++cases;}
 {struct fixture f;initialize(&f,0);f.block_retention=1;f.deny_write=1;f.denied_address=0x02521000;
  assert(start(&f)==A733_ADC_BUSY&&f.driver.fault);unsigned io=f.reads+f.writes;atomic_store(&f.clocks.busy,0);
  assert(a733_adc_poll(&f.driver)==A733_ADC_RETAINED&&io==f.reads+f.writes);++cases;}
 struct profile control;assert(profile(A733_RAIL_CLDO3,&control)&&control.enable==0x21&&control.bit==5&&control.selector==0x31&&control.mask==31);
 struct a733_pmic_snapshot s;for(unsigned i=0;i<32;++i){assert(a733_pmic_decode(A733_RAIL_CLDO3,32,(uint8_t)i,&s)&&s.enabled&&s.decoded==(i<=29)&&s.programmed_uv==(i<=29?(500+100*i)*1000:0));}
 FILE *file=fopen(argv[1],"w");assert(file);a733_clock_mask_t exclusive=A733_CLOCK_GPADC;
 assert(fprintf(file,"{\"exclusive_hex\":\"%016llx%016llx\",\"shared_hex\":\"%016llx%016llx\",\"rail_index\":%u,\"core_cases\":%u}\n",(unsigned long long)(exclusive>>64),(unsigned long long)exclusive,(unsigned long long)((A733_CLOCK_GPADC24M|A733_CLOCK_OSC|A733_CLOCK_REF|A733_CLOCK_FABRIC)>>64),(unsigned long long)(A733_CLOCK_GPADC24M|A733_CLOCK_OSC|A733_CLOCK_REF|A733_CLOCK_FABRIC),A733_RAIL_CLDO3,cases)>0&&!fclose(file));
 printf("PASS %u new core cases and additive CLDO3 profile/selectors; MMIO modeled\n",cases);
}

#endif
