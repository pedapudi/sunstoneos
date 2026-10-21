#include "dsi.h"
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
#define USB0_PRODUCT "sunstoneos native DSI"
#define USB0_SERIAL "sunstoneos-a733-dsi"
#define USB0_LIVE() board_live(NULL)
#define USB0_PROVIDERS (&providers.shared)
#include "../board/usb0_service.h"

/* One zero-valued byte requests a coherent private result. Header of 128 bytes:
 * LE32 magic/version/status/bytes, LE64 epoch/generation, session/payload SHA256
 * and 32 reserved zero bytes. Only status 1 appends 256 copied observation bytes.
 * No application buffer or DMA ownership is transferred; status 2 has no payload. */
static uint8_t dsi_response[128+DSI_OBSERVATION_BYTES];
static unsigned dsi_response_bytes,dsi_response_sent,dsi_response_transferred;
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
void tud_mount_cb(void){}
void tud_umount_cb(void){}
void tud_suspend_cb(bool wake){(void)wake;}
void tud_vendor_rx_cb(uint8_t interface,const uint8_t *buffer,uint32_t bytes)
{(void)buffer;(void)bytes;if(interface)stop();}
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{if(interface||bytes>dsi_response_sent-dsi_response_transferred)stop();dsi_response_transferred+=bytes;
 if(dsi_response_transferred==dsi_response_bytes&&dsi_response_sent==dsi_response_bytes){
  if(dsi_response[4]==2&&dsi_response[8]==1)atomic_store_explicit(&dsi.input.request,0,memory_order_release);
  dsi_response_bytes=0;}}
void tud_vendor_tx_complete_cb(uint8_t interface){(void)interface;}
static void dsi_exchange(void)
{
    if(dsi_response_bytes){if(dsi_response[8]==1&&!dsi_live())stop();unsigned n=tud_vendor_write(dsi_response+dsi_response_sent,dsi_response_bytes-dsi_response_sent);
        dsi_response_sent+=n;tud_vendor_flush();return;}
    if(!tud_vendor_available())return;
    uint8_t request;
    if(dsi.session.version==2){
     if(!atomic_load_explicit(&dsi.input.allocated,memory_order_acquire))return;
     if(dsi.input.header_used||tud_vendor_available()>1){
      uint8_t fragment[512];size_t count=dsi_input_remaining(&dsi.input);unsigned available=tud_vendor_available();
      if(count>sizeof(fragment))count=sizeof(fragment);if(count>available)count=available;
      if(!count||tud_vendor_read(fragment,count)!=count||dsi_input_feed(&dsi.input,&dsi.session,dsi_input_now,&dsi,fragment,count)<0){
       atomic_store_explicit(&dsi.failed,1,memory_order_release);atomic_store_explicit(&dsi.result,2,memory_order_release);}return;
     }
     if(tud_vendor_read(&request,1)!=1)stop();
     if(request){if(dsi_input_feed(&dsi.input,&dsi.session,dsi_input_now,&dsi,&request,1)<0){
       atomic_store_explicit(&dsi.failed,1,memory_order_release);atomic_store_explicit(&dsi.result,2,memory_order_release);}return;}
    }else if(tud_vendor_available()!=1||tud_vendor_read(&request,1)!=1||request)stop();
    unsigned result=atomic_load_explicit(&dsi.result,memory_order_acquire);
    if(result==1&&!dsi_live())result=2;
    if(dsi.session.version==2&&result!=2)result=atomic_load_explicit(&dsi.input.request,memory_order_acquire)==3?1:0;
    const uint8_t *payload=dsi.session.version==2?dsi.replay_result:dsi.observation;
    put32(dsi_response,0x31525344);put32(dsi_response+4,dsi.session.version==2?2:1);put32(dsi_response+8,result);
    put32(dsi_response+12,result==1?DSI_OBSERVATION_BYTES:0);
    put32(dsi_response+16,(uint32_t)board.session.epoch);put32(dsi_response+20,(uint32_t)(board.session.epoch>>32));
    put32(dsi_response+24,(uint32_t)board.session.generation);put32(dsi_response+28,(uint32_t)(board.session.generation>>32));
    uint8_t digest[32]={0};if(result==1)storage_digest_bytes(payload,256,digest);
    memcpy(dsi_response+32,dsi.profile_hash,32);if(result==1)memcpy(dsi_response+64,digest,32);else memset(dsi_response+64,0,32);
    if(result==1){memset(dsi_response+96,0,32);memcpy(dsi_response+128,payload,DSI_OBSERVATION_BYTES);}
    else memset(dsi_response+96,0,32);
    if(result==1&&!dsi_live()){result=2;put32(dsi_response+8,2);put32(dsi_response+12,0);memset(dsi_response+64,0,64);}
    dsi_response_sent=dsi_response_transferred=0;dsi_response_bytes=result==1?sizeof(dsi_response):128;
}
void board_usb0_task(void *unused)
{
    (void)unused;
    usb0_service_start();
    usb0_service_run(dsi_exchange);
}
