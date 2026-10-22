#include "pcie.h"
#include "boot.h"
#include "a733_storage_digest.h"
#include <string.h>
#define RECORD_BYTES (PCIE_OUTPUT_BYTES-A733_PCIE_NVME_BYTES)
_Static_assert(RECORD_BYTES==192,"NVMe identity record");
/* Leases and the NVMe queue memory stay held after any failure. The
 * controller is never reset here, so its queue and data pages are marked
 * retained even when the driver itself has not failed. */
static int fail(void)
{
 pcie_cancel();pcie.retaining=1;
 if(pcie.nvme.self==&pcie.nvme)(void)device_memory_retain(pcie_nvme_pool(),pcie.nvme.lease);
 return -1;
}
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
 if(!pcie_live())return fail();
 const struct a733_pcie_nvme_identity *id=&pcie.identity;
 const struct a733_pcie_function *f=&pcie.nvme.function;
 uint8_t *p=pcie.payload;memset(p,0,PCIE_OUTPUT_BYTES);
 /* Flags: bit 0 the read was copied, bit 1 the controller shut down and the
  * port stopped with PERST held. */
 put64(p,a733_smp_counter()/24);put32(p+8,A733_PCIE_NVME_BYTES);put32(p+12,3);
 put16(p+16,id->vendor);put16(p+18,f->ids[pcie.session.function].device);put16(p+20,id->subsystem_vendor);put16(p+22,pcie.port.endpoint.link_status);
 put32(p+24,id->version);put32(p+28,id->namespaces);put64(p+32,id->blocks);put32(p+40,id->lba_shift);put32(p+44,id->mdts);
 put64(p+48,pcie.session.lba);put32(p+56,f->bars[0].base);put32(p+60,f->bars[0].bytes);
 memcpy(p+64,id->serial,20);memcpy(p+84,id->firmware,8);put32(p+92,pcie.session.namespace_id);memcpy(p+96,id->model,40);
 put32(p+136,f->ids[pcie.session.function].class_revision);put32(p+140,pcie.session.function);memcpy(p+144,pcie.session.nvme,32);
 memcpy(p+RECORD_BYTES,pcie.data,A733_PCIE_NVME_BYTES);
 storage_digest_bytes(p,PCIE_OUTPUT_BYTES,pcie.digest);
 if(!pcie_live())return fail();
 unsigned expected=0;
 if(!atomic_compare_exchange_strong_explicit(&pcie.result,&expected,1,memory_order_release,memory_order_relaxed))return fail();
 return pcie_live()?1:fail();
}
int pcie_step(void)
{
 if(!pcie_live()){pcie_cancel();pcie.retaining=1;}
 if(pcie.retaining){retention();return -1;}
 if(atomic_load_explicit(&pcie.result,memory_order_acquire)==1)return 1;
 if(!atomic_load_explicit(&providers.shared.pmic_ready,memory_order_acquire))return 0;
 enum a733_pcie_result r;int ready;
 switch(pcie.stage){
 case 0: {
  ready=outcome(a733_pcie_poll(&pcie.port));if(ready<=0)return ready;
  /* Polled completion: INTx stays disabled in the command register and
   * MSI-X stays off, so no GIC route or handler is linked. */
  const struct a733_pcie_nvme_config c={.identity=pcie.session.board.generation,
   .timeout_us=pcie.session.command_timeout_us,.vendor=pcie.session.vendor,.device=pcie.session.device,
   .function=pcie.session.function,.irq=A733_PCIE_IRQ_POLL,.namespace_id=pcie.session.namespace_id};
  if(!a733_pcie_nvme_init(&pcie.nvme,&pcie.port,&c,pcie_nvme_pool(),NULL))return fail();
  pcie.stage=1;return 0;
 }
 case 1:
  ready=outcome(a733_pcie_nvme_poll(&pcie.nvme));if(ready<0)return ready;
  if(!pcie.window_checked&&pcie.nvme.function.window_bytes){
   if(!pcie_window_mapped(pcie.nvme.function.window_bytes))return fail();
   pcie.window_checked=1;
  }
  if(!ready)return 0;
  if(!pcie.window_checked||outcome(a733_pcie_nvme_identity(&pcie.nvme,&pcie.identity))!=1||
     pcie.identity.vendor!=pcie.session.vendor||!a733_pcie_nvme_read(&pcie.nvme,pcie.session.lba))return fail();
  pcie.stage=2;return 0;
 case 2:
  ready=outcome(a733_pcie_nvme_poll(&pcie.nvme));if(ready<=0)return ready;
  if(outcome(a733_pcie_nvme_copy(&pcie.nvme,pcie.data))!=1||!a733_pcie_nvme_shutdown(&pcie.nvme))return fail();
  pcie.stage=3;return 0;
 case 3:
  r=a733_pcie_nvme_poll(&pcie.nvme);
  if(r!=A733_PCIE_STOPPED)return outcome(r)<0?-1:0;
  pcie.stage=4;return pcie_publish();
 default:return fail();
 }
}
void pcie_task(void *unused)
{(void)unused;for(;;){(void)pcie_step();vTaskDelay(1);}}
