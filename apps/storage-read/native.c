#include "storage_read.h"
#include "boot.h"
#include "aarch64_device_memory.h"
#include "a733_storage_digest.h"
#include <string.h>
struct storage_read_application storage_read;
__attribute__((used,retain)) const uint64_t storage_read_geometry[5]={A733_STORAGE_READ_SESSION_MAGIC,STORAGE_READ_SESSION_BYTES,288,4096,2};
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
#ifdef SUNSTONE_EMMC_READ
 uintptr_t controller=0x04022000+storage_read.session.emmc_host*0x1000;
 if(!memory(controller,0x304,0,2)||!memory(0x02000040,0x34,0,2)||
    !memory(0x07090160,4,0,2))return 0;
#else
 if(!memory(0x04020000,0x304,0,2))return 0;
#endif
 static const uintptr_t a[]={0x02002d00,0x02002d0c,0x02002500,0x02002580,0x02002588,
 0x020000f0,0x02000344,0x03400118,0x03400218,0x03400318};
 for(unsigned i=0;i<sizeof(a)/sizeof(a[0]);++i)if(!memory(a[i],1,0,2))return 0;
 return memory((uintptr_t)storage_read.borrowed,sizeof(storage_read.borrowed),0xff,3);
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
 .deadline_us=storage_read.deadline,.lifetime_deadline_us=limit(),
#ifdef SUNSTONE_EMMC_READ
 .kind=SUNXI_MMC_EMMC,.emmc_host=storage_read.session.emmc_host,
#else
 .kind=SUNXI_MMC_SD,.sd_profile=storage_read.session.sd_profile,
#endif
 .voltage_mask=storage_read.session.voltage_mask};
 memcpy(storage_read.config.fabric,board.session.fabric,sizeof(storage_read.config.fabric));
 memcpy(storage_read.config.fabric_hz,board.session.fabric_hz,sizeof(storage_read.config.fabric_hz));
 memcpy(storage_read.config.expected,storage_read.session.expected,sizeof(storage_read.session.expected));
 memcpy(storage_read.config.desired,storage_read.session.desired,sizeof(storage_read.session.desired));
 if(!a733_mmc_platform_native_init_selected(&storage_read.native,&providers.shared.locks,storage_read.handle,
 storage_read.config.kind,storage_read.config.emmc_host,admit,&storage_read))return 0;
 return a733_mmc_platform_init(&storage_read.platform,&storage_read.config,&storage_read.native.operations,
 &storage_read.native,&providers.shared.clocks,&providers.shared.pins,&providers.shared.rails,&providers.shared.pmic);
}
static int client(void)
{
 TaskHandle_t task=xTaskGetCurrentTaskHandle();
 for(unsigned i=0;i<STORAGE_READ_CLIENTS;++i)
  if(storage_read.slots[i].client&&task==storage_read.slots[i].client)return (int)i;
 return -1;
}
int storage_read_output_ready(void)
{
 int i=client();if(i<0)return 0;struct storage_read_slot *slot=&storage_read.slots[i];
 /* SUCCESS acquire publishes the copied payload and its immutable deadline.
  * Only this same client can acknowledge and submit another request. */
 if(atomic_load_explicit(&slot->state,memory_order_acquire)!=STORAGE_READ_SUCCESS)return 0;
 return slot->epoch==storage_read.session.board.epoch&&slot->generation==storage_read.session.board.generation&&
 atomic_load_explicit(&storage_read.result,memory_order_acquire)!=STORAGE_READ_RETAINED&&
 !atomic_load_explicit(&storage_read.cancel,memory_order_acquire)&&board_live(NULL)&&
 a733_smp_counter()/24<limit()&&a733_smp_counter()/24<slot->deadline;
}
void storage_read_revoke(unsigned reason)
{
 (void)reason;
 atomic_store_explicit(&storage_read.cancel,1,memory_order_release);
 atomic_store_explicit(&storage_read.result,STORAGE_READ_RETAINED,memory_order_release);
}
static void failure(unsigned reason)
{
 storage_read.reason=reason;storage_read_revoke(reason);
 if(storage_read.platform.initialized)(void)a733_mmc_platform_retain(&storage_read.platform);
}
static void step(void)
{
 enum a733_mmc_platform_result r;uint64_t now=a733_smp_counter()/24;
 if(!board_live(NULL)||now>=limit()||
    (storage_read.deadline&&now>=storage_read.deadline)||atomic_load_explicit(&storage_read.cancel,memory_order_acquire)){
  failure(1);return;
 }
 for(unsigned i=0;i<STORAGE_READ_CLIENTS;++i){
  struct storage_read_slot *slot=&storage_read.slots[i];unsigned state=atomic_load_explicit(&slot->state,memory_order_acquire);
  /* Queue time and unread completed output share the request deadline. Any
   * expiry terminates this lifetime; retained output cannot be published late. */
  if(state!=STORAGE_READ_IDLE&&slot->deadline&&now>=slot->deadline){failure(1);return;}
 }
 if(!storage_read.initialized){
  storage_read.initialized=1;if(!construct())failure(2);return;
 }
 struct storage_read_slot *slot=storage_read.active<STORAGE_READ_CLIENTS?&storage_read.slots[storage_read.active]:NULL;
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
  storage_read.card_valid=1;storage_read.deadline=limit();storage_read.stage=2;
  atomic_store_explicit(&storage_read.result,STORAGE_READ_IDLE,memory_order_release);break;
 case 2: {
  /* card() checks current presence and providers but copies cached CID. */
  r=a733_mmc_platform_card(&storage_read.platform,&storage_read.card,limit());
  if(r==A733_MMC_BUSY)break;
  if(r!=A733_MMC_READY){failure(4);break;}
  unsigned chosen=STORAGE_READ_CLIENTS;uint64_t oldest=UINT64_MAX;
  for(unsigned i=0;i<STORAGE_READ_CLIENTS;++i)
   if(atomic_load_explicit(&storage_read.slots[i].state,memory_order_acquire)==STORAGE_READ_PENDING&&
      (chosen==STORAGE_READ_CLIENTS||storage_read.slots[i].ticket<oldest)){chosen=i;oldest=storage_read.slots[i].ticket;}
  if(chosen<STORAGE_READ_CLIENTS){
   storage_read.active=chosen;slot=&storage_read.slots[chosen];storage_read.deadline=slot->deadline;
   atomic_store_explicit(&slot->state,STORAGE_READ_ACTIVE,memory_order_release);storage_read.stage=3;
  }
  break;
 }
 case 3:
  if(!slot){failure(8);break;}
  r=a733_mmc_platform_read(&storage_read.platform,slot->request.first_sector,
   slot->request.blocks,storage_read.borrowed,slot->request.blocks*512u,slot->deadline);
  if(r==A733_MMC_PENDING)storage_read.stage=4;
  else if(r!=A733_MMC_BUSY)failure(5);
  break;
 case 4:
  r=a733_mmc_platform_poll(&storage_read.platform);
  if(r==A733_MMC_COMPLETE)storage_read.stage=5;
  else if(r!=A733_MMC_PENDING&&r!=A733_MMC_BUSY)failure(6);
  break;
 case 5:
  if(!slot){failure(8);break;}
  r=a733_mmc_platform_take(&storage_read.platform,&slot->completion,slot->deadline);
  if(r==A733_MMC_BUSY)break;
  if(r!=A733_MMC_READY||slot->completion.error!=SUNXI_MMC_OK||
     slot->completion.received!=slot->request.blocks*512u){failure(7);break;}
  storage_read.stage=6;break;
 case 6:
  if(!slot){failure(8);break;}
  /* Actual take released the PIO borrower. Copy into this client's slot before
   * allowing the next queued request to reuse that one controller buffer. */
  memcpy(slot->copied,storage_read.borrowed,slot->request.blocks*512u);
  storage_digest_bytes(slot->copied,slot->request.blocks*512u,slot->digest);
  if(!board_live(NULL)||a733_smp_counter()/24>=limit()||
    a733_smp_counter()/24>=slot->deadline||atomic_load_explicit(&storage_read.cancel,memory_order_acquire)){failure(1);break;}
  atomic_store_explicit(&slot->state,STORAGE_READ_SUCCESS,memory_order_release);
  storage_read.active=STORAGE_READ_CLIENTS;storage_read.deadline=limit();storage_read.stage=2;break;
 default:failure(8);break;
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
static int separate(const void *address,size_t bytes)
{
 uintptr_t a=(uintptr_t)address;
 if(!address||!bytes||bytes>UINTPTR_MAX-a)return 0;
 /* Board task stacks are legitimate client storage; protect its immutable
  * session records rather than excluding those entire containing stacks. */
 const void *objects[]={&storage_read,&providers,&board.session,&board.admitted_session};
 const size_t lengths[]={sizeof(storage_read),sizeof(providers),sizeof(board.session),sizeof(board.admitted_session)};
 for(unsigned i=0;i<4;++i){uintptr_t b=(uintptr_t)objects[i];
  if(a<b+lengths[i]&&b<a+bytes)return 0;}
 return 1;
}
static int slot_bound(const struct storage_read_slot *slot)
{return slot->epoch==storage_read.session.board.epoch&&slot->generation==storage_read.session.board.generation;}
int storage_read_submit(const struct storage_read_request *request,uint64_t *ticket)
{
 int i=client();if(i<0||!separate(request,sizeof(*request))||!separate(ticket,sizeof(*ticket)))return -1;
 struct storage_read_request r=*request;
 if(!r.blocks||r.blocks>STORAGE_READ_MAX_BLOCKS||r.reserved||!r.request_id)return -1;
 if(xSemaphoreTake(storage_read.mutex,0)!=pdTRUE)return 0;
 int answer=0;struct storage_read_slot *slot=&storage_read.slots[i];
 if(atomic_load_explicit(&storage_read.cancel,memory_order_acquire)||
    atomic_load_explicit(&storage_read.result,memory_order_acquire)==STORAGE_READ_RETAINED)answer=-2;
 else if(storage_read.card_valid&&slot_bound(slot)&&atomic_load_explicit(&slot->state,memory_order_acquire)==STORAGE_READ_IDLE){
  const struct sunxi_mmc_card *c=&storage_read.card;uint64_t now=a733_smp_counter()/24;
  if(r.request_id<=slot->last_request_id||r.first_sector>=c->sectors||
     r.blocks>c->sectors-r.first_sector||storage_read.next_ticket==UINT64_MAX||
     now>=limit()||storage_read.session.read_us>limit()-now)answer=-1;
  else {uint64_t last=r.first_sector+r.blocks-1;
   if((c->sector_addressed&&last>UINT32_MAX)||(!c->sector_addressed&&last>UINT32_MAX/512u))answer=-1;
   else {
    slot->request=r;slot->ticket=++storage_read.next_ticket;
    slot->last_request_id=r.request_id;slot->deadline=now+storage_read.session.read_us;
    slot->completion=(struct sunxi_mmc_result){0};
    atomic_store_explicit(&slot->state,STORAGE_READ_PENDING,memory_order_release);
    *ticket=slot->ticket;answer=1;
   }
  }
 }
 if(xSemaphoreGive(storage_read.mutex)!=pdTRUE){storage_read_revoke(1);return -2;}
 return answer;
}
int storage_read_ack(uint64_t ticket)
{
 int i=client();if(i<0||!ticket||xSemaphoreTake(storage_read.mutex,0)!=pdTRUE)return 0;
 struct storage_read_slot *slot=&storage_read.slots[i];
 int ok=slot_bound(slot)&&ticket==slot->ticket&&storage_read_output_ready();
 if(ok){slot->request=(struct storage_read_request){0};slot->ticket=0;slot->deadline=0;
  atomic_store_explicit(&slot->state,STORAGE_READ_IDLE,memory_order_release);}
 if(xSemaphoreGive(storage_read.mutex)!=pdTRUE){storage_read_revoke(1);return 0;}
 return ok;
}
int storage_read_cancel(uint64_t ticket)
{
 int i=client();if(i<0||!ticket)return -1;
 if(xSemaphoreTake(storage_read.mutex,0)!=pdTRUE)return 0;
 struct storage_read_slot *slot=&storage_read.slots[i];
 int ok=slot_bound(slot)&&ticket==slot->ticket&&atomic_load_explicit(&slot->state,memory_order_acquire)!=STORAGE_READ_IDLE;
 if(ok)storage_read_revoke(1);
 if(xSemaphoreGive(storage_read.mutex)!=pdTRUE){storage_read_revoke(1);return -1;}
 return ok?1:-1;
}
int storage_read_copy(uint8_t *out,size_t capacity,unsigned *bytes)
{
 int i=client();if(i<0||!separate(out,capacity)||!separate(bytes,sizeof(*bytes))||
    ((uintptr_t)out<(uintptr_t)bytes+sizeof(*bytes)&&(uintptr_t)bytes<(uintptr_t)out+capacity)||
    capacity<STORAGE_READ_HEADER||xSemaphoreTake(storage_read.mutex,0)!=pdTRUE)return 0;
 struct storage_read_slot *slot=&storage_read.slots[i];
 if(!slot_bound(slot)){storage_read_revoke(1);(void)xSemaphoreGive(storage_read.mutex);return 0;}
 unsigned result=atomic_load_explicit(&slot->state,memory_order_acquire);
 if(atomic_load_explicit(&storage_read.result,memory_order_acquire)==STORAGE_READ_RETAINED)result=STORAGE_READ_RETAINED;
 else if(!storage_read.card_valid||result==STORAGE_READ_ACTIVE)result=STORAGE_READ_PENDING;
 if(result==STORAGE_READ_SUCCESS&&!storage_read_output_ready()){storage_read_revoke(1);result=STORAGE_READ_RETAINED;}
 unsigned count=result==STORAGE_READ_SUCCESS?slot->request.blocks*512u:0;
 if(capacity<STORAGE_READ_HEADER+count){(void)xSemaphoreGive(storage_read.mutex);return 0;}
 memset(out,0,STORAGE_READ_HEADER);
 put32(out,0x44524453);
#ifdef SUNSTONE_EMMC_READ
 put32(out+4,2);
#else
 put32(out+4,storage_read.session.sd_profile?3:2);
#endif
 put32(out+8,result);put32(out+12,count);
 put64(out+16,board.session.epoch);put64(out+24,board.session.generation);
 const struct sunxi_mmc_card *c=storage_read.card_valid?&storage_read.card:&storage_read.platform.core.card;
 for(unsigned j=0;j<4;++j){put32(out+64+4*j,c->cid[j]);put32(out+80+4*j,c->csd[j]);}
 put32(out+96,c->ocr);out[100]=(uint8_t)c->rca;out[101]=(uint8_t)(c->rca>>8);
 out[102]=c->sector_addressed;out[103]=c->ext_revision;put64(out+104,c->sectors);
 const struct sunxi_mmc_result *r=result==STORAGE_READ_SUCCESS?&slot->completion:&storage_read.platform.core.result;
 const uint32_t facts[]={r->error,r->interrupts,r->status,r->response,r->stop_response,(uint32_t)r->received,
 (uint32_t)r->submitted,r->confirmed_blocks,r->write_started,storage_read.platform.core.state,
 storage_read.platform.stage,storage_read.platform.core.command,storage_read.platform.core.argument,
 storage_read.platform.core.seen,storage_read.platform.core.setup,storage_read.platform.core.protocol,
 storage_read.platform.core.command_step,storage_read.platform.core.response_seen,storage_read.card_valid};
 for(unsigned j=0;j<sizeof(facts)/sizeof(facts[0]);++j)put32(out+112+4*j,facts[j]);
 for(unsigned j=0;j<4;++j)put32(out+188+4*j,storage_read.platform.core.response[j]);
 put32(out+204,storage_read.reason);memcpy(out+208,a733_smp_build_id,32);
 put64(out+240,slot->ticket);put64(out+248,slot->request.first_sector);
 put64(out+256,slot->request.request_id);put32(out+264,slot->request.blocks);put32(out+268,(uint32_t)i);
 put32(out+272,atomic_load_explicit(&storage_read_consumer.result,memory_order_acquire));
 put32(out+276,atomic_load_explicit(&storage_read_consumer.completed,memory_order_acquire));
#ifndef SUNSTONE_EMMC_READ
 if(storage_read.session.sd_profile){
  put32(out+280,(uint32_t)storage_read.session.sd_profile|(c->bus_width<<8)|(c->timing<<16)|(c->module_divisor<<24));
  put32(out+284,c->card_hz);
 }
#endif
 if(result==STORAGE_READ_SUCCESS){memcpy(out+32,slot->digest,32);memcpy(out+STORAGE_READ_HEADER,slot->copied,count);}
 if(result==STORAGE_READ_SUCCESS&&!storage_read_output_ready()){storage_read_revoke(1);result=STORAGE_READ_RETAINED;
  put32(out+8,result);put32(out+12,0);memset(out+32,0,32);count=0;}
 *bytes=STORAGE_READ_HEADER+count;
 if(xSemaphoreGive(storage_read.mutex)!=pdTRUE){storage_read_revoke(1);return 0;}
 return 1;
}
