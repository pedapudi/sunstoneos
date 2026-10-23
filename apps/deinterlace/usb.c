#include "deinterlace.h"
#include "boot.h"
#include "platform.h"
#include "tusb.h"
#include "device/dcd.h"
#include <string.h>
#define USB0_PID 0xefe8
static _Noreturn void stop(void){board_fail(NULL);for(;;)vTaskDelay(1);}
#define USB0_PRODUCT "sunstoneos native DEINTERLACE"
#define USB0_SERIAL "sunstoneos-a733-deinterlace"
#define USB0_LIVE() board_live(NULL)
#define USB0_PROVIDERS (&providers.shared)
#include "../board/usb0_service.h"

/* Header-only queries until committed upload completes. Output bytes are sent
 * directly from the private immutable copy, with bounded TinyUSB FIFO writes;
 * a sequence sends every job's output in job order. ACK is accepted only after
 * actual transfer completion and binds generation plus copied digest. It never
 * releases device or CPU buffers. */
static uint8_t response[DEINTERLACE_RESULT_HEADER],ack[40];
static size_t response_bytes,sent,transferred,ack_used;
static unsigned delivered,acked,acking,terminal_zlp;
static void queue_response(void);
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static uint64_t le64(const uint8_t *p)
{uint64_t v=0;for(unsigned i=0;i<8;++i)v|=(uint64_t)p[i]<<(8*i);return v;}
void tud_mount_cb(void){}
void tud_umount_cb(void){}
void tud_suspend_cb(bool wake){(void)wake;}
void tud_vendor_rx_cb(uint8_t interface,const uint8_t *buffer,uint32_t bytes)
{(void)buffer;(void)bytes;if(interface)stop();}
static void response_complete(void)
{
    if(response_bytes>DEINTERLACE_RESULT_HEADER)delivered=1;
    response_bytes=0;terminal_zlp=0;
}
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{
    if(interface||!response_bytes||bytes>sent-transferred)stop();
    if(!bytes){
      if(!terminal_zlp||transferred!=response_bytes||sent!=response_bytes)stop();
      response_complete();return;
    }
    if(terminal_zlp)stop();transferred+=bytes;
    if(transferred==response_bytes&&sent==response_bytes){
      unsigned packet=tud_speed_get()==TUSB_SPEED_HIGH?512:64;
      if(bytes%packet==0)terminal_zlp=1;else response_complete();
      return;
    }
    /* The pinned vendor callback runs before its next FIFO flush/ZLP decision.
     * Refill here so an empty FIFO cannot terminate an intermediate packet. */
    queue_response();
}
void tud_vendor_tx_complete_cb(uint8_t interface){if(interface)stop();}
static int upload_now(void *unused,uint64_t *now)
{(void)unused;return board_live(NULL)&&a733_counter_clock_now(&providers.shared.counter,now)==1&&board_live(NULL)?1:-1;}
static void queue_response(void)
{
    uint8_t chunk[512];unsigned budget=4096;
    while(sent<response_bytes&&budget){
      size_t n=response_bytes-sent;if(n>sizeof(chunk))n=sizeof(chunk);
      size_t header=sent<DEINTERLACE_RESULT_HEADER?DEINTERLACE_RESULT_HEADER-sent:0;
      if(header>n)header=n;
      if(header)memcpy(chunk,response+sent,header);
      if(n>header)memcpy(chunk+header,deinterlace.payload+sent+header-DEINTERLACE_RESULT_HEADER,n-header);
      unsigned accepted=tud_vendor_write(chunk,n);if(accepted>n)stop();
      sent+=accepted;budget-=accepted;
      if(accepted<n)return;
    }
}
static void exchange(void)
{
    if(response_bytes){queue_response();tud_vendor_flush();return;}
    if(!tud_vendor_available())return;
    uint8_t incoming[512];size_t n=1;
    if(acking){n=sizeof(ack)-ack_used;if(n>tud_vendor_available())n=tud_vendor_available();
      if(tud_vendor_read(ack+ack_used,n)!=n)stop();ack_used+=n;if(ack_used!=sizeof(ack))return;
      if(le64(ack)!=deinterlace.session.board.generation||memcmp(ack+8,deinterlace.digest,32))stop();
      acked=1;acking=0;return;}
    if(deinterlace.supplied.header_used&&!deinterlace_input_ready(&deinterlace.supplied)){
      n=deinterlace_input_remaining(&deinterlace.supplied);if(!n)stop();if(n>512)n=512;
      if(n>tud_vendor_available())n=tud_vendor_available();
    }
    if(tud_vendor_read(incoming,n)!=n)stop();
    if(n!=1||incoming[0]>1||(deinterlace.supplied.header_used&&!deinterlace_input_ready(&deinterlace.supplied))){
      if(!board_live(NULL)||deinterlace_input_feed(&deinterlace.supplied,&deinterlace.session,upload_now,NULL,
         board.session.deadline_us-10000,incoming,n)<0||!board_live(NULL))stop();return;
    }
    unsigned result=atomic_load_explicit(&deinterlace.result,memory_order_acquire);
    if(incoming[0]==1){if(result!=1||!delivered||acked)stop();acking=1;return;}
    memset(response,0,sizeof(response));put32(response,0x52494453);put32(response+4,1);
    size_t outputs=a733_deinterlace_jobs(&deinterlace.session)*deinterlace.bytes;
    put32(response+8,acked?3:result);put32(response+12,result==1&&!acked?(uint32_t)outputs:0);
    put32(response+16,(uint32_t)board.session.epoch);put32(response+20,(uint32_t)(board.session.epoch>>32));
    put32(response+24,(uint32_t)board.session.generation);put32(response+28,(uint32_t)(board.session.generation>>32));
    memcpy(response+32,deinterlace.session.fixture,32);
    memcpy(response+64,deinterlace.session_digest,32);
    if(result==1)memcpy(response+96,deinterlace.digest,32);
    const uint32_t geometry[]={deinterlace.session.width,deinterlace.session.height,
      deinterlace.session.y_stride,deinterlace.session.uv_stride,deinterlace.session.format,
      deinterlace.session.mode,deinterlace.session.top_field_first,
      deinterlace.session.strength|(uint32_t)deinterlace.session.repeats<<16};
    for(unsigned i=0;i<8;++i)put32(response+128+4*i,geometry[i]);
    if(result){put32(response+160,deinterlace.complete);put32(response+164,deinterlace.status);
      put32(response+168,deinterlace.iommu.fault.scanned_masters);put32(response+172,deinterlace.guards_ok);
      /* A sequence reports its finished jobs and each job's TNR strength. */
      if(deinterlace.session.repeats){put32(response+176,deinterlace.job+deinterlace.complete);
        if(deinterlace.session.mode==A733_DEINTERLACE_TNR)memcpy(response+180,deinterlace.strengths,sizeof(deinterlace.strengths));}}
    sent=transferred=0;terminal_zlp=0;response_bytes=DEINTERLACE_RESULT_HEADER+(result==1&&!acked?outputs:0);
}
void board_usb0_task(void *unused)
{
    (void)unused;
    usb0_service_start();
    usb0_service_run(exchange);
}
