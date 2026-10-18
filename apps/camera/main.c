#include "application.h"
#include "cached.h"
#include "platform.h"
#include <string.h>
struct camera_application app;
int camera_live(void *unused)
{(void)unused;return !atomic_load_explicit(&app.fatal,memory_order_acquire)&&
 a733_smp_counter()/24<app.session.deadline_us;}
void camera_fail(void *unused)
{(void)unused;atomic_store_explicit(&app.fatal,1,memory_order_release);}
void a733_storage_maintenance_stop(void){camera_fail(NULL);}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name)
{(void)task;platform_panic(name,2);}
static void pmic(void *unused)
{
 (void)unused;for(;;){
  if(camera_live(NULL)){
   enum a733_pmic_status r=a733_pmic_poll(&app.shared.pmic);
   if(r==A733_PMIC_READY||r==A733_PMIC_COMPLETE)
    atomic_store_explicit(&app.shared.pmic_ready,1,memory_order_release);
   else if(r!=A733_PMIC_PENDING&&r!=A733_PMIC_BUSY)camera_fail(NULL);
  }
  vTaskDelay(1);
 }
}
int a733_storage_native_task_privileged(void (*entry)(void *),void *context)
{return !context&&(entry==pmic||entry==camera_usb0_task||entry==camera_capture_task);}
void a733_smp_diagnostic_start(void)
{
 if(!a733_smp_device_preparation_admitted())platform_panic("camera memory",1);
 volatile const uint8_t *source=(void *)A733_STORAGE_SESSION_ADDRESS;
 for(unsigned i=0;i<sizeof(app.session);++i)((uint8_t *)&app.session)[i]=source[i];
 if(!camera_session_valid(&app.session,a733_smp_counter())||app.session.epoch!=a733_cached_runtime[0].epoch||
    app.session.deadline_us*24!=a733_cached_runtime[0].diagnostic_deadline)platform_panic("camera session",2);
 struct camera_session *s=&app.session;
 app.window=(struct a733_watchdog_window){s->deadline_us,s->guard_us,s->rtc_control,s->rtc_divider,s->rtc_source};
 if(!a733_watchdog_hw_admit(&app.window))platform_panic("camera recovery",3);
 if(!a733_native_usb_init(&app.shared,&s->shared,camera_live,camera_fail,NULL))platform_panic("camera providers",4);
 TaskFunction_t functions[]={pmic,camera_usb0_task,camera_capture_task};
 const char *names[]={"camera-pmic","camera-usb0","camera-owner"};
 for(unsigned i=0;i<3;++i){
  app.handles[i]=xTaskCreateStatic(functions[i],names[i],2048,NULL,4,app.stacks[i],&app.tasks[i]);
  if(!app.handles[i])platform_panic("camera tasks",5);
 }
 if(!a733_native_usb_bind(&app.shared,app.handles[0],app.handles[1]))platform_panic("camera bind",6);
 /* Native construction reserves memory only under actual primary preparation.
  * These headers describe its future providers; no live lease is invented.
  * The owner task initializes media after acquiring actual rail/PCK authority. */
 app.media.config=s->media;app.media.registry=&app.shared.clocks;app.rail.owner=s->camera.owner;
 struct a733_camera_native_config config={&s->camera,&app.media,&app.shared.clocks,&app.shared.pins,
  &app.shared.rails,&app.rail,&app.shared.pmic,&app.shared.locks,app.handles[2]};
 if(!a733_camera_native_init(&app.camera,&config)||
    !device_memory_acquire(app.camera.pool,s->camera.owner,CAMERA_FRAME_BYTES,&app.output,&app.pixels))
  platform_panic("camera reservation",7);
 vTaskStartScheduler();platform_panic("camera scheduler returned",7);
}
