#include "monitor.h"
#include "boot.h"
/* The ADC owner configures the parent before constructing either consumer.
 * The exclusive token remains live until atomic stable publication; its
 * replacement is held permanently and is never lent to a core object. */
static int read_word(uintptr_t a,uint32_t *v)
{
 struct a733_adc_native *n=&monitor.owners[0].native;
 return monitor_live()&&n->operations.read(n,a,v)==1&&monitor_live();
}
static int bits(uintptr_t a,uint32_t m,uint32_t v)
{uint32_t actual;return read_word(a,&actual)&&(actual&m)==v;}
static int update(uint32_t mask,uint32_t v)
{
 struct a733_adc_native *n=&monitor.owners[0].native;uint32_t old,actual;
 if(!read_word(0x02002fc0,&old))return 0;
 uint32_t next=(old&~mask)|(v&mask);
 return old==next||(monitor_live()&&n->operations.write(n,0x02002fc0,next)==1&&read_word(0x02002fc0,&actual)&&actual==next);
}
int monitor_parent_step(void)
{
 struct a733_adc_native *n=&monitor.owners[0].native;
 uint64_t deadline=monitor.adc_config.startup_deadline_us;
 if(!monitor.parent.id){
  a733_clock_mask_t shared=A733_CLOCK_OSC|A733_CLOCK_REF|A733_CLOCK_FABRIC;
  if(monitor.session.apb1>>24==3)shared|=A733_CLOCK_PERI0;
  enum a733_clock_result r=a733_clock_acquire(&providers.shared.clocks,monitor.session.clock_owner,shared,A733_CLOCK_GPADC24M,deadline,&monitor.parent);
  return r==A733_CLOCK_BUSY?0:r==A733_CLOCK_OK?0:-1;}
 enum a733_clock_result r;
 if(!monitor.parent_configured){
 int entered=n->operations.enter(n,deadline);if(!entered)return 0;if(entered!=1)return -1;
 r=a733_clock_token_status(&providers.shared.clocks,&monitor.parent);
 if(r==A733_CLOCK_BUSY){n->operations.leave(n);return 0;}
 uint32_t osc,ref,peri;
 int ok=r==A733_CLOCK_OK&&read_word(0x07090160,&osc)&&
 (osc&0x10000002)==0x10000002&&(((osc>>14)&3)==0||((osc>>14)&3)==3)&&
 read_word(0x02002000,&ref)&&(ref&0xf8000000)==0xf8000000&&
 (((ref>>8)&255)+1)==(((ref>>16)&127)+1)&&((ref>>8)&255)+1>=80&&((ref>>8)&255)+1<=116&&
 bits(0x02002518,0x0300001f,monitor.session.apb1);
 if(ok&&monitor.session.apb1>>24==3){
  ok=read_word(0x020020a0,&peri)&&(peri&0xff000002)==0xfe000000&&
   (((peri>>8)&255)+1)>=53&&(((peri>>8)&255)+1)<=105&&
   24u*(((peri>>8)&255)+1)==1200u*(((peri>>20)&7)+1)&&
   bits(0x020020a8,0x80000000,0)&&bits(0x020020ac,0x8e000000,0)&&
   bits(0x02003908,0x0a000a00,0x0a000a00)&&bits(0x02003988,0x0a000000,0x0a000000);}
 if(ok&&monitor.session.configure_adc_clock){
  ok=bits(0x02002fc0,0x80000000,0)&&update(0x0700001f,0x01000000)&&update(0x80000000,0x80000000);}
 ok=ok&&bits(0x02002fc0,0x8700001f,0x81000000);
 if(!ok){n->operations.retain(n);return -1;}
 n->operations.leave(n);monitor.parent_configured=1;
 }
 /* Publication accepts disjoint storage and replaces the exclusive identity
  * atomically; copy the new identity only on OK. No core has seen the old one. */
 struct a733_clock_token replacement;
 r=a733_clock_publish_stable(&providers.shared.clocks,&monitor.parent,A733_CLOCK_GPADC24M,deadline,&replacement);
 if(r==A733_CLOCK_BUSY)return 0;if(r!=A733_CLOCK_OK)return -1;
 monitor.parent=replacement;monitor.latest.parent_word=0x81000000;
 if(!monitor_live())return -1;
 atomic_store_explicit(&monitor.parent_ready,1,memory_order_release);return 1;
}
