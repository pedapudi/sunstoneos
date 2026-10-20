/* One supervised board composition; all resources remain retained. */
#include "application.h"
#include "boot.h"
#include "a733_storage_digest.h"
#include <string.h>
struct dp_application dp_app;
static void display_task(void *);
static void typec_task(void *);
static void power_task(void *);
int board_application_task_privileged(void (*entry)(void *),void *context)
{return !context&&(entry==display_task||entry==typec_task||entry==power_task);}
int board_application_pins(struct a733_pin_handoff *pins)
{
 const struct a733_displayport_session *s=displayport_admitted_session();
 if(!pins||!s||memcmp(&s->board,&board.admitted_session,sizeof(s->board))||
 !a733_displayport_session_valid(s))return 0;
 pins->authorized[A733_PIN_L]|=(1u<<8)|(1u<<9)|(1u<<12)|(1u<<13);
 pins->latch[0]=s->latch;return 1;
}
int board_application_start(void)
{
 const struct a733_displayport_session *s=displayport_admitted_session();
 if(!s||memcmp(&s->board,&board.admitted_session,sizeof(s->board))||!a733_displayport_session_valid(s)||
 memcmp(s->board.image,a733_smp_build_id,32)||board.session.deadline_us<50000)return 0;
 dp_app.session=*s;dp_app.deadline=board.session.deadline_us-50000;
 atomic_init(&dp_app.terminal,0);atomic_init(&dp_app.pd_enabled,0);atomic_init(&dp_app.typec_off,0);
 dp_app.memory.self=&dp_app.memory;dp_app.initialized=1;
 if(!dp_memory_boot())return 0;
 TaskFunction_t fn[]={display_task,typec_task,power_task};const char *names[]={"dp-output","dp-typec","dp-power"};
 for(unsigned i=0;i<3;i++){
  dp_app.handles[i]=xTaskCreateStatic(fn[i],names[i],4096,NULL,i?5:4,dp_app.stacks[i],&dp_app.tasks[i]);
  if(!dp_app.handles[i])return 0;
 }
 struct a733_typec_native_config tc={.clocks=&providers.shared.clocks,.pins=&providers.shared.pins,
  .rails=&providers.shared.rails,.pmic=&providers.shared.pmic,.locks=&providers.shared.locks,
  .provider_task=dp_app.handles[1],.power_task=dp_app.handles[2]};
 tc.electrical=(struct a733_typec_config){.owner=s->typec_owner,.generation=s->board.generation,
  .qualification=s->typec_qualification,.recovery=s->typec_recovery,
  .source_min_mv=s->source_min_mv,.source_max_mv=s->source_max_mv,.source_ma=s->source_ma,
  .zero_threshold_max_mv=s->zero_threshold_max_mv,.device_id=(uint8_t)s->fusb_device_id,.displayport=1};
 for(unsigned i=0;i<4;i++)tc.expected[i]=(struct a733_pin_config){s->typec_pins[i][0],s->typec_pins[i][1],s->typec_pins[i][2]};
 return a733_typec_native_init(&dp_app.typec,&tc);
}
static void typec_task(void *unused)
{
 (void)unused;unsigned started=0;
 for(;;){
  if(!board_live(NULL)||a733_smp_counter()/24>=dp_app.deadline)a733_typec_revoke(&dp_app.typec.provider);
  if(!started&&atomic_load_explicit(&dp_app.pd_enabled,memory_order_acquire)&&board_live(NULL)&&
     atomic_load_explicit(&providers.shared.pmic_ready,memory_order_acquire)){
   if(a733_typec_begin(&dp_app.typec.provider,dp_app.deadline)<0)a733_typec_revoke(&dp_app.typec.provider);started=1;
  }
  if(started)(void)a733_typec_poll(&dp_app.typec.provider);
  if(dp_app.typec.provider.state==A733_TYPEC_OFF_RETAINED&&
     atomic_load_explicit(&dp_app.typec.output_state,memory_order_acquire)==1)
   atomic_store_explicit(&dp_app.typec_off,1,memory_order_release);
  /* Keep a pending byte/block transfer moving within its5ms deadline.
   * Yield to the independent equal-priority power task between polls. */
  if(started&&dp_app.typec.provider.state!=A733_TYPEC_OFF_RETAINED&&
     dp_app.typec.provider.state!=A733_TYPEC_UNCERTAIN&&
     (dp_app.typec.provider.transaction||dp_app.typec.provider.pd.transaction))taskYIELD();
  else vTaskDelay(1);
 }
}
static void power_task(void *unused)
{
 (void)unused;for(;;){
  if(!board_live(NULL)||a733_smp_counter()/24>=dp_app.deadline)a733_typec_revoke(&dp_app.typec.provider);
  (void)a733_typec_native_power_poll(&dp_app.typec);vTaskDelay(1);
 }
}
void dp_publish(unsigned success)
{
 struct a733_displayport_reply *r=&dp_app.reply;
 memset(r,0,sizeof(*r));r->magic=A733_DISPLAYPORT_RESULT_MAGIC;r->version=1;r->bytes=sizeof(*r);
 r->status=success?A733_DISPLAYPORT_READY:A733_DISPLAYPORT_FAULT;r->retained=1;
 r->epoch=dp_app.session.board.epoch;r->generation=dp_app.session.board.generation;r->lease=dp_app.session.board.lease;
 r->pd_generation=dp_app.native.token.generation;r->hpd_irq=dp_app.native.token.hpd_irq;
 memcpy(r->session_digest,displayport_admitted_session_digest(),32);memcpy(r->build_id,a733_smp_build_id,32);
 memcpy(r->pixel_digest,dp_app.memory.pixel_digest,32);
 r->allocation_cpu=dp_app.memory.extent.cpu;r->allocation_physical=dp_app.memory.extent.physical;r->allocation_bytes=dp_app.memory.extent.bytes;
 r->pixel_iova=A733_DISPLAYPORT_PIXEL_IOVA;r->pitch=A733_DISPLAYPORT_STRIDE;r->width=A733_DISPLAYPORT_WIDTH;r->height=A733_DISPLAYPORT_HEIGHT;
 r->de_state=dp_app.memory.de.state;r->iommu_state=dp_app.memory.iommu.state;
 r->scanned_masters=dp_app.memory.iommu.fault.scanned_masters;r->guards=dp_app.guards;r->padding_valid=dp_app.padding_valid;
 r->polarity=dp_app.native.token.polarity;r->pin_assignment=dp_app.native.token.pin_assignment;r->hpd=dp_app.native.token.hpd;
 struct a733_dp_result core=dp_app.native.controller.result;
 r->error=core.error;r->stage=core.stage;r->core_bytes=sizeof(core);memcpy(r->core,&core,sizeof(core));
 atomic_store_explicit(&dp_app.terminal,1,memory_order_release);
}
static void display_task(void *unused)
{
 (void)unused;unsigned stage=0;uint64_t stop_at=0;int result;
 for(;;){
  if(atomic_load_explicit(&dp_app.terminal,memory_order_acquire)){vTaskSuspend(NULL);continue;}
  if(!dp_current()||!board_live(NULL)||a733_smp_counter()/24>=dp_app.deadline)goto fault;
  switch(stage){
  case 0:result=dp_native_reserve();if(result<0)goto fault;if(result){atomic_store_explicit(&dp_app.pd_enabled,1,memory_order_release);stage=1;}break;
  case 1:result=a733_typec_dp_snapshot(&dp_app.typec.provider,&dp_app.native.token,dp_app.deadline);
   if(result<0)goto fault;if(result&&dp_app.native.token.hpd)stage=2;break;
  case 2:result=dp_native_prepare();if(result<0)goto fault;if(result)stage=3;break;
  case 3:result=dp_memory_prepare();if(result<0)goto fault;if(result){
    if(dp_memory_frame(1)!=1||dp_native_start()!=1)goto fault;stop_at=a733_smp_counter()/24+250000;stage=4;}break;
  case 4:if(!a733_dp_service(&dp_app.native.controller)||dp_memory_service()<0)goto fault;
   result=dp_memory_frame(0);if(result<0)goto fault;if(result)dp_app.fresh_frame=1;
   if(a733_smp_counter()/24>=stop_at){if(!dp_app.fresh_frame||dp_native_stop()!=1)goto fault;stage=5;}break;
  case 5:result=dp_memory_stop();if(result<0)goto fault;if(result){
    if(dp_app.memory.iommu.fault.scanned_masters!=0x100||!dp_memory_guards())goto fault;
    dp_app.guards=1;dp_app.padding_valid=1;a733_typec_revoke(&dp_app.typec.provider);stage=6;}break;
  case 6:if(atomic_load_explicit(&dp_app.typec_off,memory_order_acquire))dp_publish(1);break;
  default:goto fault;
  }
  vTaskDelay(1);continue;
 fault:
  if(!dp_app.native.controller.result.error)dp_app.native.controller.result.error=A733_DP_AUTHORITY;
  dp_app.native.controller.result.state=A733_DP_FAILED_RETAINED;
  dp_retain();a733_typec_revoke(&dp_app.typec.provider);dp_publish(0);
 }
}
