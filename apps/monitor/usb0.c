#include "monitor.h"
#include "boot.h"
#include "a733_storage_digest.h"
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
#define USB0_PRODUCT "sunstoneos native monitor"
#define USB0_SERIAL "sunstoneos-a733-monitor"
#define USB0_LIVE() board_live(NULL)
#define USB0_PROVIDERS (&providers.shared)
#include "../board/usb0_service.h"

/* GET: byte0 followed by LE64 last seen generation; STOP: byte1.
 * Each copied frame has128-byte header and an optional192-byte snapshot. */
static uint8_t response[320],request[9];
static unsigned response_bytes,response_sent,response_transferred,request_bytes;
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
void tud_mount_cb(void){}
void tud_umount_cb(void){}
void tud_suspend_cb(bool wake){(void)wake;}
void tud_vendor_rx_cb(uint8_t interface,const uint8_t *buffer,uint32_t bytes)
{(void)buffer;(void)bytes;if(interface)stop();}
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{if(interface||bytes>response_sent-response_transferred)stop();response_transferred+=bytes;
 if(response_transferred==response_bytes&&response_sent==response_bytes)response_bytes=0;}
void tud_vendor_tx_complete_cb(uint8_t interface){(void)interface;}
static void monitor_exchange(void)
{
 if(response_bytes){
  if(response[8]==MONITOR_SNAPSHOT&&!monitor_output_eligible())stop();
  unsigned n=tud_vendor_write(response+response_sent,response_bytes-response_sent);
  response_sent+=n;tud_vendor_flush();return;}
 if(!tud_vendor_available())return;
 unsigned wanted=request_bytes?9:1;
 unsigned n=tud_vendor_read(request+request_bytes,wanted-request_bytes);request_bytes+=n;
 if(!request_bytes)return;
 if(request[0]>1)stop();
 if(!request[0]&&request_bytes<9){
  n=tud_vendor_read(request+request_bytes,9-request_bytes);request_bytes+=n;if(request_bytes<9)return;}
 struct monitor_snapshot snapshot;enum monitor_status result;
 if(request[0])result=monitor_request_stop();
 else {uint64_t after=0;for(unsigned i=0;i<8;++i)after|=(uint64_t)request[i+1]<<(8*i);
  result=monitor_read_snapshot(&snapshot,after);}
 request_bytes=0;
 memset(response,0,sizeof(response));put32(response,0x314e4f4d);put32(response+4,1);put32(response+8,result);
 put32(response+12,result==MONITOR_SNAPSHOT?sizeof(snapshot):0);
 memcpy(response+16,&board.session.epoch,8);memcpy(response+24,&board.session.generation,8);
 memcpy(response+32,monitor.session_digest,32);
 if(result==MONITOR_SNAPSHOT){memcpy(response+128,&snapshot,sizeof(snapshot));storage_digest_bytes(&snapshot,sizeof(snapshot),response+64);}
 if(result==MONITOR_SNAPSHOT&&!monitor_output_eligible()){
  result=MONITOR_FAILED;put32(response+8,result);put32(response+12,0);memset(response+64,0,64);}
 response_sent=response_transferred=0;response_bytes=result==MONITOR_SNAPSHOT?sizeof(response):128;
}
void board_usb0_task(void *unused)
{
 (void)unused;
 usb0_service_start();
 usb0_service_run(monitor_exchange);
}
