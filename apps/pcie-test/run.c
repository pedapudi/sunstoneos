#include "pcie.h"
#include "boot.h"
#include "a733_storage_digest.h"
#include <string.h>
static int irq_arm(void *p){return p==&pcie?pcie_test_irq_arm():-1;}
static int irq_event(void *p,uint32_t *n){return p==&pcie?pcie_test_irq_event(n):-1;}
static void irq_stop(void *p){if(p==&pcie)(void)pcie_test_irq_stop();}
static int fail(void)
{pcie_cancel();pcie.retaining=1;a733_pcie_epf_stop(&pcie.test);return -1;}
static void retention(void)
{
 if(pcie.port.clock.id&&!pcie.clock_retained&&
    (pcie.port.clock_retained||a733_clock_retain(&providers.shared.clocks,&pcie.port.clock)!=A733_CLOCK_BUSY))pcie.clock_retained=1;
 if(pcie.port.rail.id&&!pcie.rail_retained&&
    (pcie.port.rail_retained||a733_rail_retain(&providers.shared.rails,&pcie.port.rail)!=A733_RAIL_BUSY))pcie.rail_retained=1;
}
static int outcome(enum a733_pcie_result r)
{
 if(pcie.port.failed||!pcie_live()||(r!=A733_PCIE_READY&&r!=A733_PCIE_PENDING&&r!=A733_PCIE_BUSY))return fail();
 return r==A733_PCIE_READY;
}
static void put16(uint8_t *p,uint16_t n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static void put64(uint8_t *p,uint64_t n){for(unsigned i=0;i<8;++i)p[i]=(uint8_t)(n>>(8*i));}
int pcie_publish(void)
{
 if(!pcie_live()||!pcie.test.published)return fail();
 const struct a733_pcie_epf_test *t=&pcie.test;
 uint8_t *p=pcie.payload;memset(p,0,PCIE_OUTPUT_BYTES);
 put64(p,a733_smp_counter()/24);put32(p+8,32);put32(p+12,1);
 put16(p+16,pcie.session.vendor);put16(p+18,pcie.session.device);put16(p+20,(uint16_t)((t->command&~0x407u)|0x406));put16(p+22,0x11);
 put32(p+24,pcie.session.bar);put32(p+28,t->event_count);put64(p+32,t->bar_bytes);
 put64(p+40,t->memory[0].physical+4096);put64(p+48,t->memory[1].physical+4096);put64(p+56,t->memory[2].physical);
 put32(p+64,t->status);put32(p+68,4096);put64(p+72,A733_PCIE_TEST_WINDOW);
 memcpy(p+80,pcie.session.endpoint_test,32);memcpy(p+128,pcie.copied,4096);
 storage_digest_bytes(p,PCIE_OUTPUT_BYTES,pcie.digest);
 if(!pcie_live())return fail();
 atomic_store_explicit(&pcie.result,1,memory_order_release);return 1;
}
int pcie_step(void)
{
 if(!pcie_live()){(void)fail();}
 if(pcie.retaining){retention();return -1;}
 if(!atomic_load_explicit(&providers.shared.pmic_ready,memory_order_acquire))return 0;
 int ready;
 if(!pcie.stage){
  ready=outcome(a733_pcie_poll(&pcie.port));if(ready<=0)return ready;
  struct a733_pcie_epf_config c={.identity=pcie.session.board.generation,
   .timeout_us=pcie.session.test_timeout_us,.vendor=pcie.session.vendor,.device=pcie.session.device,.bar=(unsigned)pcie.session.bar};
  struct a733_pcie_epf_irq irq={irq_arm,irq_event,irq_stop,&pcie};
  /* Allocation metadata and mapping checks use the existing image-owned pool;
   * one task owns this concrete client's later ownership transitions. */
  extern struct device_memory_pool *pcie_test_pool(void);
  if(!a733_pcie_epf_init(&pcie.test,&pcie.port,&c,pcie_test_pool(),&irq))return fail();
  pcie.stage=1;return 0;
 }
 ready=outcome(a733_pcie_epf_poll(&pcie.test));if(ready<=0)return ready;
 uint64_t until=a733_smp_counter()/24+10000;
 if(until>pcie.port.config.lifetime_deadline)until=pcie.port.config.lifetime_deadline;
 atomic_store_explicit(&pcie.observed_until,until,memory_order_release);
 if(atomic_load_explicit(&pcie.result,memory_order_acquire)==1)return 1;
 ready=outcome(a733_pcie_epf_copy(&pcie.test,pcie.copied));if(ready<=0)return ready;
 return pcie_publish();
}
void pcie_task(void *unused)
{(void)unused;for(;;){(void)pcie_step();vTaskDelay(1);}}
