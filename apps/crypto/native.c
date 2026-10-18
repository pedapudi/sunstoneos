#include "crypto.h"
#include "boot.h"
#include "a733_ce_native.h"
#include "a733_memory_reservations.h"
#include "aarch64_device_memory.h"
#include "a733_storage_digest.h"
#include <string.h>
struct crypto_application crypto;
static uint64_t deadline(void){return board.session.deadline_us-10000;}
static int now(void *v,uint64_t *n)
{return v==&crypto?a733_counter_clock_now(&providers.shared.counter,n):0;}
static int enter(void *v,uint64_t d)
{return v==&crypto&&board_live(NULL)?a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,d):-1;}
static void leave(void *v)
{if(v==&crypto)a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);}
static void retain(void *v)
{if(v==&crypto)(void)a733_platform_lock_retain(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);}
static int device(uintptr_t a)
{struct aarch64_translation t;return aarch64_device_translate(a,1,&t)&&t.physical==a&&t.attributes==0&&t.shareability==2;}
static int mappings(void)
{
 static const uintptr_t addresses[]={0x02002000,0x02003000,0x07090160,0x04603000,0x04603800,0x03400118};
 for(unsigned i=0;i<sizeof(addresses)/sizeof(addresses[0]);++i)if(!device(addresses[i]))return 0;
 return 1;
}
static int masked(void)
{
 if(!device(0x03400118))return 0;
 __asm__ volatile("dsb sy":::"memory");
 uint32_t enabled=*(volatile const uint32_t *)(uintptr_t)0x03400118;
 __asm__ volatile("dsb sy":::"memory");
 return !(enabled&0x300); /* INTIDs200/201. Ownership of both security views is admitted separately. */
}
static int authorize(void *v,const struct a733_ce_config *c,const struct a733_clock_token *t,const struct a733_ce_memory *m)
{
 if(v!=&crypto||!board_live(NULL)||!mappings()||!masked()||!crypto.engine.initialized||
 m!=crypto.engine.controller.memory||c->owner!=crypto.session.owner||c->handoff!=crypto.session.authority||
 c->profile!=A733_CE_SHA256_AES128||c->deadline_us!=deadline()||c->health_profile||c->configure_health||
 t->id!=crypto.token.id||t->owner!=crypto.token.owner||t->shared!=crypto.token.shared||t->exclusive!=crypto.token.exclusive)return 0;
 for(unsigned i=0;i<4;++i)if(c->health[i])return 0;
 for(unsigned i=0;i<3;++i)if(c->fabric[i]!=board.session.fabric[i]||c->fabric_hz[i]!=board.session.fabric_hz[i])return 0;
 struct device_memory_extent e;enum device_memory_state state;
 if(!device_memory_describe(crypto.engine.binding.pool,crypto.engine.binding.lease,&e,&state)||
 state!=DEVICE_MEMORY_DEVICE||e.cpu!=(uintptr_t)m||e.bytes!=4096||e.physical!=c->descriptor_physical||c->output_physical!=e.physical+256)return 0;
 for(uintptr_t p=e.cpu;p<e.cpu+e.bytes;p+=64){struct aarch64_translation x;
  if(!aarch64_device_translate(p,1,&x)||x.physical!=e.physical+p-e.cpu||x.attributes!=0xff||x.shareability!=3)return 0;}
 /* Immutable initial/memory/clock/supervision records authenticate exclusion,
  * prior drain and secure access; masked/readable registers alone never do. */
 return a733_clock_token_status(&providers.shared.clocks,&crypto.token)==A733_CLOCK_OK;
}
int crypto_construct(void)
{
 if(!mappings()||!masked()||!board_live(NULL)||board.session.deadline_us<=10000)return 0;
 crypto.mutex=xSemaphoreCreateMutexStatic(&crypto.mutex_storage);if(!crypto.mutex)return 0;
 crypto.config=(struct a733_ce_config){.owner=crypto.session.owner,.handoff=crypto.session.authority,
 .deadline_us=deadline(),.profile=A733_CE_SHA256_AES128};
 memcpy(crypto.config.fabric,board.session.fabric,sizeof(crypto.config.fabric));
 memcpy(crypto.config.fabric_hz,board.session.fabric_hz,sizeof(crypto.config.fabric_hz));
 if(a733_clock_acquire(&providers.shared.clocks,crypto.session.owner,
 A733_CLOCK_PERI0|A733_CLOCK_REF|A733_CLOCK_OSC|A733_CLOCK_FABRIC,A733_CLOCK_CE,deadline(),&crypto.token)!=A733_CLOCK_OK)return 0;
 struct a733_ce_ops ops={.now_us=now,.authorize=authorize,.enter=enter,.leave=leave,.retain=retain};
 if(xSemaphoreTake(crypto.mutex,0)!=pdTRUE)return 0;
 int ok=a733_ce_session_native_init(&crypto.engine,&crypto.config,&ops,&crypto,&providers.shared.clocks,&crypto.token)&&
 a733_ce_hash_bind(&crypto.run.hash,&crypto.engine)&&a733_ce_cipher_bind(&crypto.run.cipher,&crypto.engine);
 if(xSemaphoreGive(crypto.mutex)!=pdTRUE)return 0;return ok;
}
static int eligible(void *v)
{return v==&crypto&&authorize(&crypto,&crypto.engine.controller.config,&crypto.token,crypto.engine.controller.memory)==1&&
 board_live(NULL)&&a733_smp_counter()/24<deadline();}
int crypto_output_ready(void)
{
 return board_live(NULL)&&a733_smp_counter()/24<deadline()&&
 !atomic_load_explicit(&crypto.engine.fault,memory_order_acquire)&&
 atomic_load_explicit(&crypto.result,memory_order_acquire)==1;
}
void crypto_task(void *unused)
{
 (void)unused;
 for(;;){
  if(!board_live(NULL)||a733_smp_counter()/24>=deadline()){
   crypto.retaining=1;atomic_store_explicit(&crypto.result,2,memory_order_release);
  }
  if(xSemaphoreTake(crypto.mutex,0)==pdTRUE){
   if(crypto.retaining)(void)a733_ce_session_retain(&crypto.engine);
   else if(!atomic_load_explicit(&crypto.result,memory_order_acquire)){
    int r=crypto_run_step(&crypto.run,deadline());
    if(r<0){crypto.retaining=1;atomic_store_explicit(&crypto.result,2,memory_order_release);(void)a733_ce_session_retain(&crypto.engine);}
    else if(r==1){
     if(crypto_run_publish(&crypto.run,crypto.payload,crypto.digest,&crypto.result,eligible,&crypto)!=1){
      crypto.retaining=1;(void)a733_ce_session_retain(&crypto.engine);
     }
    }
   }
   if(xSemaphoreGive(crypto.mutex)!=pdTRUE){crypto.retaining=1;atomic_store_explicit(&crypto.result,2,memory_order_release);}
  }
  vTaskDelay(1);
 }
}
