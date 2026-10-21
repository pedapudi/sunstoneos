#include "uart.h"
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
#define USB0_PRODUCT "sunstoneos native UART"
#define USB0_SERIAL "sunstoneos-a733-uart"
#define USB0_LIVE() board_live(NULL)
#define USB0_PROVIDERS (&providers.shared)
#include "../board/usb0_service.h"

/* One zero-valued byte requests a coherent private result. Header of 128 bytes:
 * LE32 magic/version/status/bytes, LE64 epoch/generation, session/payload SHA256
 * and 32 reserved zero bytes. Only status 1 appends 144 copied observation bytes.
 * No application buffer or DMA ownership is transferred; status 2 has no payload. */
static uint8_t uart_response[128+UART_OUTPUT_BYTES];
static unsigned uart_response_bytes,uart_response_sent,uart_response_transferred;
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
void tud_mount_cb(void){}
void tud_umount_cb(void){}
void tud_suspend_cb(bool wake){(void)wake;}
void tud_vendor_rx_cb(uint8_t interface,const uint8_t *buffer,uint32_t bytes)
{(void)buffer;(void)bytes;if(interface)stop();}
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{if(interface||bytes>uart_response_sent-uart_response_transferred)stop();uart_response_transferred+=bytes;
 if(uart_response_transferred==uart_response_bytes&&uart_response_sent==uart_response_bytes)uart_response_bytes=0;}
void tud_vendor_tx_complete_cb(uint8_t interface){(void)interface;}
static void uart_exchange(void)
{
    if(uart_response_bytes){if(uart_response[8]==1&&!uart_output_ready())stop();unsigned n=tud_vendor_write(uart_response+uart_response_sent,uart_response_bytes-uart_response_sent);
        uart_response_sent+=n;tud_vendor_flush();return;}
    if(!tud_vendor_available())return;
    uint8_t request;if(tud_vendor_available()!=1||tud_vendor_read(&request,1)!=1||request)stop();
    unsigned result=atomic_load_explicit(&uart.result,memory_order_acquire);
    if(result==1&&!uart_output_ready())result=2;
    put32(uart_response,0x31545255);put32(uart_response+4,UART_WIRE_VERSION);put32(uart_response+8,result);
    put32(uart_response+12,result==1?uart.payload_bytes:0);
    put32(uart_response+16,(uint32_t)board.session.epoch);put32(uart_response+20,(uint32_t)(board.session.epoch>>32));
    put32(uart_response+24,(uint32_t)board.session.generation);put32(uart_response+28,(uint32_t)(board.session.generation>>32));
    memcpy(uart_response+32,uart.session_digest,32);if(result==1)memcpy(uart_response+64,uart.digest,32);else memset(uart_response+64,0,32);
    if(result==1){memset(uart_response+96,0,32);memcpy(uart_response+128,uart.payload,uart.payload_bytes);}
    else memset(uart_response+96,0,32);
    if(result==1&&!uart_output_ready()){result=2;put32(uart_response+8,2);put32(uart_response+12,0);memset(uart_response+64,0,64);}
    uart_response_sent=uart_response_transferred=0;uart_response_bytes=result==1?128+uart.payload_bytes:128;
}
void board_usb0_task(void *unused)
{
    (void)unused;
    usb0_service_start();
    usb0_service_run(uart_exchange);
}
