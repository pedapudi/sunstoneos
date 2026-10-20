#include "adc.h"
#include "boot.h"
#include "aarch64_device_memory.h"
#include "a733_storage_digest.h"
#include <string.h>
struct adc_application adc;
int adc_live(void)
{return board_live(NULL)&&!atomic_load_explicit(&adc.failed,memory_order_acquire)&&
 !atomic_load_explicit(&providers.shared.clocks.failed,memory_order_acquire)&&
 !atomic_load_explicit(&providers.shared.locks.retained,memory_order_acquire)&&
 a733_smp_counter()/24<board.session.deadline_us-10000;}
void adc_cancel(void)
{atomic_store_explicit(&adc.failed,1,memory_order_release);atomic_store_explicit(&adc.result,2,memory_order_release);}
int adc_output_ready(void)
{return adc_live()&&atomic_load_explicit(&adc.result,memory_order_acquire)==1;}
static int mappings(void)
{
 const uintptr_t pages[]={0x02002000,0x02003000,adc.session.version==2?0x02524000:0x02521000,0x03400000,0x07090000};
 for(unsigned i=0;i<sizeof(pages)/sizeof(*pages);++i){struct aarch64_translation t;
  if(!aarch64_device_translate(pages[i],1,&t)||t.physical!=pages[i]||t.attributes||t.shareability!=2)return 0;}
 return 1;
}
static int admitted(void *v,const struct a733_adc_config *c,const struct a733_clock_token *t)
{
 return v==&adc&&adc_live()&&c==(adc.session.version==2?&adc.lradc.config:&adc.driver.config)&&
 c->owner==adc.config.owner&&c->handoff==adc.config.handoff&&c->startup_deadline_us==adc.config.startup_deadline_us&&
 c->apb1==adc.config.apb1&&c->apb1_hz==adc.config.apb1_hz&&c->configure_adc_clock==adc.config.configure_adc_clock&&
 t->id==adc.token.id&&t->owner==adc.token.owner&&t->shared==adc.token.shared&&t->exclusive==adc.token.exclusive&&mappings();
}
static int now(void *v,uint64_t *n)
{return v==&adc&&adc_live()&&adc.native.operations.now_us(&adc.native,n);}
static int current(void *v)
{return v==&adc&&adc_live()&&adc.native.operations.current(&adc.native);}
static int authorize(void *v,const struct a733_adc_config *c,const struct a733_clock_token *t)
{return v==&adc&&adc_live()?adc.native.operations.authorize(&adc.native,c,t):-1;}
static int enter(void *v,uint64_t d)
{return v==&adc&&adc_live()?adc.native.operations.enter(&adc.native,d):-1;}
static void leave(void *v)
{if(v==&adc)adc.native.operations.leave(&adc.native);}
static void retain(void *v)
{if(v==&adc)adc.native.operations.retain(&adc.native);}
static int read_register(void *v,uintptr_t a,uint32_t *out)
{return v==&adc&&adc_live()&&adc.native.operations.read(&adc.native,a,out)==1&&adc_live();}
static int write_register(void *v,uintptr_t a,uint32_t x)
{return v==&adc&&adc_live()&&adc.native.operations.write(&adc.native,a,x)==1&&adc_live();}
int adc_construct(void)
{
 if(!board_live(NULL)||board.session.deadline_us<=10000||!adc.handle||!mappings())return 0;
 atomic_init(&adc.failed,0);atomic_init(&adc.result,0);atomic_init(&adc.pending,0);
 adc.irq=(struct gicv3_spi){0x03400000,adc.session.version==2?A733_LRADC_INTID:A733_ADC_INTID};adc.masked=1;
 adc.config=(struct a733_adc_config){.owner=adc.session.owner,.handoff=adc.session.authority,
 .startup_deadline_us=board.session.deadline_us-10000,.apb1=adc.session.apb1,.apb1_hz=adc.session.apb1_hz,
 .configure_adc_clock=adc.session.configure_adc_clock};
 if(!(adc.session.version==2?a733_lradc_native_init:a733_adc_native_init)(&adc.native,&providers.shared.locks,adc.handle,admitted,&adc))return 0;
 adc.operations=(struct a733_adc_ops){now,current,authorize,enter,leave,retain,read_register,write_register};
 storage_digest_bytes(&adc.session,sizeof(adc.session),adc.session_digest);return 1;
}
