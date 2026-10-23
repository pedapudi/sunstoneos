#include "hevc.h"
#include "boot.h"
#include "platform.h"
#include "tusb.h"
#include "device/dcd.h"
#include <string.h>
#define USB0_PID 0xefe8
static _Noreturn void stop(void){board_fail(NULL);for(;;)vTaskDelay(1);}
#define USB0_PRODUCT "sunstoneos native HEVC"
#define USB0_SERIAL "sunstoneos-a733-hevc"
#define USB0_LIVE() board_live(NULL)
#define USB0_PROVIDERS (&providers.shared)
#include "../board/usb0_service.h"

/* One upload per lifetime: the 32-byte header of apps/hevc/prepare.py, then
 * the clip. Afterwards the host sends commands that start with one byte:
 *   0, ordinal                       query picture 0 or 1
 *   1, LE32 ordinal, LE64 generation, copied SHA-256   acknowledge a copy
 * A query returns a 160-byte header, LE32 unless noted: magic D625,
 * version 1, status (0 pending, 1 copied, 2 failed), payload bytes, LE64
 * epoch and generation, clip SHA-256, copied SHA-256, then ordinal, picture
 * count, POC, width, height, luma stride, chroma stride, chroma offset,
 * format 6 (NV12), slice type and terminal failure, and 20 zero bytes.
 * Only status 1 appends the 460800 copied NV12 bytes, read in place from the
 * immutable copy. Acknowledgments arrive in order and must name a copied
 * picture of this session with its digest; anything else stops the board. */
#define HEVC_ACK_BYTES 45u
static uint8_t hevc_header[HEVC_RESULT_HEADER],hevc_command[HEVC_ACK_BYTES];
static const uint8_t *hevc_payload;
static unsigned hevc_command_used,hevc_command_bytes;
static unsigned hevc_response_bytes,hevc_response_sent,hevc_response_transferred;
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static void put64(uint8_t *p,uint64_t n){put32(p,(uint32_t)n);put32(p+4,(uint32_t)(n>>32));}
static uint64_t get64(const uint8_t *p){uint64_t n=0;for(unsigned i=8;i--;)n=(n<<8)|p[i];return n;}
void tud_mount_cb(void){}
void tud_umount_cb(void){}
void tud_suspend_cb(bool wake){(void)wake;}
void tud_vendor_rx_cb(uint8_t interface,const uint8_t *buffer,uint32_t bytes)
{(void)buffer;(void)bytes;if(interface)stop();}
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{if(interface||bytes>hevc_response_sent-hevc_response_transferred)stop();hevc_response_transferred+=bytes;
 if(hevc_response_transferred==hevc_response_bytes&&hevc_response_sent==hevc_response_bytes)hevc_response_bytes=0;}
void tud_vendor_tx_complete_cb(uint8_t interface){(void)interface;}
static int upload_now(void *unused,uint64_t *now)
{(void)unused;return board_live(NULL)&&a733_counter_clock_now(&providers.shared.counter,now)==1&&board_live(NULL)?1:-1;}
static void hevc_query(unsigned picture)
{
    if(picture>=A733_HEVC_PICTURES)stop();
    /* A failure observed here already covers every earlier publication. */
    unsigned failed=atomic_load_explicit(&hevc.result,memory_order_acquire)==2;
    unsigned completed=atomic_load_explicit(&hevc.completed,memory_order_acquire);
    unsigned status=picture<completed?1:failed?2:0;
    memset(hevc_header,0,sizeof(hevc_header));
    put32(hevc_header,HEVC_RESULT_MAGIC);put32(hevc_header+4,1);put32(hevc_header+8,status);
    put32(hevc_header+12,status==1?A733_HEVC_PICTURE_BYTES:0);
    put64(hevc_header+16,hevc.session.board.epoch);put64(hevc_header+24,hevc.session.board.generation);
    memcpy(hevc_header+32,hevc.session.input_digest,32);
    if(status==1)memcpy(hevc_header+64,hevc.digest[picture],32);
    const uint32_t fields[]={picture,A733_HEVC_PICTURES,
        hevc_input_ready(&hevc.supplied)?hevc.supplied.plan.pictures[picture].poc:0,
        640,480,640,640,A733_HEVC_CHROMA_OFFSET,6,picture?1:2,failed};
    for(unsigned i=0;i<sizeof(fields)/sizeof(*fields);++i)put32(hevc_header+96+4*i,fields[i]);
    hevc_payload=status==1?hevc.storage.copies[picture]:NULL;
    hevc_response_sent=hevc_response_transferred=0;
    hevc_response_bytes=HEVC_RESULT_HEADER+(status==1?A733_HEVC_PICTURE_BYTES:0);
}
static void hevc_ack(void)
{
    unsigned picture=(unsigned)hevc_command[1]|((unsigned)hevc_command[2]<<8)|
        ((unsigned)hevc_command[3]<<16)|((unsigned)hevc_command[4]<<24);
    unsigned acknowledged=atomic_load_explicit(&hevc.acknowledged,memory_order_acquire);
    if(picture!=acknowledged||picture>=atomic_load_explicit(&hevc.completed,memory_order_acquire)||
       get64(hevc_command+5)!=hevc.session.board.generation||memcmp(hevc_command+13,hevc.digest[picture],32)||
       !board_live(NULL))stop();
    atomic_store_explicit(&hevc.acknowledged,picture+1,memory_order_release);
}
static void hevc_exchange(void)
{
    if(hevc_response_bytes){
        unsigned at=hevc_response_sent,n=hevc_response_bytes-at;
        const uint8_t *from=at<HEVC_RESULT_HEADER?hevc_header+at:hevc_payload+(at-HEVC_RESULT_HEADER);
        if(at<HEVC_RESULT_HEADER&&n>HEVC_RESULT_HEADER-at)n=HEVC_RESULT_HEADER-at;
        hevc_response_sent+=tud_vendor_write(from,n);tud_vendor_flush();return;
    }
    if(!tud_vendor_available())return;
    uint8_t incoming[512];size_t n;
    int uploading=hevc.supplied.header_used&&!hevc_input_ready(&hevc.supplied);
    if(hevc_command_used)n=hevc_command_bytes-hevc_command_used;
    else if(uploading){n=hevc_input_remaining(&hevc.supplied);if(!n)stop();if(n>sizeof(incoming))n=sizeof(incoming);}
    else n=1;
    if(n>tud_vendor_available())n=tud_vendor_available();
    if(tud_vendor_read(incoming,n)!=n)stop();
    if(hevc_command_used){
        memcpy(hevc_command+hevc_command_used,incoming,n);hevc_command_used+=n;
        if(hevc_command_used<hevc_command_bytes)return;
        hevc_command_used=0;
        if(hevc_command[0])hevc_ack();else hevc_query(hevc_command[1]);
        return;
    }
    if(uploading||(incoming[0]>1&&!hevc_input_ready(&hevc.supplied))){
        if(!board_live(NULL)||hevc_input_feed(&hevc.supplied,&hevc.session,upload_now,NULL,
           board.session.deadline_us-10000,incoming,n)<0||!board_live(NULL))stop();
        return;
    }
    if(incoming[0]>1)stop();
    hevc_command[0]=incoming[0];hevc_command_used=1;hevc_command_bytes=incoming[0]?HEVC_ACK_BYTES:2;
}
void board_usb0_task(void *unused)
{
    (void)unused;
    usb0_service_start();
    usb0_service_run(hevc_exchange);
}
