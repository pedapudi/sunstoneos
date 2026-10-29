#include "application.h"
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
/* Board parks only USB0 and leaves its other services running. An image that
 * reuses this owner may define BOARD_USB0_STOP() to add its own revocation. */
#ifdef BOARD_USB0_STOP
static _Noreturn void stop(void){BOARD_USB0_STOP();for(;;)vTaskDelay(1);}
#else
static _Noreturn void stop(void){for(;;)vTaskDelay(1);}
#endif
#define USB0_PRODUCT "sunstoneos native board"
#define USB0_SERIAL "sunstoneos-a733-board"
#define USB0_LIVE() board_live(NULL)
#define USB0_PROVIDERS (&providers.shared)
#include "usb0_service.h"

/* One zero-byte request. Header: six LEu32 fields(magic,version,USB state,
 * USB bytes,radio state,verified radio bytes), LEu64 epoch/generation, then
 * profile SHA256. Only USB state1 carries a copied payload:512 bytes for a
 * single result or1088 for combined workload4 after the72-byte header. Radio state2 follows full comparison and successful TCP CLOSE. */
static uint8_t response[1160];
static unsigned response_bytes,response_sent,response_transferred;
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
static void exchange(void)
{
    if(response_bytes){unsigned n=tud_vendor_write(response+response_sent,response_bytes-response_sent);
        response_sent+=n;tud_vendor_flush();return;}
    if(!tud_vendor_available())return;
    uint8_t request;if(tud_vendor_available()!=1||tud_vendor_read(&request,1)!=1||request)stop();
    unsigned result=atomic_load_explicit(&board.result,memory_order_acquire);
    put32(response,0x52414f42);
    /* Acquiring result publishes immutable host kind and copied backing. */
    unsigned dual=board.session.workload==4;
    unsigned ss=board.session.workload==6;
    unsigned audio=board.session.workload==14;
    unsigned payload=ss?544:(dual||audio)?1088:512;
    put32(response+4,result==1?(audio?5:ss?4:dual?3:board.host.kind==A733_USB2_DEVICE_HID?2:1):1);put32(response+8,result);put32(response+12,result==1?payload:0);
    unsigned radio=atomic_load_explicit(&board.radio_result,memory_order_acquire);
    if(a733_board_radio_workload(board.session.workload)&&(atomic_load_explicit(&board.watchdog_state,memory_order_acquire)&4))radio=3;
    put32(response+16,radio);
    put32(response+20,radio==2?atomic_load_explicit(&board.radio_bytes,memory_order_acquire):0);
    put32(response+24,(uint32_t)board.session.epoch);put32(response+28,(uint32_t)(board.session.epoch>>32));
    put32(response+32,(uint32_t)board.session.generation);put32(response+36,(uint32_t)(board.session.generation>>32));
    memcpy(response+40,board.session.radio_profile_digest,32);
    if(result==1)memcpy(response+72,ss?board.ss_result:(dual||audio)?board.dual_result:board.sector,payload);
    response_sent=response_transferred=0;response_bytes=result==1?72+payload:72;
}
void board_usb0_task(void *unused)
{
    (void)unused;
    usb0_service_start();
    usb0_service_run(exchange);
}
