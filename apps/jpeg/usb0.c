#include "jpeg.h"
#include "boot.h"
#include "platform.h"
#include "tusb.h"
#include "device/dcd.h"
#include <string.h>
#ifdef SUNSTONE_BOARD_SUPERVISED
#define USB0_PID 0xefe8
#else
#define USB0_PID 0xefea
#endif
static _Noreturn void stop(void){board_fail(NULL);for(;;)vTaskDelay(1);}
#define USB0_PRODUCT "sunstoneos native JPEG"
#define USB0_SERIAL "sunstoneos-a733-jpeg"
#define USB0_LIVE() board_live(NULL)
#define USB0_PROVIDERS (&providers.shared)
#include "../board/usb0_service.h"

/* One upload per lifetime, then zero-byte queries. Version2 header144bytes
 * adds actual width, height, padded luma stride and padded rows after hashes.
 * Only status1 appends owned active Y/Cb/Cr pixels. A sequence session uses
 * version 3 with a 160-byte header that adds the image index, image count
 * and published count, and carries the current image's digests. */
static uint8_t jpeg_response[JPEG_SEQUENCE_RESULT_HEADER+JPEG_OUTPUT_CAPACITY];
static unsigned jpeg_response_bytes,jpeg_response_sent,jpeg_response_transferred;
/* Published count of the sequence response being sent, when it carries pixels. */
static unsigned jpeg_response_delivers;
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
void tud_mount_cb(void){}
void tud_umount_cb(void){}
void tud_suspend_cb(bool wake){(void)wake;}
void tud_vendor_rx_cb(uint8_t interface,const uint8_t *buffer,uint32_t bytes)
{(void)buffer;(void)bytes;if(interface)stop();}
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{if(interface||bytes>jpeg_response_sent-jpeg_response_transferred)stop();jpeg_response_transferred+=bytes;
 if(jpeg_response_transferred==jpeg_response_bytes&&jpeg_response_sent==jpeg_response_bytes){
  /* A fully transferred success response releases the next sequence image. */
  if(jpeg_response_delivers>jpeg.supplied.delivered)jpeg.supplied.delivered=jpeg_response_delivers;
  jpeg_response_bytes=0;}}
void tud_vendor_tx_complete_cb(uint8_t interface){(void)interface;}
static int upload_now(void *unused,uint64_t *now)
{(void)unused;return board_live(NULL)&&a733_counter_clock_now(&providers.shared.counter,now)==1&&board_live(NULL)?1:-1;}
static void jpeg_exchange(void)
{
    if(jpeg_response_bytes){unsigned n=tud_vendor_write(jpeg_response+jpeg_response_sent,jpeg_response_bytes-jpeg_response_sent);
        jpeg_response_sent+=n;tud_vendor_flush();return;}
    if(!tud_vendor_available())return;
    uint8_t incoming[512];size_t n=jpeg_input_partial(&jpeg.supplied),partial=n;
    if(n){
        if(n>sizeof(incoming))n=sizeof(incoming);
        if(n>tud_vendor_available())n=tud_vendor_available();
    }else n=1;
    if(tud_vendor_read(incoming,n)!=n)stop();
    if(partial||incoming[0]){
        if(!board_live(NULL))stop();
        int fed=jpeg_input_feed(&jpeg.supplied,&jpeg.session,upload_now,NULL,board.session.deadline_us-10000,incoming,n);
        unsigned expected=1;
        /* Image k>0 replaces the delivered success of image k-1 with pending;
         * a failure recorded meanwhile is never overwritten. */
        if(fed<0||(fed==1&&jpeg_input_ready(&jpeg.supplied)>1&&!atomic_compare_exchange_strong_explicit(
           &jpeg.result,&expected,0,memory_order_acq_rel,memory_order_acquire))||!board_live(NULL))stop();
        return;
    }
    unsigned result=atomic_load_explicit(&jpeg.result,memory_order_acquire),sequence=jpeg.supplied.sequence;
    unsigned header=sequence?JPEG_SEQUENCE_RESULT_HEADER:JPEG_RESULT_HEADER,published=jpeg_input_ready(&jpeg.supplied);
    memset(jpeg_response,0,header);
    put32(jpeg_response,0x4745504a);put32(jpeg_response+4,sequence?3:2);put32(jpeg_response+8,result);
    put32(jpeg_response+12,result==1?jpeg.supplied.output_bytes:0);
    put32(jpeg_response+16,(uint32_t)board.session.epoch);put32(jpeg_response+20,(uint32_t)(board.session.epoch>>32));
    put32(jpeg_response+24,(uint32_t)board.session.generation);put32(jpeg_response+28,(uint32_t)(board.session.generation>>32));
    if(!sequence){memcpy(jpeg_response+32,jpeg.session.fixture,32);memcpy(jpeg_response+64,jpeg.session.reference,32);}
    else if(published){
        /* The current image's digests and geometry stay fixed until the
         * next image is accepted, which needs this result delivered first. */
        memcpy(jpeg_response+32,jpeg.supplied.input_hash,32);memcpy(jpeg_response+64,jpeg.supplied.oracle_hash,32);
        put32(jpeg_response+144,published-1);
    }
    if(sequence){put32(jpeg_response+148,jpeg.supplied.count);put32(jpeg_response+152,published);}
    if(!sequence||published){
        put32(jpeg_response+128,jpeg.supplied.width);put32(jpeg_response+132,jpeg.supplied.height);
        put32(jpeg_response+136,jpeg.supplied.stride);put32(jpeg_response+140,jpeg.supplied.rows);
    }
    if(result==1){memcpy(jpeg_response+96,jpeg.digest,32);memcpy(jpeg_response+header,jpeg.payload,jpeg.supplied.output_bytes);}
    jpeg_response_delivers=sequence&&result==1?published:0;
    jpeg_response_sent=jpeg_response_transferred=0;jpeg_response_bytes=header+(result==1?jpeg.supplied.output_bytes:0);
}
void board_usb0_task(void *unused)
{
    (void)unused;
    usb0_service_start();
    usb0_service_run(jpeg_exchange);
}
