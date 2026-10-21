#include "pcie.h"
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
#define USB0_PRODUCT "sunstoneos native PCIE"
#define USB0_SERIAL "sunstoneos-a733-pcie"
#define USB0_LIVE() board_live(NULL)
#define USB0_PROVIDERS (&providers.shared)
#include "../board/usb0_service.h"

/* One zero-valued byte requests a coherent private result. Header128bytes:
 * LE32 magic/version/status/bytes, LE64 epoch/generation, session/payload SHA256
 * and32reserved zero bytes. Only status1 appends64 copied observation bytes.
 * No endpoint buffer or DMA ownership is transferred; status2 has no payload. */
static uint8_t pcie_response[128+PCIE_OUTPUT_BYTES];
static unsigned pcie_response_bytes,pcie_response_sent,pcie_response_transferred;
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
void tud_mount_cb(void){}
void tud_umount_cb(void){}
void tud_suspend_cb(bool wake){(void)wake;}
void tud_vendor_rx_cb(uint8_t interface,const uint8_t *buffer,uint32_t bytes)
{(void)buffer;(void)bytes;if(interface)stop();}
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{if(interface||bytes>pcie_response_sent-pcie_response_transferred)stop();pcie_response_transferred+=bytes;
 if(pcie_response_transferred==pcie_response_bytes&&pcie_response_sent==pcie_response_bytes)pcie_response_bytes=0;}
void tud_vendor_tx_complete_cb(uint8_t interface){(void)interface;}
static void pcie_exchange(void)
{
    if(pcie_response_bytes){if(pcie_response[8]==1&&!pcie_output_ready())stop();unsigned n=tud_vendor_write(pcie_response+pcie_response_sent,pcie_response_bytes-pcie_response_sent);
        pcie_response_sent+=n;tud_vendor_flush();return;}
    if(!tud_vendor_available())return;
    uint8_t request;if(tud_vendor_available()!=1||tud_vendor_read(&request,1)!=1||request)stop();
    unsigned result=atomic_load_explicit(&pcie.result,memory_order_acquire);
    if(result==1&&!pcie_output_ready())result=2;
    put32(pcie_response,0x45494350);put32(pcie_response+4,1);put32(pcie_response+8,result);
    put32(pcie_response+12,result==1?PCIE_OUTPUT_BYTES:0);
    put32(pcie_response+16,(uint32_t)board.session.epoch);put32(pcie_response+20,(uint32_t)(board.session.epoch>>32));
    put32(pcie_response+24,(uint32_t)board.session.generation);put32(pcie_response+28,(uint32_t)(board.session.generation>>32));
    memcpy(pcie_response+32,pcie.session_digest,32);if(result==1)memcpy(pcie_response+64,pcie.digest,32);else memset(pcie_response+64,0,32);
    if(result==1){memset(pcie_response+96,0,32);memcpy(pcie_response+128,pcie.payload,PCIE_OUTPUT_BYTES);}
    else memset(pcie_response+96,0,32);
    if(result==1&&!pcie_output_ready()){result=2;put32(pcie_response+8,2);put32(pcie_response+12,0);memset(pcie_response+64,0,64);}
    pcie_response_sent=pcie_response_transferred=0;pcie_response_bytes=result==1?sizeof(pcie_response):128;
}
void board_usb0_task(void *unused)
{
    (void)unused;
    usb0_service_start();
    usb0_service_run(pcie_exchange);
}
