/* Actual descriptor callbacks, USB stop and application metadata revocation. */
#include "../apps/camera/usb0.c"
#include "../apps/camera/main.c"
volatile unsigned camera_check_result;
static unsigned scenario;
uint64_t a733_smp_counter(void){return 240;}
static void finish(int yes,unsigned code)
{camera_check_result=yes?1:code;__asm__ volatile("brk #0");for(;;){}}
void vTaskDelay(TickType_t ticks)
{
 uint64_t time=0xfeed;
 finish(ticks==1&&atomic_load(&app.fatal)==1&&atomic_load(&app.shared.counter.failed)==1&&
  !camera_live(NULL)&&!a733_counter_clock_now(&app.shared.counter,&time)&&time==0xfeed&&
  app.camera.memory.generation==17&&app.camera.state==A733_CAMERA_NATIVE_CAPTURE,20+scenario);
}
static int text(unsigned index,const char *want)
{
 const uint16_t *s=tud_descriptor_string_cb((uint8_t)index,0x409);
 if(!s||s[0]!=(uint16_t)(0x0300+2+2*strlen(want)))return 0;
 for(unsigned i=0;want[i];++i)if(s[i+1]!=(uint8_t)want[i])return 0;
 return 1;
}
void camera_check_run(unsigned mode)
{
 scenario=mode;
 if(!mode){
  const tusb_desc_device_t *d=(const void *)tud_descriptor_device_cb();
  finish(d->idVendor==0x1f3a&&d->idProduct==0xefe8&&
   text(2,"sunstoneos native camera")&&text(3,"sunstoneos-a733-camera"),10);
 }
 app.session.deadline_us=100000;app.shared.counter.initialized=1;app.shared.counter.frequency=24000000;
 app.camera.memory.generation=17;app.camera.state=A733_CAMERA_NATIVE_CAPTURE;
 if(mode==1)a733_usb_assert();
 /* An invalid vendor interface takes the same retained terminal path. */
 tud_vendor_rx_cb(1,NULL,0);
 finish(0,30);
}
