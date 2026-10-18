#include "pwm.h"
#include "boot.h"
#include "aarch64_device_memory.h"
#include "a733_storage_digest.h"
#include <string.h>
struct pwm_application pwm;
__attribute__((used,retain)) const uint64_t pwm_geometry[5]={A733_PWM_SESSION_MAGIC,784,256,25000,4000};
static int live(void)
{return board_live(NULL)&&pwm.deadline&&a733_smp_counter()/24<pwm.deadline;}
static int mappings(void)
{
 static const uintptr_t a[]={0x02527000,0x02527240,0x02002784,0x02002510,0x07090160,
 0x02000040,0x02000048,0x02000090,0x03400000,0x03400108,0x03400188,0x03406218};
 for(unsigned i=0;i<sizeof(a)/sizeof(a[0]);++i){struct aarch64_translation t;
  if(!aarch64_device_translate(a[i],1,&t)||t.physical!=a[i]||t.attributes||t.shareability!=2)return 0;}
 return 1;
}
static int authorize(void *v,const struct a733_pwm_config *c,const struct a733_clock_token *t)
{
 return v==&pwm&&live()&&!atomic_load_explicit(&pwm.cancel,memory_order_acquire)&&
 mappings()&&!memcmp(c,&pwm.config,sizeof(*c))&&t->owner==pwm.session.owner&&
 t->exclusive==A733_CLOCK_PWM0&&a733_clock_token_status(&providers.shared.clocks,t)==A733_CLOCK_OK;
}
void pwm_cancel(void)
{
 atomic_store_explicit(&pwm.cancel,1,memory_order_release);
 atomic_store_explicit(&pwm.result,2,memory_order_release);
}
/* Only owned GIC masking and publication happen in ISR context; PWM/MMIO and
 * pin/rail managers remain exclusively in the designated task. */
void pwm_application_irq(void)
{
 UBaseType_t flags=taskENTER_CRITICAL_FROM_ISR();
 if(!pwm.masked){pwm.masked=1;if(!gicv3_spi_mask(&pwm.irq))pwm_cancel();}
 atomic_store_explicit(&pwm.pending,1,memory_order_release);
 taskEXIT_CRITICAL_FROM_ISR(flags);
}
static void mask(void)
{
 taskENTER_CRITICAL();
 if(!pwm.masked){pwm.masked=1;if(!gicv3_spi_mask(&pwm.irq))pwm_cancel();}
 taskEXIT_CRITICAL();
}
static int rearm(void)
{
 int result=0;taskENTER_CRITICAL();
 if(live()&&!atomic_load_explicit(&pwm.cancel,memory_order_acquire)&&pwm.masked){
  result=gicv3_spi_rearm(&pwm.irq);
  if(result==1){pwm.masked=0;pwm.rearm_wait=0;}
  else if(result<0||++pwm.rearm_wait>=100)pwm_cancel();
 }
 taskEXIT_CRITICAL();return result==1;
}
static int construct(void)
{
 if(!board_live(NULL)||board.session.deadline_us<=10000||!mappings())return 0;
 uint64_t n=a733_smp_counter()/24,limit=board.session.deadline_us-10000;
 if(n>=limit||pwm.session.operation_us>limit-n)return 0;
 pwm.deadline=n+pwm.session.operation_us;
 pwm.irq=(struct gicv3_spi){0x03400000,A733_PWM_INTID};pwm.masked=1;
 pwm.config=(struct a733_pwm_config){.owner=pwm.session.owner,.handoff=pwm.session.authority,.deadline_us=pwm.deadline,
 .apb0=pwm.session.apb0,.apb0_hz=pwm.session.apb0_hz,.period_ns=25000,.duty_ns=12500,.pulses=4000,.active_high=1,
 .expected=pwm.session.expected,.desired=pwm.session.desired};
 storage_digest_bytes(&pwm.session,sizeof(pwm.session),pwm.session_digest);
 return a733_pwm_native_init(&pwm.native,&providers.shared.locks,pwm.handle,&pwm.config,authorize,&pwm)&&
 a733_pwm_init(&pwm.driver,&pwm.config,&pwm.native.operations,&pwm.native,
 &providers.shared.clocks,&providers.shared.pins,&providers.shared.rails,&providers.shared.pmic);
}
int pwm_output_ready(void)
{return live()&&!atomic_load_explicit(&pwm.cancel,memory_order_acquire)&&atomic_load_explicit(&pwm.result,memory_order_acquire)==1;}
static void step(void)
{
 if(pwm.constructed&&(!live()||atomic_load_explicit(&pwm.cancel,memory_order_acquire)))pwm_cancel();
 if(atomic_load_explicit(&pwm.result,memory_order_acquire)==2){
  mask();pwm.retaining=1;
  if(pwm.driver.initialized)(void)a733_pwm_retain(&pwm.driver);
  return;
 }
 if(atomic_load_explicit(&pwm.result,memory_order_acquire)==1)return;
 if(!pwm.constructed){pwm.constructed=1;if(!construct()){pwm.error=2;pwm_cancel();}return;}
 enum a733_pwm_result r;
 if(pwm.active==1){r=a733_pwm_start(&pwm.driver);if(r==A733_PWM_RUNNING)pwm.active=2;}
 else{
  unsigned notice=atomic_exchange_explicit(&pwm.pending,0,memory_order_acq_rel);
  r=a733_pwm_poll(&pwm.driver,notice);
  if(notice&&r==A733_PWM_BUSY)atomic_store_explicit(&pwm.pending,1,memory_order_release);
  if(r==A733_PWM_ARMED&&rearm())pwm.active=1;
 }
 if(r==A733_PWM_COMPLETE){
  if(!live()||atomic_load_explicit(&pwm.cancel,memory_order_acquire))pwm_cancel();
  else atomic_store_explicit(&pwm.result,1,memory_order_release);
 }else if(r==A733_PWM_RETAINED||r==A733_PWM_INVALID){pwm.error=3;pwm_cancel();mask();(void)a733_pwm_retain(&pwm.driver);}
}
void pwm_task(void *unused)
{
 (void)unused;
 for(;;){if(xSemaphoreTake(pwm.mutex,0)==pdTRUE){step();if(xSemaphoreGive(pwm.mutex)!=pdTRUE)pwm_cancel();}vTaskDelay(1);}
}
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static void put64(uint8_t *p,uint64_t n){put32(p,(uint32_t)n);put32(p+4,(uint32_t)(n>>32));}
int pwm_copy(uint8_t out[256])
{
 if(xSemaphoreTake(pwm.mutex,0)!=pdTRUE)return 0;
 unsigned status=atomic_load_explicit(&pwm.result,memory_order_acquire);
 if(status==1&&!pwm_output_ready()){pwm_cancel();status=2;}
 memset(out,0,256);put32(out,0x314d5750);put32(out+4,1);put32(out+8,status);put32(out+12,256);
 put64(out+16,pwm.session.board.epoch);put64(out+24,pwm.session.board.generation);
 memcpy(out+32,a733_smp_build_id,32);memcpy(out+64,pwm.session_digest,32);
 const struct a733_pwm_report *r=&pwm.driver.report;
 const uint32_t fields[]={r->phase,r->error,r->started,r->irq_seen,r->pisr,r->pcr,r->ppr,r->counter,r->pulse_counter,
 r->waveform_stopped,r->gpio_low,r->bgr,r->apb0,r->voltage_mode,r->voltage_value,r->reserved};
 for(unsigned i=0;i<16;++i)put32(out+96+4*i,fields[i]);put64(out+160,r->started_us);put64(out+168,r->completed_us);
 put32(out+176,pwm.driver.plan.pcr);put32(out+180,pwm.driver.plan.ppr);put32(out+184,pwm.driver.plan.ticks);
 put32(out+188,pwm.driver.plan.active);put64(out+192,pwm.driver.plan.duration_us);
 const struct a733_pmic_snapshot *s=&pwm.driver.supply;
 put32(out+200,s->rail);out[204]=s->page_before;out[205]=s->chip;out[206]=s->revision;out[207]=s->enable;
 out[208]=s->selector;out[209]=s->page_after;put32(out+212,s->enabled);put32(out+216,s->decoded);put32(out+220,s->programmed_uv);
 put64(out+224,pwm.deadline);put32(out+232,pwm.driver.initialized);put32(out+236,pwm.driver.fault);
 put32(out+240,pwm.driver.clock_retained);put32(out+244,pwm.driver.rail_retained);put32(out+248,pwm.error);
 if(status==1&&!pwm_output_ready()){pwm_cancel();put32(out+8,2);}
 if(xSemaphoreGive(pwm.mutex)!=pdTRUE){pwm_cancel();return 0;}return 1;
}
