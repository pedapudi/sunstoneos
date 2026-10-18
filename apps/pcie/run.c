#include "pcie.h"
#include "boot.h"
#include "a733_storage_digest.h"
#include <string.h>
static const unsigned offsets[4]={0,4,8,12};
static int fail(void)
{pcie_cancel();pcie.retaining=1;return -1;}
static void retention(void)
{
 if(pcie.port.clock.id&&!pcie.clock_retained){
  if(pcie.port.clock_retained||a733_clock_retain(&providers.shared.clocks,&pcie.port.clock)!=A733_CLOCK_BUSY)pcie.clock_retained=1;
 }
 if(pcie.port.rail.id&&!pcie.rail_retained){
  if(pcie.port.rail_retained||a733_rail_retain(&providers.shared.rails,&pcie.port.rail)!=A733_RAIL_BUSY)pcie.rail_retained=1;
 }
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
 uint8_t *p=pcie.payload;memset(p,0,PCIE_OUTPUT_BYTES);
 put64(p,a733_smp_counter()/24);put32(p+8,4);put32(p+12,0);
 put16(p+16,pcie.endpoint.vendor);put16(p+18,pcie.endpoint.device);put16(p+20,pcie.endpoint.command);put16(p+22,pcie.endpoint.link_status);
 put32(p+24,pcie.endpoint.class_revision);p[28]=pcie.endpoint.header_type;p[29]=pcie.endpoint.pcie_cap;p[30]=pcie.endpoint.msi_cap;
 for(unsigned i=0;i<4;++i){put32(p+32+8*i,offsets[i]);put32(p+36+8*i,pcie.header[i]);}
 storage_digest_bytes(p,PCIE_OUTPUT_BYTES,pcie.digest);
 if(!pcie_live())return fail();
 unsigned expected=0;
 if(!atomic_compare_exchange_strong_explicit(&pcie.result,&expected,1,memory_order_release,memory_order_relaxed))return fail();
 if(!pcie_live())return fail();return 1;
}
int pcie_step(void)
{
 if(!pcie_live()){pcie_cancel();pcie.retaining=1;}
 if(pcie.retaining){retention();return -1;}
 if(atomic_load_explicit(&pcie.result,memory_order_acquire)==1)return 1;
 if(!atomic_load_explicit(&providers.shared.pmic_ready,memory_order_acquire))return 0;
 enum a733_pcie_result r;uint32_t value;int ready;
 switch(pcie.stage){
 case 0:
  ready=outcome(a733_pcie_poll(&pcie.port));if(ready>0)pcie.stage=1;return ready<0?-1:0;
 case 1:
  ready=outcome(a733_pcie_identity(&pcie.port,&pcie.endpoint));if(ready<=0)return ready;
  if(pcie.endpoint.vendor!=pcie.session.vendor||pcie.endpoint.device!=pcie.session.device||
  (pcie.endpoint.command&4)||(pcie.endpoint.header_type&127)||!pcie.endpoint.pcie_cap||
  (pcie.endpoint.link_status&0x3ff)!=0x11)return fail();
  pcie.stage=2;return 0;
 case 2:
  r=a733_pcie_config_read(&pcie.port,0,offsets[pcie.index],pcie.config.lifetime_deadline,&value);
  ready=outcome(r);if(ready<=0)return ready;
  pcie.header[pcie.index++]=value;
  if(pcie.index==4){
   if(pcie.header[0]!=((uint32_t)pcie.endpoint.device<<16|pcie.endpoint.vendor)||
   (uint16_t)pcie.header[1]!=pcie.endpoint.command||(pcie.header[1]&4)||!(pcie.header[1]&(1u<<20))||
   pcie.header[2]!=pcie.endpoint.class_revision||((pcie.header[3]>>16)&255)!=pcie.endpoint.header_type)return fail();
   pcie.stage=3;
  }
  return 0;
 case 3:
  ready=outcome(a733_pcie_config_read(&pcie.port,0,0x34,pcie.config.lifetime_deadline,&value));if(ready<=0)return ready;
  pcie.cap_next=value&255;pcie.stage=4;return 0;
 case 4: {
  unsigned at=pcie.cap_next;
  if(!at){
   if(pcie.pcie_cap!=pcie.endpoint.pcie_cap||pcie.msi_cap!=pcie.endpoint.msi_cap)return fail();
   pcie.stage=5;return 0;
  }
  if(at<0x40||at>0xfc||(at&3)||pcie.cap_count>=48||(pcie.cap_seen&(UINT64_C(1)<<(at/4))))return fail();
  ready=outcome(a733_pcie_config_read(&pcie.port,0,at,pcie.config.lifetime_deadline,&value));if(ready<=0)return ready;
  unsigned id=value&255,control=value>>16;pcie.cap_seen|=UINT64_C(1)<<(at/4);++pcie.cap_count;
  if(id==0x10){
   if(pcie.pcie_cap||at>0xe4||!(control&15)||((control>>4)&15)>1)return fail();pcie.pcie_cap=at;
  }else if(id==5){
   if(pcie.msi_cap||at>0xe8||(control&1))return fail();pcie.msi_cap=at;
  }else if(id==0x11){
   if(pcie.msix_cap||at>0xf4||(control&0x8000))return fail();pcie.msix_cap=at;
  }else if(!id)return fail();
  pcie.cap_next=(value>>8)&255;return 0;
 }
 case 5:
  ready=outcome(a733_pcie_config_read(&pcie.port,0,pcie.pcie_cap+0x10,pcie.config.lifetime_deadline,&value));if(ready<=0)return ready;
  if(((value>>16)&0x3ff)!=0x11)return fail();pcie.endpoint.link_status=(uint16_t)(value>>16);pcie.stage=6;return 0;
 case 6:
  ready=outcome(a733_pcie_poll(&pcie.port));if(ready<=0)return ready;pcie.stage=7;return pcie_publish();
 default:return fail();
 }
}
void pcie_task(void *unused)
{(void)unused;for(;;){(void)pcie_step();vTaskDelay(1);}}
