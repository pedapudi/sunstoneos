#include "encode.h"
#include "boot.h"
#include "platform.h"
#include "tusb.h"
#include "device/dcd.h"
#include <string.h>
#define USB0_PID 0xeff1
static _Noreturn void stop(void){board_fail(NULL);for(;;)vTaskDelay(1);}
#define USB0_PRODUCT "sunstoneos native JPEG encoder"
#define USB0_SERIAL "sunstoneos-a733-jpeg-encode"
#define USB0_LIVE() board_live(NULL)
#define USB0_PROVIDERS (&providers.shared)
#include "../board/usb0_service.h"

/* Header-only queries until committed upload completes. Output bytes are sent
 * directly from the private immutable copy, with bounded TinyUSB FIFO writes.
 * ACK is accepted only after actual transfer completion and binds generation
 * plus copied digest. It never releases device or CPU buffers. */
static uint8_t response[JPEG_ENCODE_RESULT_HEADER],ack[40];
static size_t response_bytes,sent,transferred,ack_used;
static unsigned delivered,acked,acking;
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static uint64_t le64(const uint8_t *p)
{uint64_t v=0;for(unsigned i=0;i<8;++i)v|=(uint64_t)p[i]<<(8*i);return v;}
void tud_mount_cb(void){}
void tud_umount_cb(void){}
void tud_suspend_cb(bool wake){(void)wake;}
void tud_vendor_rx_cb(uint8_t interface,const uint8_t *buffer,uint32_t bytes)
{(void)buffer;(void)bytes;if(interface)stop();}
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{
    if(interface||bytes>sent-transferred)stop();transferred+=bytes;
    if(transferred==response_bytes&&sent==response_bytes){
      if(response_bytes>JPEG_ENCODE_RESULT_HEADER)delivered=1;response_bytes=0;
    }
}
void tud_vendor_tx_complete_cb(uint8_t interface){if(interface)stop();}
static int upload_now(void *unused,uint64_t *now)
{(void)unused;return board_live(NULL)&&a733_counter_clock_now(&providers.shared.counter,now)==1&&board_live(NULL)?1:-1;}
static void exchange(void)
{
    if(response_bytes){
      size_t left;const uint8_t *p;
      if(sent<JPEG_ENCODE_RESULT_HEADER){p=response+sent;left=JPEG_ENCODE_RESULT_HEADER-sent;}
      else{p=jpeg_encode.payload+sent-JPEG_ENCODE_RESULT_HEADER;left=response_bytes-sent;}
      if(left>512)left=512;unsigned n=tud_vendor_write(p,left);if(n>left)stop();
      sent+=n;tud_vendor_flush();return;
    }
    if(!tud_vendor_available())return;
    uint8_t incoming[512];size_t n=1;
    if(acking){n=sizeof(ack)-ack_used;if(n>tud_vendor_available())n=tud_vendor_available();
      if(tud_vendor_read(ack+ack_used,n)!=n)stop();ack_used+=n;if(ack_used!=sizeof(ack))return;
      if(le64(ack)!=jpeg_encode.session.board.generation||memcmp(ack+8,jpeg_encode.digest,32))stop();
      acked=1;acking=0;return;}
    if(jpeg_encode.supplied.header_used&&!jpeg_encode_input_ready(&jpeg_encode.supplied)){
      n=jpeg_encode_input_remaining(&jpeg_encode.supplied);if(!n)stop();if(n>512)n=512;
      if(n>tud_vendor_available())n=tud_vendor_available();
    }
    if(tud_vendor_read(incoming,n)!=n)stop();
    if(n!=1||incoming[0]>1||(jpeg_encode.supplied.header_used&&!jpeg_encode_input_ready(&jpeg_encode.supplied))){
      if(!board_live(NULL)||jpeg_encode_input_feed(&jpeg_encode.supplied,&jpeg_encode.session,upload_now,NULL,
         board.session.deadline_us-10000,incoming,n)<0||!board_live(NULL))stop();return;
    }
    unsigned result=atomic_load_explicit(&jpeg_encode.result,memory_order_acquire);
    if(incoming[0]==1){if(result!=1||!delivered||acked)stop();acking=1;return;}
    memset(response,0,sizeof(response));put32(response,0x4547504a);put32(response+4,1);
    put32(response+8,acked?3:result);put32(response+12,result==1&&!acked?(uint32_t)jpeg_encode.jpeg_bytes:0);
    put32(response+16,(uint32_t)board.session.epoch);put32(response+20,(uint32_t)(board.session.epoch>>32));
    put32(response+24,(uint32_t)board.session.generation);put32(response+28,(uint32_t)(board.session.generation>>32));
    memcpy(response+32,jpeg_encode.session.fixture,32);put32(response+64,1); /* NV12 */
    put32(response+128,640);put32(response+132,480);put32(response+136,640);put32(response+140,75);
    if(result==1)memcpy(response+96,jpeg_encode.digest,32);
    sent=transferred=0;response_bytes=JPEG_ENCODE_RESULT_HEADER+(result==1&&!acked?jpeg_encode.jpeg_bytes:0);
}
void board_usb0_task(void *unused)
{
    (void)unused;
    usb0_service_start();
    usb0_service_run(exchange);
}
