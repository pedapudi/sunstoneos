#include "storage_read.h"
#include "boot.h"
#include "aarch64_device_memory.h"
#include "a733_storage_digest.h"
#include <string.h>
struct storage_read_application storage_read;
__attribute__((used,retain)) const uint64_t storage_read_geometry[5]={A733_STORAGE_READ_SESSION_MAGIC,864,256,512,1};
static uint64_t limit(void){return board.session.deadline_us-10000;}
static int memory(uintptr_t a,size_t bytes,uint8_t attributes,uint8_t shareability)
{
 if(!bytes||bytes>UINTPTR_MAX-a)return 0;
 uintptr_t last=a+bytes-1;
 for(uintptr_t p=a;;){
  struct aarch64_translation t;
  if(!aarch64_device_translate(p,1,&t)||t.physical!=p||t.attributes!=attributes||t.shareability!=shareability)return 0;
  if((p>>12)==(last>>12))break;
  p=(p&~(uintptr_t)4095)+4096;
 }
 return 1;
}
static int mappings(void)
{
 static const uintptr_t a[]={0x04020000,0x04020200,0x02002d00,0x02002d0c,0x02002500,0x02002580,0x02002588,
 0x020000f0,0x02000344,0x03400118,0x03400218,0x03400318};
 for(unsigned i=0;i<sizeof(a)/sizeof(a[0]);++i)if(!memory(a[i],1,0,2))return 0;
 return memory((uintptr_t)storage_read.borrowed,512,0xff,3);
}
static int admit(void *v,const struct a733_mmc_platform_config *c,const struct a733_clock_token *t)
{
 return v==&storage_read&&board_live(NULL)&&!atomic_load_explicit(&storage_read.cancel,memory_order_acquire)&&
 !storage_read.platform.update&&!storage_read.native.update&&mappings()&&
 !memcmp(c,&storage_read.config,sizeof(*c))&&t->owner==storage_read.session.owner&&
 a733_clock_token_status(&providers.shared.clocks,t)==A733_CLOCK_OK;
}
static int construct(void)
{
 if(!board_live(NULL)||board.session.deadline_us<=10000||!mappings())return 0;
 uint64_t now=a733_smp_counter()/24;
 if(now>=limit()||storage_read.session.identify_us>limit()-now)return 0;
 storage_read.deadline=now+storage_read.session.identify_us;
 storage_read.config=(struct a733_mmc_platform_config){.owner=storage_read.session.owner,
 .handoff=storage_read.session.authority,.topology=board.session.topology,
 .deadline_us=storage_read.deadline,.lifetime_deadline_us=limit(),.kind=SUNXI_MMC_SD,
 .voltage_mask=storage_read.session.voltage_mask};
 memcpy(storage_read.config.fabric,board.session.fabric,sizeof(storage_read.config.fabric));
 memcpy(storage_read.config.fabric_hz,board.session.fabric_hz,sizeof(storage_read.config.fabric_hz));
 memcpy(storage_read.config.expected,storage_read.session.expected,sizeof(storage_read.session.expected));
 memcpy(storage_read.config.desired,storage_read.session.desired,sizeof(storage_read.session.desired));
 if(!a733_mmc_platform_native_init(&storage_read.native,&providers.shared.locks,storage_read.handle,
 SUNXI_MMC_SD,admit,&storage_read))return 0;
 return a733_mmc_platform_init(&storage_read.platform,&storage_read.config,&storage_read.native.operations,
 &storage_read.native,&providers.shared.clocks,&providers.shared.pins,&providers.shared.rails,&providers.shared.pmic);
}
int storage_read_output_ready(void)
{
 return board_live(NULL)&&a733_smp_counter()/24<limit()&&a733_smp_counter()/24<storage_read.deadline&&
 !atomic_load_explicit(&storage_read.cancel,memory_order_acquire)&&
 atomic_load_explicit(&storage_read.result,memory_order_acquire)==1;
}
void storage_read_revoke(unsigned reason)
{
 (void)reason;
 atomic_store_explicit(&storage_read.cancel,1,memory_order_release);
 atomic_store_explicit(&storage_read.result,2,memory_order_release);
}
static void failure(unsigned reason)
{
 storage_read.reason=reason;
 atomic_store_explicit(&storage_read.result,2,memory_order_release);
 if(storage_read.platform.initialized)(void)a733_mmc_platform_retain(&storage_read.platform);
}
static void step(void)
{
 enum a733_mmc_platform_result r;
 if(!board_live(NULL)||a733_smp_counter()/24>=limit()||
    (storage_read.deadline&&a733_smp_counter()/24>=storage_read.deadline)||atomic_load_explicit(&storage_read.cancel,memory_order_acquire)){
  failure(1);return;
 }
 if(atomic_load_explicit(&storage_read.result,memory_order_acquire)==2){failure(storage_read.reason);return;}
 if(!storage_read.initialized){
  storage_read.initialized=1;if(!construct())failure(2);return;
 }
 switch(storage_read.stage){
 case 0:
  r=a733_mmc_platform_poll(&storage_read.platform);
  if(r==A733_MMC_READY)storage_read.stage=1;
  else if(r!=A733_MMC_PENDING&&r!=A733_MMC_BUSY)failure(3);
  break;
 case 1:
  r=a733_mmc_platform_card(&storage_read.platform,&storage_read.card,storage_read.deadline);
  if(r==A733_MMC_BUSY)break;
  if(r!=A733_MMC_READY){failure(4);break;}
  storage_read.card_valid=1;
  {uint64_t now=a733_smp_counter()/24;
   if(now>=limit()||storage_read.session.read_us>limit()-now){failure(1);break;}
   storage_read.deadline=now+storage_read.session.read_us;}
  storage_read.stage=2;break;
 case 2:
  r=a733_mmc_platform_read(&storage_read.platform,0,1,storage_read.borrowed,512,storage_read.deadline);
  if(r==A733_MMC_PENDING)storage_read.stage=3;
  else if(r!=A733_MMC_BUSY)failure(5);
  break;
 case 3:
  r=a733_mmc_platform_poll(&storage_read.platform);
  if(r==A733_MMC_COMPLETE)storage_read.stage=4;
  else if(r!=A733_MMC_PENDING&&r!=A733_MMC_BUSY)failure(6);
  break;
 case 4:
  r=a733_mmc_platform_take(&storage_read.platform,&storage_read.completion,storage_read.deadline);
  if(r==A733_MMC_BUSY)break;
  if(r!=A733_MMC_READY||storage_read.completion.error!=SUNXI_MMC_OK||storage_read.completion.received!=512){failure(7);break;}
  storage_read.stage=5;break;
 case 5:
  /* A separate scheduling point after take makes revocation precede copying. */
  memcpy(storage_read.copied,storage_read.borrowed,512);
  storage_digest_bytes(storage_read.copied,512,storage_read.digest);
  if(!board_live(NULL)||a733_smp_counter()/24>=limit()||
    (storage_read.deadline&&a733_smp_counter()/24>=storage_read.deadline)||atomic_load_explicit(&storage_read.cancel,memory_order_acquire))failure(1);
  else atomic_store_explicit(&storage_read.result,1,memory_order_release);
  storage_read.stage=6;break;
 default:break;
 }
}
void storage_read_task(void *unused)
{
 (void)unused;
 for(;;){if(xSemaphoreTake(storage_read.mutex,0)==pdTRUE){step();
  if(xSemaphoreGive(storage_read.mutex)!=pdTRUE)storage_read_revoke(1);}
  vTaskDelay(1);}
}
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static void put64(uint8_t *p,uint64_t n){put32(p,(uint32_t)n);put32(p+4,(uint32_t)(n>>32));}
int storage_read_copy(uint8_t *out,unsigned *bytes)
{
 if(xSemaphoreTake(storage_read.mutex,0)!=pdTRUE)return 0;
 unsigned result=atomic_load_explicit(&storage_read.result,memory_order_acquire);
 if(result==1&&!storage_read_output_ready()){storage_read_revoke(1);result=2;}
 memset(out,0,STORAGE_READ_HEADER);
 put32(out,0x44524453);put32(out+4,1);put32(out+8,result);put32(out+12,result==1?512:0);
 put64(out+16,board.session.epoch);put64(out+24,board.session.generation);
 const struct sunxi_mmc_card *c=storage_read.card_valid?&storage_read.card:&storage_read.platform.core.card;
 for(unsigned i=0;i<4;++i){put32(out+64+4*i,c->cid[i]);put32(out+80+4*i,c->csd[i]);}
 put32(out+96,c->ocr);out[100]=(uint8_t)c->rca;out[101]=(uint8_t)(c->rca>>8);
 out[102]=c->sector_addressed;out[103]=c->ext_revision;put64(out+104,c->sectors);
 const struct sunxi_mmc_result *r=result==1?&storage_read.completion:&storage_read.platform.core.result;
 const uint32_t facts[]={r->error,r->interrupts,r->status,r->response,r->stop_response,(uint32_t)r->received,
 (uint32_t)r->submitted,r->confirmed_blocks,r->write_started,storage_read.platform.core.state,
 storage_read.platform.stage,storage_read.platform.core.command,storage_read.platform.core.argument,
 storage_read.platform.core.seen,storage_read.platform.core.setup,storage_read.platform.core.protocol,
 storage_read.platform.core.command_step,storage_read.platform.core.response_seen,storage_read.card_valid};
 for(unsigned i=0;i<sizeof(facts)/sizeof(facts[0]);++i)put32(out+112+4*i,facts[i]);
 for(unsigned i=0;i<4;++i)put32(out+188+4*i,storage_read.platform.core.response[i]);
 put32(out+204,storage_read.reason);memcpy(out+208,a733_smp_build_id,32);
 if(result==1){memcpy(out+32,storage_read.digest,32);memcpy(out+256,storage_read.copied,512);}
 if(result==1&&!storage_read_output_ready()){storage_read_revoke(1);result=2;put32(out+8,2);put32(out+12,0);memset(out+32,0,32);}
 *bytes=result==1?768:256;
 if(xSemaphoreGive(storage_read.mutex)!=pdTRUE){storage_read_revoke(1);return 0;}
 return 1;
}
