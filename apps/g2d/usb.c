#include "g2d.h"
#include "boot.h"
#include "platform.h"
#include "tusb.h"
#include "device/dcd.h"
#include <string.h>
#define USB0_PID 0xefe8
static _Noreturn void stop(void){board_fail(NULL);for(;;)vTaskDelay(1);}
#define USB0_PRODUCT "sunstoneos native G2D"
#define USB0_SERIAL "sunstoneos-a733-g2d"
#define USB0_LIVE() board_live(NULL)
#define USB0_PROVIDERS (&providers.shared)
#include "../board/usb0_service.h"

/* Header-only queries until committed upload completes. Output bytes are sent
 * directly from the private immutable copy, with bounded TinyUSB FIFO writes.
 * ACK is accepted only after actual transfer completion and binds generation
 * plus copied digest. It never releases device or CPU buffers. */
static uint8_t response[G2D_RESULT_HEADER],ack[40];
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
    if(response_bytes>G2D_RESULT_HEADER)delivered=1;
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
      size_t header=sent<G2D_RESULT_HEADER?G2D_RESULT_HEADER-sent:0;
      if(header>n)header=n;
      if(header)memcpy(chunk,response+sent,header);
      if(n>header)memcpy(chunk+header,g2d.payload+sent+header-G2D_RESULT_HEADER,n-header);
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
      if(le64(ack)!=g2d.session.board.generation||memcmp(ack+8,g2d.digest,32))stop();
      acked=1;acking=0;return;}
    if(g2d.supplied.header_used&&!g2d_input_ready(&g2d.supplied)){
      n=g2d_input_remaining(&g2d.supplied);if(!n)stop();if(n>512)n=512;
      if(n>tud_vendor_available())n=tud_vendor_available();
    }
    if(tud_vendor_read(incoming,n)!=n)stop();
    if(n!=1||incoming[0]>1||(g2d.supplied.header_used&&!g2d_input_ready(&g2d.supplied))){
      if(!board_live(NULL)||g2d_input_feed(&g2d.supplied,&g2d.session,upload_now,NULL,
         board.session.deadline_us-10000,incoming,n)<0||!board_live(NULL))stop();return;
    }
    unsigned result=atomic_load_explicit(&g2d.result,memory_order_acquire);
    if(incoming[0]==1){if(result!=1||!delivered||acked)stop();acking=1;return;}
    memset(response,0,sizeof(response));put32(response,0x44324753);put32(response+4,1);
    put32(response+8,acked?3:result);put32(response+12,result==1&&!acked?(uint32_t)g2d.bytes:0);
    put32(response+16,(uint32_t)board.session.epoch);put32(response+20,(uint32_t)(board.session.epoch>>32));
    put32(response+24,(uint32_t)board.session.generation);put32(response+28,(uint32_t)(board.session.generation>>32));
    memcpy(response+32,g2d.session.fixture,32);
    memcpy(response+64,g2d.session_digest,32);
    if(result==1)memcpy(response+96,g2d.digest,32);
    const uint32_t geometry[]={g2d.session.width,g2d.session.height,g2d.session.stride,
      g2d.session.format,g2d.session.op,g2d.session.dx,g2d.session.dy,g2d.session.w,
      g2d.session.h,g2d.session.sx,g2d.session.sy,g2d.session.color};
    for(unsigned i=0;i<12;++i)put32(response+128+4*i,geometry[i]);
    if(result){put32(response+176,g2d.complete);put32(response+180,g2d.rcq_status);
      put32(response+184,g2d.iommu.fault.scanned_masters);put32(response+188,g2d.guards_ok);}
    sent=transferred=0;terminal_zlp=0;response_bytes=G2D_RESULT_HEADER+(result==1&&!acked?g2d.bytes:0);
}
void board_usb0_task(void *unused)
{
    (void)unused;
    usb0_service_start();
    usb0_service_run(exchange);
}
