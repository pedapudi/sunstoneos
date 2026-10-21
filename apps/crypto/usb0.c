#include "crypto.h"
#include "boot.h"
#include "platform.h"
#include "tusb.h"
#include "device/dcd.h"
#include "a733_storage_digest.h"
#include <string.h>
#ifdef SUNSTONE_BOARD_SUPERVISED
#define USB0_PID 0xefe8
#else
#define USB0_PID 0xefe5
#endif
static uint8_t operation_prefix[5],operation_input[CRYPTO_REQUEST_MAX];
static unsigned operation_prefix_bytes,operation_bytes,operation_extent;
static void clear_upload(void)
{volatile uint8_t *p=operation_input;for(unsigned i=0;i<sizeof(operation_input);++i)p[i]=0;}
static _Noreturn void stop(void){clear_upload();board_fail(NULL);for(;;)vTaskDelay(1);}
#define USB0_PRODUCT "sunstoneos native CRYPTO"
#define USB0_SERIAL "sunstoneos-a733-crypto"
#define USB0_LIVE() board_live(NULL)
#define USB0_PROVIDERS (&providers.shared)
#include "../board/usb0_service.h"

/* One zero-valued byte requests a coherent private result. Header128bytes:
 * LE32 magic/version/status/bytes, LE64 epoch/generation, fixture/reference/
 * copied-payload SHA256. Only status1 appends160 verified SHA/encrypt/decrypt bytes.
 * Device buffers remain DEVICE-owned; status2 carries no payload. */
static uint8_t crypto_response[144+CRYPTO_INPUT_MAX];
static unsigned crypto_response_bytes,crypto_response_sent,crypto_response_transferred;
static uint32_t get32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
void tud_mount_cb(void){}
void tud_umount_cb(void){}
void tud_suspend_cb(bool wake){(void)wake;}
void tud_vendor_rx_cb(uint8_t interface,const uint8_t *buffer,uint32_t bytes)
{(void)buffer;(void)bytes;if(interface)stop();}
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{if(interface||bytes>crypto_response_sent-crypto_response_transferred)stop();crypto_response_transferred+=bytes;
 if(crypto_response_transferred==crypto_response_bytes&&crypto_response_sent==crypto_response_bytes)crypto_response_bytes=0;}
void tud_vendor_tx_complete_cb(uint8_t interface){(void)interface;}
static void crypto_exchange(void)
{
    if(crypto_response_bytes){if(crypto_response[8]==1&&!crypto_output_ready())stop();unsigned n=tud_vendor_write(crypto_response+crypto_response_sent,crypto_response_bytes-crypto_response_sent);
        crypto_response_sent+=n;tud_vendor_flush();return;}
    if(!tud_vendor_available())return;
    if(operation_prefix_bytes || crypto_session_runtime(&crypto.session)){
        if(!operation_prefix_bytes){
            uint8_t command;if(tud_vendor_read(&command,1)!=1)stop();
            if(!command){if(tud_vendor_available())stop();goto response;}
            if(command!=1||!crypto_input_live())stop();
            operation_prefix[0]=command;operation_prefix_bytes=1;
        }
        if(!crypto_input_live())stop();
        unsigned available=tud_vendor_available(),take=5-operation_prefix_bytes;
        if(take>available)take=available;
        if(take&&tud_vendor_read(operation_prefix+operation_prefix_bytes,take)!=take)stop();
        operation_prefix_bytes+=take;if(operation_prefix_bytes<5)return;
        if(!operation_extent){
            operation_extent=get32(operation_prefix+1);
            if(operation_extent<CRYPTO_REQUEST_HEADER||operation_extent>CRYPTO_REQUEST_MAX)stop();
        }
        available=tud_vendor_available();take=operation_extent-operation_bytes;
        if(available>take)stop();if(take>available)take=available;
        if(take&&tud_vendor_read(operation_input+operation_bytes,take)!=take)stop();
        operation_bytes+=take;if(operation_bytes<operation_extent)return;
        int accepted=crypto_accept(operation_input,operation_extent);clear_upload();
        if(!accepted)stop();operation_prefix_bytes=operation_bytes=operation_extent=0;
    }else{
        uint8_t request;if(tud_vendor_available()!=1||tud_vendor_read(&request,1)!=1||request)stop();
    }
response:;
    unsigned result=atomic_load_explicit(&crypto.result,memory_order_acquire);
    if(result==1&&!crypto_output_ready())result=2;
    put32(crypto_response,0x54505943);put32(crypto_response+4,1);put32(crypto_response+8,result);
    put32(crypto_response+12,result==1?CRYPTO_OUTPUT_BYTES:0);
    put32(crypto_response+16,(uint32_t)board.session.epoch);put32(crypto_response+20,(uint32_t)(board.session.epoch>>32));
    put32(crypto_response+24,(uint32_t)board.session.generation);put32(crypto_response+28,(uint32_t)(board.session.generation>>32));
    memcpy(crypto_response+32,crypto.session.fixture,32);memcpy(crypto_response+64,crypto.session.reference,32);
    if(result==1){memcpy(crypto_response+96,crypto.digest,32);memcpy(crypto_response+128,crypto.payload,CRYPTO_OUTPUT_BYTES);}
    else memset(crypto_response+96,0,32);
    if(result==1&&!crypto_output_ready()){result=2;put32(crypto_response+8,2);put32(crypto_response+12,0);memset(crypto_response+96,0,32);}
    if(crypto_session_runtime(&crypto.session)){
        put32(crypto_response+4,2);
        unsigned accepted=atomic_load_explicit(&crypto.input_ready,memory_order_acquire);
        const struct crypto_request *r=&crypto.run.request;
        put32(crypto_response+32,accepted?(uint32_t)r->id:0);
        put32(crypto_response+36,accepted?(uint32_t)(r->id>>32):0);
        put32(crypto_response+40,accepted?r->operation:0);put32(crypto_response+44,accepted?r->bytes:0);
        memcpy(crypto_response+48,crypto.session.fixture,32);
        storage_digest_bytes(&crypto.session,sizeof(crypto.session),crypto_response+80);
        memset(crypto_response+112,0,32);
        if(result==1){
            if(!accepted||!crypto_output_ready())stop();
            put32(crypto_response+12,crypto.run.output_bytes);
            memcpy(crypto_response+112,crypto.digest,32);
            memcpy(crypto_response+144,crypto.payload,crypto.run.output_bytes);
        }
        crypto_response_bytes=result==1?144+crypto.run.output_bytes:144;
    }else crypto_response_bytes=result==1?128+CRYPTO_OUTPUT_BYTES:128;
    crypto_response_sent=crypto_response_transferred=0;
}
void board_usb0_task(void *unused)
{
    (void)unused;
    usb0_service_start();
    usb0_service_run(crypto_exchange);
}
