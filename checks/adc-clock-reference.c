/* Reuse the accepted controller model; exercise changed clock admission only. */
#define main preserved_adc_main
#include "adc-application-core.c"
#undef main
static void configure(struct fixture *f,unsigned cold,unsigned oscillator,unsigned peri)
{
 initialize(f,cold);
 *cell(f,0x07090160)=oscillator==0?0x10000002:oscillator==1?0x983fb0f7:0x10004002;
 *cell(f,0x02002000)=oscillator==0?0xf8636300:oscillator==1?0xf8675f00:0xf84f6300;
 if(peri){
  /* Actual token producer, not a fabricated expanded token. */
  assert(a733_clock_release(&f->clocks,&f->token,100000)==A733_CLOCK_OK);
  a733_clock_mask_t shared=A733_CLOCK_OSC|A733_CLOCK_REF|A733_CLOCK_FABRIC|A733_CLOCK_PERI0;
  a733_clock_mask_t exclusive=A733_CLOCK_GPADC;
  if(cold)exclusive|=A733_CLOCK_GPADC24M;else shared|=A733_CLOCK_GPADC24M;
  assert(a733_clock_acquire(&f->clocks,23,shared,exclusive,100000,&f->token)==A733_CLOCK_OK);
  memset(&f->driver,0,sizeof(f->driver));
  struct a733_adc_config config={23,91,100000,0x03000005,100000000,cold};
  struct a733_adc_ops ops={fixture_now,task_current,authorize_adc,register_enter,register_leave,register_retain,read_register,write_register};
  assert(a733_adc_init(&f->driver,&config,&ops,f,&f->clocks,&f->token));
  *cell(f,0x02002518)=0x03000005;*cell(f,0x020020a0)=0xff126310;
  *cell(f,0x02003908)=0x0a000a00;*cell(f,0x02003988)=0x0a000000;
 }
}
int main(void)
{
 unsigned cases=0;
 for(unsigned oscillator=0;oscillator<3;++oscillator)for(unsigned cold=0;cold<2;++cold){
  struct fixture f;configure(&f,cold,oscillator,1);
  if(!cold)*cell(&f,0x02002fc0)=0x80000040;else *cell(&f,0x02002fc0)=0x40;
  assert(start(&f)==A733_ADC_READY&&!f.held);
  assert((*cell(&f,0x02002fc0)&0x8700001f)==(cold&&!oscillator?0x81000000u:0x80000000u));
  assert((*cell(&f,0x02002fc0)&0x40)==0x40);
  struct a733_adc_sample s;sample(&f,15);assert(a733_adc_read(&f.driver,100000,&s)==A733_ADC_SAMPLE&&s.valid==15);
  assert(a733_adc_stop(&f.driver,100000)==A733_ADC_STOPPED&&!f.held);++cases;
 }
 for(unsigned oscillator=1;oscillator<3;++oscillator){
  struct fixture f;configure(&f,0,oscillator,0);
  assert(start(&f)==A733_ADC_RETAINED&&!f.writes);++cases;
 }
 for(unsigned loss=0;loss<6;++loss){
  struct fixture f;configure(&f,1,1,1);
  if(loss==0)*cell(&f,0x02002000)^=0x100;
  if(loss==1)*cell(&f,0x02002000)&=~0x10000000u;
  if(loss==2)*cell(&f,0x07090160)&=~2u;
  if(loss==3)*cell(&f,0x07090160)&=~0x10000000u;
  if(loss==4)*cell(&f,0x020020a0)|=2;
  if(loss==5)*cell(&f,0x02002518)^=1;
  assert(start(&f)==A733_ADC_RETAINED&&!f.writes);++cases;
 }
 printf("PASS %u ADC normalized24 clock cases; actual registry/start/read/stop, modeled MMIO\n",cases);
}
