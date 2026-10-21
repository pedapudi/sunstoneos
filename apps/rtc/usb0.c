#include "rtc.h"
#include "boot.h"
#include "platform.h"
#include "tusb.h"
#include "device/dcd.h"
#include <string.h>
#define USB0_PID 0xefeb
static _Noreturn void stop(void){board_fail(NULL);for(;;)vTaskDelay(1);}
#define USB0_PRODUCT "sunstoneos native RTC"
#define USB0_SERIAL "sunstoneos-a733-rtc"
#define USB0_LIVE() board_live(NULL)
#define USB0_PROVIDERS (&providers.shared)
#include "../board/usb0_service.h"

/* Request0 observes; request1 requests cancellation. Header64B followed by
 * an immutable88B adoption or136B initialization snapshot only on success. All integers are little endian. */
_Static_assert(sizeof(struct a733_rtc_snapshot)==88,"RTC result layout");
_Static_assert(sizeof(struct rtc_runtime_output)==104,"RTC runtime result layout");
_Static_assert(sizeof(struct rtc_initialized_output)==136,"RTC initialized result layout");
_Static_assert(sizeof(struct rtc_service_result)==200,"RTC service record layout");
_Static_assert(sizeof(struct rtc_service_output)==2416,"RTC service transcript layout");
static uint8_t response[64+sizeof(struct rtc_service_output)];
static unsigned response_bytes,response_sent,response_transferred;
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
void tud_mount_cb(void){}
void tud_umount_cb(void){}
void tud_suspend_cb(bool wake){(void)wake;}
void tud_vendor_rx_cb(uint8_t interface,const uint8_t *buffer,uint32_t bytes)
{(void)buffer;(void)bytes;if(interface)stop();}
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{if(interface||bytes>response_sent-response_transferred)stop();response_transferred+=bytes;
 if(response_transferred==response_bytes&&response_sent==response_bytes)response_bytes=0;}
void tud_vendor_tx_complete_cb(uint8_t interface){if(interface)stop();}
static void rtc_exchange(void)
{
 if(response_bytes){unsigned n=tud_vendor_write(response+response_sent,response_bytes-response_sent);
  if(n>response_bytes-response_sent)stop();response_sent+=n;tud_vendor_flush();return;}
 if(!tud_vendor_available())return;
 uint8_t request;if(tud_vendor_available()!=1||tud_vendor_read(&request,1)!=1||request>1)stop();
 if(request&&rtc.session.version==4)stop();
 if(request)atomic_store_explicit(&rtc.cancel,1,memory_order_release);
 union {struct rtc_initialized_output initialized;struct rtc_runtime_output runtime;struct rtc_service_output service;} snapshot;uint8_t hash[32];
 unsigned initialized=rtc.session.version==2,runtime=rtc.session.version==3,service=rtc.session.version==4;
 unsigned result=service?(unsigned)rtc_copy_service(&snapshot.service,hash):runtime?(unsigned)rtc_copy_runtime(&snapshot.runtime,hash):
     initialized?(unsigned)rtc_copy_initialized(&snapshot.initialized,hash):
     (unsigned)rtc_copy(&snapshot.initialized.calendar,hash);
 unsigned payload=service?sizeof(snapshot.service):runtime?sizeof(snapshot.runtime):initialized?sizeof(snapshot.initialized):sizeof(snapshot.initialized.calendar);
 memset(response,0,sizeof(response));
 put32(response,0x31435452);put32(response+4,service?4:runtime?3:initialized?2:1);put32(response+8,result);
 put32(response+12,result==1?payload:0);
 put32(response+16,(uint32_t)board.session.epoch);put32(response+20,(uint32_t)(board.session.epoch>>32));
 put32(response+24,(uint32_t)board.session.generation);put32(response+28,(uint32_t)(board.session.generation>>32));
 if(result==1){memcpy(response+32,hash,32);memcpy(response+64,&snapshot,payload);}
 response_sent=response_transferred=0;response_bytes=result==1?64+payload:64;
}
void board_usb0_task(void *unused)
{
    (void)unused;
    usb0_service_start();
    usb0_service_run(rtc_exchange);
}
