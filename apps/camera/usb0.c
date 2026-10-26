#include "application.h"
#include "boot.h"
#include "platform.h"
#include "a733_storage_digest.h"
#include <string.h>
static _Noreturn void stop(void){camera_fail(NULL);for(;;)vTaskDelay(1);}
#define USB0_PID 0xefe8
#define USB0_PRODUCT "sunstoneos native camera"
#define USB0_SERIAL "sunstoneos-a733-camera"
#define USB0_LIVE() camera_live(NULL)
#define USB0_PROVIDERS (&app.shared)
#include "../board/usb0_service.h"

/* Fixed 12-byte requests: LEu32 operation(0=status,1=read,2=cancel),
 * byte offset, payload bytes(up to4096). Response: magic/version/state/offset/
 * payload bytes/total bytes, epoch/generation, immutable session SHA256,
 * then payload. One request remains in flight until the device reports TX. */
#ifdef SUNSTONE_CAMERA_STREAM
#define RESPONSE_HEADER 80u
#define RESPONSE_VERSION 2u
#else
#define RESPONSE_HEADER 72u
#define RESPONSE_VERSION 1u
#endif
static uint8_t response[RESPONSE_HEADER+4096],request[12];
static unsigned response_bytes,response_sent,response_transferred,request_bytes;
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static uint32_t get32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
void tud_mount_cb(void){}
void tud_umount_cb(void){}
void tud_suspend_cb(bool wake){(void)wake;}
void tud_vendor_rx_cb(uint8_t interface,const uint8_t *buffer,uint32_t bytes)
{(void)buffer;(void)bytes;if(interface)stop();}
static void queue_response(void)
{response_sent+=tud_vendor_write(response+response_sent,response_bytes-response_sent);}
/* The pinned vendor class decides on a zero-length packet right after this
 * callback. Refilling here keeps an empty FIFO from ending a response early. */
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{if(interface||bytes>response_sent-response_transferred)stop();response_transferred+=bytes;
 if(response_transferred==response_bytes&&response_sent==response_bytes)response_bytes=0;
 else if(response_sent<response_bytes)queue_response();}
void tud_vendor_tx_complete_cb(uint8_t interface){(void)interface;}
static void exchange(void)
{
 if(response_bytes){queue_response();tud_vendor_flush();return;}
 unsigned available=tud_vendor_available();if(!available)return;
 if(available>sizeof(request)-request_bytes)stop();
 request_bytes+=tud_vendor_read(request+request_bytes,available);
 if(request_bytes<sizeof(request))return;request_bytes=0;
 uint32_t op=get32(request),offset=get32(request+4),bytes=get32(request+8);
 if(op>2||bytes>4096||offset>CAMERA_FRAME_BYTES||bytes>CAMERA_FRAME_BYTES-offset||
    (op!=1&&(offset||bytes)))stop();
 if(op==2)atomic_store_explicit(&app.cancel,1,memory_order_release);
 unsigned result=atomic_load_explicit(&app.result,memory_order_acquire);
 if(atomic_load_explicit(&app.cancel,memory_order_acquire))result=3;
 unsigned count=0;if(op==1&&(result==2||result==4)&&bytes){
  if(camera_output(offset,response+RESPONSE_HEADER,bytes))count=bytes;else result=3;
 }
 put32(response,0x524d4143);put32(response+4,RESPONSE_VERSION);put32(response+8,result);put32(response+12,offset);
 put32(response+16,count);put32(response+20,CAMERA_FRAME_BYTES);
 put32(response+24,(uint32_t)app.session.epoch);put32(response+28,(uint32_t)(app.session.epoch>>32));
 put32(response+32,(uint32_t)app.session.generation);put32(response+36,(uint32_t)(app.session.generation>>32));
 storage_digest_bytes(
#ifdef SUNSTONE_CAMERA_SUPERVISED
 &app.admitted,sizeof(app.admitted),
#else
 &app.session,sizeof(app.session),
#endif
 response+40);
#ifdef SUNSTONE_CAMERA_STREAM
 put32(response+72,result==4?app.retained_status:0);put32(response+76,0);
#endif
 response_sent=response_transferred=0;response_bytes=RESPONSE_HEADER+count;
}
void camera_usb0_task(void *unused)
{
    (void)unused;
    usb0_service_start();
    usb0_service_run(exchange);
}
