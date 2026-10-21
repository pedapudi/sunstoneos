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
 * Only status1 appends owned active Y/Cb/Cr pixels. */
static uint8_t jpeg_response[JPEG_RESULT_HEADER+JPEG_OUTPUT_CAPACITY];
static unsigned jpeg_response_bytes,jpeg_response_sent,jpeg_response_transferred;
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
void tud_mount_cb(void){}
void tud_umount_cb(void){}
void tud_suspend_cb(bool wake){(void)wake;}
void tud_vendor_rx_cb(uint8_t interface,const uint8_t *buffer,uint32_t bytes)
{(void)buffer;(void)bytes;if(interface)stop();}
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{if(interface||bytes>jpeg_response_sent-jpeg_response_transferred)stop();jpeg_response_transferred+=bytes;
 if(jpeg_response_transferred==jpeg_response_bytes&&jpeg_response_sent==jpeg_response_bytes)jpeg_response_bytes=0;}
void tud_vendor_tx_complete_cb(uint8_t interface){(void)interface;}
static int upload_now(void *unused,uint64_t *now)
{(void)unused;return board_live(NULL)&&a733_counter_clock_now(&providers.shared.counter,now)==1&&board_live(NULL)?1:-1;}
static void jpeg_exchange(void)
{
    if(jpeg_response_bytes){unsigned n=tud_vendor_write(jpeg_response+jpeg_response_sent,jpeg_response_bytes-jpeg_response_sent);
        jpeg_response_sent+=n;tud_vendor_flush();return;}
    if(!tud_vendor_available())return;
    uint8_t incoming[512];size_t n;
    if(jpeg.supplied.header_used&&!jpeg_input_ready(&jpeg.supplied)){
        n=jpeg_input_remaining(&jpeg.supplied);if(!n)stop();
        if(n>sizeof(incoming))n=sizeof(incoming);
        if(n>tud_vendor_available())n=tud_vendor_available();
    }else n=1;
    if(tud_vendor_read(incoming,n)!=n)stop();
    if(n!=1||incoming[0]||(jpeg.supplied.header_used&&!jpeg_input_ready(&jpeg.supplied))){
        if(!board_live(NULL)||jpeg_input_feed(&jpeg.supplied,&jpeg.session,upload_now,NULL,
           board.session.deadline_us-10000,incoming,n)<0||!board_live(NULL))stop();
        return;
    }
    unsigned result=atomic_load_explicit(&jpeg.result,memory_order_acquire);
    put32(jpeg_response,0x4745504a);put32(jpeg_response+4,2);put32(jpeg_response+8,result);
    put32(jpeg_response+12,result==1?jpeg.supplied.output_bytes:0);
    put32(jpeg_response+16,(uint32_t)board.session.epoch);put32(jpeg_response+20,(uint32_t)(board.session.epoch>>32));
    put32(jpeg_response+24,(uint32_t)board.session.generation);put32(jpeg_response+28,(uint32_t)(board.session.generation>>32));
    memcpy(jpeg_response+32,jpeg.session.fixture,32);memcpy(jpeg_response+64,jpeg.session.reference,32);
    put32(jpeg_response+128,jpeg.supplied.width);put32(jpeg_response+132,jpeg.supplied.height);
    put32(jpeg_response+136,jpeg.supplied.stride);put32(jpeg_response+140,jpeg.supplied.rows);
    if(result==1){memcpy(jpeg_response+96,jpeg.digest,32);memcpy(jpeg_response+JPEG_RESULT_HEADER,jpeg.payload,jpeg.supplied.output_bytes);}
    else memset(jpeg_response+96,0,32);
    jpeg_response_sent=jpeg_response_transferred=0;jpeg_response_bytes=JPEG_RESULT_HEADER+(result==1?jpeg.supplied.output_bytes:0);
}
void board_usb0_task(void *unused)
{
    (void)unused;
    usb0_service_start();
    usb0_service_run(jpeg_exchange);
}
