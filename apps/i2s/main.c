#include "application.h"
#include "snapshot.h"
#include "a733_i2s_platform_native.h"
#include "aarch64_device_memory.h"
#include "cached.h"
#include "platform.h"
struct i2s_application app;
int i2s_live(void *unused)
{(void)unused;return !atomic_load_explicit(&app.fatal,memory_order_acquire)&&a733_smp_counter()/24<app.deadline;}
void i2s_fail(void *unused)
{
 (void)unused;
 atomic_store_explicit(&app.fatal,1,memory_order_release);
#ifdef SUNSTONE_I2S_TRANSCRIBE
 i2s_recognition_abort();
#endif
 /* Revoke the common time provider as metadata only. DMA worker callbacks
  * inspect this fault before device/cache access; startup and lease callbacks
  * also refuse subsequent time samples. Keep every active reservation held. */
 atomic_store_explicit(&app.shared.counter.failed,1,memory_order_release);
}
void a733_storage_maintenance_stop(void){i2s_fail(NULL);}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name){(void)task;platform_panic(name,2);}
static _Noreturn void dma_fatal(void *unused)
{(void)unused;i2s_fail(NULL);platform_panic("I2S retained DMA",1);}
static int worker(void)
{return i2s_live(NULL)&&xTaskGetCurrentTaskHandle()==
 (atomic_load_explicit(&app.audio_ready,memory_order_acquire)?app.dma.service.worker:app.handles[2]);}
static int now(void *v,uint64_t *out)
{return v==&app&&worker()&&a733_counter_clock_now(&app.shared.counter,out);}
static int enter(void *v,uint64_t d)
{return v==&app&&worker()&&a733_platform_lock_enter(&app.shared.locks,A733_PLATFORM_REGISTER_LOCK,d);}
static void leave(void *v)
{configASSERT(v==&app);a733_platform_lock_leave(&app.shared.locks,A733_PLATFORM_REGISTER_LOCK);}
static int mapped(uintptr_t a,int write)
{struct aarch64_translation t;return aarch64_device_translate(a,write,&t)&&t.physical==a&&t.attributes==0;}
static int read_reg(void *v,uintptr_t a,uint32_t *out)
{return v==&app&&worker()&&mapped(a,0)&&a733_audio_native_read(NULL,a,out)==1;}
static int write_reg(void *v,uintptr_t a,uint32_t value)
{return v==&app&&worker()&&mapped(a,1)&&a733_audio_native_write(NULL,a,value)==1;}
int i2s_prepare_audio(void)
{
 if(atomic_load_explicit(&app.audio_ready,memory_order_acquire))return 1;
 if(xTaskGetCurrentTaskHandle()!=app.handles[2]||!i2s_live(NULL))return -1;
 enum a733_i2s_resource_result r=a733_audio_parent_poll(&app.parent);
 if(r==A733_I2S_RESOURCE_BUSY||r==A733_I2S_RESOURCE_PENDING)return 0;
 if(r!=A733_I2S_RESOURCE_READY||!a733_audio_parent_ready(&app.parent))return -1;
 /* OPEN is not queued until after this publication. From here only the DMA
  * worker enters the backend; the client never polls the parent again. */
 atomic_store_explicit(&app.audio_ready,1,memory_order_release);return 1;
}
static void pmic(void *unused)
{
 (void)unused;for(;;){if(i2s_live(NULL)){
  enum a733_pmic_status r=a733_pmic_poll(&app.shared.pmic);
  if(r==A733_PMIC_READY||r==A733_PMIC_COMPLETE)atomic_store_explicit(&app.shared.pmic_ready,1,memory_order_release);
  else if(r!=A733_PMIC_PENDING&&r!=A733_PMIC_BUSY)i2s_fail(NULL);
 }vTaskDelay(1);}
}
int a733_storage_native_task_privileged(void (*entry)(void *),void *context)
{return (!context&&(entry==pmic||entry==i2s_usb0_task||entry==i2s_client_task))||
 a733_dma_pool_task_privileged(entry,context);}
void a733_smp_diagnostic_start(void)
{
 if(!a733_smp_device_preparation_admitted())platform_panic("I2S primary",1);
 const struct i2s_session *source=i2s_admitted_session();
 if(!source)platform_panic("I2S sealed session",2);
 app.admitted=*source;
 if(!i2s_session_valid(&app.admitted)||app.admitted.epoch!=a733_cached_runtime[0].epoch)
  platform_panic("I2S admission",2);
 app.session=app.admitted;app.deadline=a733_cached_runtime[0].diagnostic_deadline/24;
 struct i2s_session *s=&app.session;
 s->shared.pmic.startup_deadline=s->shared.usb.deadline_us=s->dma.allowed.deadline_us=app.deadline;
 if(!a733_native_usb_init(&app.shared,&s->shared,i2s_live,i2s_fail,NULL))platform_panic("I2S providers",3);
 TaskFunction_t functions[]={pmic,i2s_usb0_task,i2s_client_task};const char *names[]={"i2s-pmic","i2s-usb0","i2s-client"};
 for(unsigned i=0;i<3;++i){app.handles[i]=xTaskCreateStatic(functions[i],names[i],2048,NULL,4,app.stacks[i],&app.tasks[i]);
  if(!app.handles[i])platform_panic("I2S tasks",4);}
 if(!a733_native_usb_bind(&app.shared,app.handles[0],app.handles[1]))platform_panic("I2S bind",5);
 struct a733_audio_operations backend={.enter=enter,.leave=leave,.now_us=now,.read=read_reg,.write=write_reg};
 if(!a733_audio_leases_init(&app.leases,&app.shared.clocks,s->dma.allowed.owner,&backend,&app)||
    !a733_audio_parent_init(&app.parent,&app.leases.operations,&app.leases,s->audio_cold,app.deadline))platform_panic("I2S audio",6);
 unsigned direction=s->direction==I2S_RUNTIME_DIRECTION?1:s->direction%3,duplex=direction==2;
 struct a733_pin_request pins[]={{5,s->inherited[0],{3,0,0}},{6,s->inherited[1],{3,0,0}},
  {duplex?8u:direction?7u:8u,s->inherited[2],{3,0,0}},{7,s->inherited[3],{3,0,0}}};
 int bound=duplex?a733_i2s_platform_duplex_init(&app.binding,&app.parent,&app.shared.pins,&app.leases,
    s->dma.allowed.owner,pins,4):
    a733_i2s_platform_stream_init(&app.binding,&app.parent,&app.shared.pins,&app.leases,
    s->dma.allowed.owner,pins,3,(enum a733_i2s_direction)direction);
 if(!bound)platform_panic("I2S pins",7);
 for(unsigned j=0;j<(duplex?2u:1u);++j){
  app.stream[j].stream=(struct a733_i2s_stream_profile){{24576000,16000,0,16,32,1,0},
   a733_i2s_platform_stream_operations(),&app.binding,
   duplex?(j?A733_I2S_TRANSMIT:A733_I2S_RECEIVE):(enum a733_i2s_direction)direction,10000,s->direction>=3&&s->direction<=5};
  app.stream[j].duplex=duplex;
 }
 struct a733_dma_pool_config config={&s->dma,&app.shared.counter,&app.shared.clocks,&app.shared.locks,
  240000,4096,5,1,dma_fatal,&app};
 if(!a733_dma_pool_session_stream_supervised_init(&app.dma,&config,app.stream,I2S_PERIODS,I2S_PERIOD_BYTES)||
    !dma_service_bind(&app.dma.service,0,app.handles[2],duplex?3:1))platform_panic("I2S native pool",8);
 for(unsigned i=0;i<I2S_PERIODS;++i){
  app.period_memory[i]=app.dma.stream_mappings[i].cpu;
  if(duplex)app.playback_memory[i]=app.dma.duplex_mappings[i].cpu;
 }
#ifdef SUNSTONE_I2S_TRANSCRIBE
 if(!i2s_recognition_start(&s->models))platform_panic("I2S recognition",10);
#endif
 vTaskStartScheduler();platform_panic("I2S scheduler returned",9);
}
