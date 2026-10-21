#include "pwm.h"
#include "boot.h"
#include "platform.h"
#include "tusb.h"
#include "device/dcd.h"
#include <string.h>
#ifdef SUNSTONE_BOARD_SUPERVISED
#define USB0_PID 0xefe8
#else
#define USB0_PID 0xefe5
#endif
static _Noreturn void stop(void){board_fail(NULL);for(;;)vTaskDelay(1);}
#define USB0_PRODUCT "sunstoneos native PWM"
#define USB0_SERIAL "sunstoneos-a733-pwm"
#define USB0_LIVE() board_live(NULL)
#define USB0_PROVIDERS (&providers.shared)
#include "../board/usb0_service.h"

/* Request0 copies a record; request1 irrevocably cancels. A revoked in-flight
 * frame is abandoned, so the host must discard incomplete transfers. */
static uint8_t response[PWM_RECORD_BYTES];
static unsigned response_bytes,response_sent,response_transferred;
void tud_mount_cb(void){}
void tud_umount_cb(void){}
void tud_suspend_cb(bool wake){(void)wake;}
void tud_vendor_rx_cb(uint8_t i,const uint8_t *b,uint32_t n)
{(void)b;(void)n;if(i)stop();}
void tud_vendor_tx_cb(uint8_t i,uint32_t n)
{if(i||n>response_sent-response_transferred)stop();response_transferred+=n;
 if(response_transferred==response_bytes&&response_sent==response_bytes)response_bytes=0;}
void tud_vendor_tx_complete_cb(uint8_t i){(void)i;}
static uint8_t command[80];
static unsigned command_bytes,command_active;
static uint64_t command_receive_deadline;
static void pwm_exchange(void)
{
 if(command_active){
  if(a733_smp_counter()/24>=command_receive_deadline){pwm_cancel();command_active=0;command_bytes=0;return;}
  unsigned available=tud_vendor_available(),remaining=80-command_bytes;
  if(available){unsigned n=available<remaining?available:remaining;
   if(tud_vendor_read(command+command_bytes,n)!=n)stop();command_bytes+=n;}
  if(command_bytes==80){int r=pwm_submit(command);if(r){command_active=0;command_bytes=0;}}
 }else if(tud_vendor_available()){
  uint8_t request;if(tud_vendor_read(&request,1)!=1||request>2)stop();
  if(request==1){pwm_cancel();response_bytes=response_sent;if(response_transferred==response_sent)response_bytes=0;}
  else if(request==2){command_active=1;command_bytes=0;command_receive_deadline=a733_smp_counter()/24+100000;}
  else if(!response_bytes){if(!pwm_copy(response))return;response_bytes=PWM_RECORD_BYTES;response_sent=response_transferred=0;}
 }
 if(response_bytes&&response_sent<response_bytes){
  if(response[8]==1&&!pwm_output_ready()){response_bytes=response_sent;if(response_transferred==response_sent)response_bytes=0;return;}
  response_sent+=tud_vendor_write(response+response_sent,response_bytes-response_sent);tud_vendor_flush();
 }
}
void board_usb0_task(void *unused)
{
    (void)unused;
    usb0_service_start();
    usb0_service_run(pwm_exchange);
}
