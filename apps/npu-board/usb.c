/* USB0 vendor protocol of the NPU board image over the board's shared loop
 * (apps/board/usb0_service.h). Each request is a 64-byte header, and an
 * UPLOAD is followed by its payload:
 *   0 u32 magic 0x3151424e ("NBQ1"), 4 u32 operation, 8 u32 sequence,
 *   12 u32 payload bytes, 16 u64 epoch, 24 u64 generation, 32 u32 argument
 *   (LOAD: catalogue index; UPLOAD and READ: tensor index), 36 u32 offset,
 *   40..63 zero.
 * Operations: 1 STATUS, 2 LOAD, 3 UPLOAD, 4 SUBMIT, 5 READ, 6 RETURN, 7 CLOSE.
 * Each request gets one 64-byte response; READ appends the output tensor:
 *   0 u32 magic 0x3152424e ("NBR1"), 4 operation, 8 sequence, 12 result,
 *   16 runtime state, 20 window, 24 model, 28 payload bytes, 32 u64 epoch,
 *   40 u64 generation, 48 u64 job ticket, 56 u32 inputs<<16|outputs,
 *   60 u32 bytes of the named tensor.
 * UPLOAD payload goes from the TinyUSB FIFO straight into the lent read
 * window, and READ transmits from the lent write window; each tensor byte is
 * copied once, by tud_vendor_read or tud_vendor_write. A malformed or
 * out-of-order request stops USB0 and fails the board session. */
#include "npu-board.h"
#include "boot.h"
#include "platform.h"
#include "tusb.h"
#include "device/dcd.h"
#include <string.h>
#define USB0_PID 0xefe8
static _Noreturn void stop(void){board_fail(NULL);for(;;)vTaskDelay(1);}
#define USB0_PRODUCT "sunstoneos native NPU"
#define USB0_SERIAL "sunstoneos-a733-npu-board"
#define USB0_LIVE() board_live(NULL)
#define USB0_PROVIDERS (&providers.shared)
#include "../board/usb0_service.h"
#define REQUEST_MAGIC UINT32_C(0x3151424e)
#define RESPONSE_MAGIC UINT32_C(0x3152424e)
enum { STATUS=1, LOAD, UPLOAD, SUBMIT, READ, RETURN, CLOSE };
static uint8_t header[64],response[64];
static unsigned header_used,operation,sequence,last_sequence,posted,tensor;
static uintptr_t upload_at;static size_t upload_left;
static uintptr_t payload;static size_t response_bytes,sent,transferred;static unsigned terminal_zlp;
/* Counts for the host model: bytes moved between the FIFO and the windows. */
uint64_t npu_board_usb_upload_bytes,npu_board_usb_output_bytes;
static uint32_t le32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t le64(const uint8_t *p){return le32(p)|(uint64_t)le32(p+4)<<32;}
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(8*i));}
static void put64(uint8_t *p,uint64_t v){put32(p,(uint32_t)v);put32(p+4,(uint32_t)(v>>32));}
void tud_mount_cb(void){}
void tud_umount_cb(void){}
void tud_suspend_cb(bool wake){(void)wake;}
void tud_vendor_rx_cb(uint8_t interface,const uint8_t *buffer,uint32_t bytes){(void)buffer;(void)bytes;if(interface)stop();}
/* A response that ends on a whole packet is closed by the zero-length packet
 * that follows it, as in the G2D image; the next response waits for it. */
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{
    if(interface||!response_bytes||bytes>sent-transferred)stop();
    if(!bytes){
        if(!terminal_zlp||transferred!=response_bytes||sent!=response_bytes)stop();
        response_bytes=0;terminal_zlp=0;return;
    }
    if(terminal_zlp)stop();
    transferred+=bytes;
    if(transferred==response_bytes&&sent==response_bytes){
        unsigned packet=tud_speed_get()==TUSB_SPEED_HIGH?512:64;
        if(bytes%packet==0)terminal_zlp=1;else response_bytes=0;
    }
}
void tud_vendor_tx_complete_cb(uint8_t interface){if(interface)stop();}
static void respond(unsigned result,size_t extra,uintptr_t from)
{
    const struct npu_board_client *c=&npu_board.client;
    memset(response,0,sizeof(response));
    put32(response,RESPONSE_MAGIC);put32(response+4,operation);put32(response+8,sequence);put32(response+12,result);
    put32(response+16,c->state);put32(response+20,atomic_load_explicit(&c->window,memory_order_acquire));
    put32(response+24,c->model);put32(response+28,(uint32_t)extra);
    put64(response+32,board.session.epoch);put64(response+40,board.session.generation);put64(response+48,c->job);
    put32(response+56,c->inputs<<16|c->outputs);
    put32(response+60,operation==READ&&tensor<c->outputs?c->output[tensor].bytes:
        operation==UPLOAD&&tensor<c->inputs?c->input[tensor].bytes:0);
    payload=from;sent=transferred=0;terminal_zlp=0;response_bytes=sizeof(response)+extra;
}
/* Header first, then the output tensor straight from the write window. */
static void transmit(void)
{
    while(sent<response_bytes) {
        const uint8_t *from=sent<sizeof(response)?response+sent:(const uint8_t *)payload+(sent-sizeof(response));
        size_t n=sent<sizeof(response)?sizeof(response)-sent:response_bytes-sent;
        unsigned accepted=tud_vendor_write(from,n);if(accepted>n)stop();
        if(sent>=sizeof(response))npu_board_usb_output_bytes+=accepted;
        sent+=accepted;if(accepted<n)break;
    }
    tud_vendor_flush();
}
static void request(void)
{
    struct npu_board_client *c=&npu_board.client;
    const unsigned bytes=le32(header+12),argument=le32(header+32),offset=le32(header+36);
    operation=le32(header+4);sequence=le32(header+8);tensor=argument;
    int zero=1;for(unsigned i=40;i<64;++i)zero&=!header[i];
    if(le32(header)!=REQUEST_MAGIC||sequence<=last_sequence||le64(header+16)!=board.session.epoch||
       le64(header+24)!=board.session.generation||!zero||(operation!=UPLOAD&&bytes)||
       !atomic_load_explicit(&npu_board.ready,memory_order_acquire))stop();
    last_sequence=sequence;
    const unsigned window=atomic_load_explicit(&c->window,memory_order_acquire);
    switch(operation) {
    case STATUS:respond(NPU_BOARD_RESULT_OK,0,0);return;
    case UPLOAD:
        if(window!=NPU_BOARD_READ_LENT||argument>=c->inputs||offset>c->input[argument].bytes||
           bytes>c->input[argument].bytes-offset)stop();
        upload_at=c->input[argument].cpu+offset;upload_left=bytes;
        if(!bytes)respond(NPU_BOARD_RESULT_OK,0,0);
        return;
    case READ:
        if(window!=NPU_BOARD_WRITE_LENT||argument>=c->outputs)stop();
        respond(NPU_BOARD_RESULT_OK,c->output[argument].bytes,c->output[argument].cpu);return;
    case LOAD:case SUBMIT:case RETURN:case CLOSE:
        /* Hand the lent window back before the client acts on the request. */
        if(window!=NPU_BOARD_HELD)npu_board_client_return(c);
        if(!npu_board_client_post(c,sequence,operation==LOAD?NPU_BOARD_LOAD:operation==SUBMIT?NPU_BOARD_SUBMIT:
           operation==RETURN?NPU_BOARD_RETURN:NPU_BOARD_CLOSE,argument))stop();
        posted=1;return;
    default:stop();
    }
}
static void exchange(void)
{
    if(response_bytes){transmit();return;}
    if(posted) {
        unsigned result=npu_board_client_answer(&npu_board.client,sequence);
        if(!result)return;
        posted=0;respond(result,0,0);if(result==NPU_BOARD_RESULT_FAULT)stop();return;
    }
    uint32_t available=tud_vendor_available();
    if(!available)return;
    if(upload_left) {
        size_t n=available<upload_left?available:upload_left;
        if(tud_vendor_read((void *)upload_at,(uint32_t)n)!=n)stop();
        npu_board_usb_upload_bytes+=n;upload_at+=n;upload_left-=n;
        if(!upload_left)respond(NPU_BOARD_RESULT_OK,0,0);
        return;
    }
    size_t n=sizeof(header)-header_used;if(n>available)n=available;
    if(tud_vendor_read(header+header_used,(uint32_t)n)!=n)stop();
    header_used+=(unsigned)n;
    if(header_used==sizeof(header)){header_used=0;request();}
}
void board_usb0_task(void *unused)
{
    (void)unused;
    usb0_service_start();
    usb0_service_run(exchange);
}
