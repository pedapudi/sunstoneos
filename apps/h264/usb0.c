#include "h264.h"
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
#define USB0_PRODUCT "sunstoneos native H.264"
#define USB0_SERIAL "sunstoneos-a733-h264"
#define USB0_LIVE() board_live(NULL)
#define USB0_PROVIDERS (&providers.shared)
#include "../board/usb0_service.h"

/* Zero followed by a LE32 picture index requests a coherent private result.
 * Header160bytes: LE32 magic/version/status/bytes, LE64 epoch/generation,
 * fixture/reference/copied-payload SHA256, then index/count/frame_num/POC/
 * width/height/slice_type/terminal. Reference hash0 means no qualification oracle.
 * Only status1 appends2304 owned copied Y/Cb/Cr bytes.
 * Device buffers remain DEVICE-owned; status2 carries no payload. */
static uint8_t h264_response[160+A733_H264_OUTPUT_BYTES],h264_query[5];
static unsigned h264_query_used;
static unsigned h264_response_bytes,h264_response_sent,h264_response_transferred;
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
void tud_mount_cb(void){}
void tud_umount_cb(void){}
void tud_suspend_cb(bool wake){(void)wake;}
void tud_vendor_rx_cb(uint8_t interface,const uint8_t *buffer,uint32_t bytes)
{(void)buffer;(void)bytes;if(interface)stop();}
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{if(h264.session.version==H264_STREAM_SESSION_VERSION){if(interface)stop();h264_stream_tx_complete(bytes);return;}
 if(interface||bytes>h264_response_sent-h264_response_transferred)stop();h264_response_transferred+=bytes;
 if(h264_response_transferred==h264_response_bytes&&h264_response_sent==h264_response_bytes)h264_response_bytes=0;}
void tud_vendor_tx_complete_cb(uint8_t interface){(void)interface;}
static int upload_now(void *unused,uint64_t *now)
{(void)unused;return board_live(NULL)&&a733_counter_clock_now(&providers.shared.counter,now)==1&&board_live(NULL)?1:-1;}
static void h264_exchange(void)
{
    if(h264.session.version==H264_STREAM_SESSION_VERSION){h264_stream_usb();return;}
    if(h264_response_bytes){unsigned n=tud_vendor_write(h264_response+h264_response_sent,h264_response_bytes-h264_response_sent);
        h264_response_sent+=n;tud_vendor_flush();return;}
    if(!tud_vendor_available())return;
    uint8_t incoming[512];
    size_t n;
    if(h264_query_used){
        n=5-h264_query_used;if(n>tud_vendor_available())n=tud_vendor_available();
    }else if(h264.supplied.header_used&&!a733_h264_input_ready(&h264.supplied)){
        n=a733_h264_input_remaining(&h264.supplied);
        if(!n)stop();
        if(n>sizeof(incoming))n=sizeof(incoming);
        if(n>tud_vendor_available())n=tud_vendor_available();
    }else n=1;
    if(tud_vendor_read(incoming,n)!=n)stop();
    if(h264_query_used){
        memcpy(h264_query+h264_query_used,incoming,n);h264_query_used+=n;
        if(h264_query_used!=5)return;
    }else if(n!=1||incoming[0]||
       (h264.supplied.header_used&&!a733_h264_input_ready(&h264.supplied))){
        if(!board_live(NULL)||
           a733_h264_input_feed(&h264.supplied,&h264.session,upload_now,NULL,
               board.session.deadline_us-10000,incoming,n)<0||!board_live(NULL))stop();
        return;
    }else{h264_query[0]=0;h264_query_used=1;return;}
    unsigned picture=(unsigned)h264_query[1]|((unsigned)h264_query[2]<<8)|
        ((unsigned)h264_query[3]<<16)|((unsigned)h264_query[4]<<24);
    h264_query_used=0;
    unsigned count=a733_h264_input_ready(&h264.supplied)?h264.supplied.sequence.count:0;
    if(picture>=A733_H264_PICTURES||(count&&picture>=count))stop();
    unsigned terminal=atomic_load_explicit(&h264.result,memory_order_acquire);
    unsigned completed=atomic_load_explicit(&h264.completed,memory_order_acquire);
    unsigned result=picture<completed?1:terminal==2?2:0;
    put32(h264_response,0x34363248);put32(h264_response+4,2);put32(h264_response+8,result);
    put32(h264_response+12,result==1?A733_H264_OUTPUT_BYTES:0);
    put32(h264_response+16,(uint32_t)board.session.epoch);put32(h264_response+20,(uint32_t)(board.session.epoch>>32));
    put32(h264_response+24,(uint32_t)board.session.generation);put32(h264_response+28,(uint32_t)(board.session.generation>>32));
    memcpy(h264_response+32,h264.session.fixture,32);memcpy(h264_response+64,h264.session.reference,32);
    if(result==1){memcpy(h264_response+96,h264.digest[picture],32);memcpy(h264_response+160,h264.payload[picture],A733_H264_OUTPUT_BYTES);}
    else memset(h264_response+96,0,32);
    put32(h264_response+128,picture);put32(h264_response+132,count);
    const struct a733_h264_picture *p=count?&h264.supplied.sequence.pictures[picture]:NULL;
    put32(h264_response+136,p?p->frame_num:0);put32(h264_response+140,p?p->poc:0);
    put32(h264_response+144,p?p->width:0);put32(h264_response+148,p?p->height:0);
    put32(h264_response+152,p?p->slice_type:0);put32(h264_response+156,terminal);
    h264_response_sent=h264_response_transferred=0;h264_response_bytes=result==1?sizeof(h264_response):160;
}
void board_usb0_task(void *unused)
{
    (void)unused;
    usb0_service_start();
    usb0_service_run(h264_exchange);
}
