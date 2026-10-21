#include "application.h"
#include "session.h"
#include "boot.h"
#include "platform.h"
#include "tusb.h"
#include "device/dcd.h"
#include <string.h>
static _Noreturn void stop(void){gpu_application_fail(NULL);for(;;)vTaskDelay(1);}
#define USB0_PID 0xefe8
#define USB0_PRODUCT "sunstoneos native GPU"
#define USB0_SERIAL "sunstoneos-a733-gpu"
#define USB0_LIVE() gpu_application_live(NULL)
#define USB0_PROVIDERS (&app.shared)
#include "../board/usb0_service.h"

/* Request is one LEu32 copied-output chunk index (0..63). Response is
 * a 40-byte header plus 256 copied pixel bytes only after complete validation. */
static uint8_t response[304];
static unsigned response_bytes,response_sent,response_transferred;
static unsigned response_pending;
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
void tud_mount_cb(void){}
void tud_umount_cb(void){}
void tud_suspend_cb(bool wake){(void)wake;}
void tud_vendor_rx_cb(uint8_t interface,const uint8_t *buffer,uint32_t bytes)
{(void)buffer;(void)bytes;if(interface)stop();}
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{
#ifdef SUNSTONE_GPU_RUNTIME_TEXTURE
 if(interface)stop();gpu_application_texture_tx(bytes);return;
#endif
 if(interface||bytes>response_sent-response_transferred)stop();response_transferred+=bytes;
 if(response_transferred==response_bytes&&response_sent==response_bytes){
    if(response_pending&&response_bytes&&gpu_application_live(NULL))
        atomic_store_explicit(&app.usb_observed,1,memory_order_release);
    response_bytes=0;response_pending=0;}}
void tud_vendor_tx_complete_cb(uint8_t interface){(void)interface;}
#if defined(SUNSTONE_GPU_RUNTIME_INPUT) || defined(SUNSTONE_GPU_RUNTIME_DRAW)
/* Version2 request: magic/version/op/expected-sequence/epoch-lo/epoch-hi/
 * first-word/input-bytes, then bounded input bytes for SUBMIT. A partial frame
 * owns the mailbox but never reaches the service. */
#ifdef SUNSTONE_GPU_LIFECYCLE
#define RUNTIME_HEADER_BYTES PVR_GPU_LIFECYCLE_REQUEST_BYTES
#define RUNTIME_REPLY_BYTES PVR_GPU_LIFECYCLE_REPLY_BYTES
#else
#define RUNTIME_HEADER_BYTES 32u
#define RUNTIME_REPLY_BYTES 40u
#endif
static uint8_t runtime_header[RUNTIME_HEADER_BYTES];
static unsigned runtime_header_bytes,runtime_received,runtime_length,runtime_reserved;
static uint64_t runtime_deadline;
static uint32_t get32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void runtime_exchange(void)
{
    if(!runtime_header_bytes)runtime_deadline=a733_smp_counter()/24+1000000;
    if(a733_smp_counter()/24>=runtime_deadline)stop();
    if(runtime_header_bytes<RUNTIME_HEADER_BYTES) {
        unsigned available=tud_vendor_available(),n=RUNTIME_HEADER_BYTES-runtime_header_bytes;
        if(n>available)n=available;
        runtime_header_bytes+=tud_vendor_read(runtime_header+runtime_header_bytes,n);
        if(runtime_header_bytes<RUNTIME_HEADER_BYTES)return;
        runtime_length=get32(runtime_header+28);
        if(runtime_length>PVR_COMPUTE_PACKAGE_INPUT)stop();
        if(runtime_length) {
            unsigned idle=0;
            runtime_reserved=atomic_compare_exchange_strong_explicit(&app.runtime_state,&idle,2,
                memory_order_acquire,memory_order_relaxed);
        }
    }
    if(runtime_received<runtime_length) {
        unsigned n=runtime_length-runtime_received,available=tud_vendor_available();
        if(n>available)n=available;
        uint8_t discard[64];
        if(!runtime_reserved&&n>sizeof(discard))n=sizeof(discard);
        runtime_received+=tud_vendor_read(runtime_reserved?app.runtime_input+runtime_received:discard,n);
        if(runtime_received<runtime_length)return;
    }
    unsigned op=get32(runtime_header+8),expected=get32(runtime_header+12),first=get32(runtime_header+24);
#ifdef SUNSTONE_GPU_LIFECYCLE
    taskENTER_CRITICAL();
#endif
    unsigned result=atomic_load_explicit(&app.result,memory_order_acquire);
    unsigned sequence=atomic_load_explicit(&app.runtime_sequence,memory_order_acquire);
    unsigned closed=atomic_load_explicit(&app.runtime_closed,memory_order_acquire);
    unsigned mailbox=atomic_load_explicit(&app.runtime_state,memory_order_acquire);
#ifdef SUNSTONE_GPU_LIFECYCLE
    uint64_t current_epoch=atomic_load_explicit(&app.runtime_epoch,memory_order_acquire);
    taskEXIT_CRITICAL();
#endif
    unsigned count=0,posted=0;
#ifdef SUNSTONE_GPU_RUNTIME_DRAW
    unsigned total=4096,version=3,page=64;
#else
    unsigned total=gpu_application_package()->output_bytes/4,version=2,page=32;
#endif
    uint64_t epoch=(uint64_t)get32(runtime_header+16)|(uint64_t)get32(runtime_header+20)<<32;
    #ifdef SUNSTONE_GPU_LIFECYCLE
    version+=4;
    uint64_t queue_epoch=(uint64_t)get32(runtime_header+32)|(uint64_t)get32(runtime_header+36)<<32;
#endif
    int valid=get32(runtime_header)==0x49525047&&get32(runtime_header+4)==version&&epoch==app.session.epoch;
#ifdef SUNSTONE_GPU_LIFECYCLE
    if(op>5 || (op==0?(queue_epoch && queue_epoch!=current_epoch):(!queue_epoch || queue_epoch!=current_epoch)))valid=0;
#endif
    if(!gpu_application_live(NULL))result=2;
    else if(!valid)result=4;
    #ifdef SUNSTONE_GPU_LIFECYCLE
    else if(op==0&&!runtime_length&&!first&&!expected) {count=0;}
#endif
    else if(op==1&&!runtime_length&&first<total&&!(first&(page-1))) {
        if(!sequence || closed ||
           (expected!=sequence&&!(result==0&&expected==sequence+1)))result=4;
        else if(result==1)count=total-first<page?total-first:page;
    }
#ifdef SUNSTONE_GPU_RUNTIME_DRAW
    else if(op==2&&runtime_reserved&&runtime_length<=PVR_RENDER_DRAW_BYTES&&first==0&&expected==sequence&&
        sequence<PVR_RENDER_DRAW_MAX_FRAMES&&(sequence?result==1:result!=2)&&
        pvr_render_draw_vertices_valid(app.runtime_input,runtime_length,runtime_length/32)&&
        !closed) {
#else
    else if(op==2&&runtime_reserved&&runtime_length==gpu_application_package()->input_bytes&&
              get32(app.runtime_input)==gpu_application_package()->count&&first==0&&
              result==1&&sequence&&sequence!=UINT32_MAX&&expected==sequence&&
              !closed) {
        #endif
        app.runtime_operation=2;app.runtime_expected=expected;app.runtime_bytes=runtime_length;
#ifdef SUNSTONE_GPU_LIFECYCLE
        app.runtime_request_epoch=queue_epoch;
#endif
        atomic_store_explicit(&app.result,0,memory_order_release);
        atomic_store_explicit(&app.runtime_state,1,memory_order_release);
        runtime_reserved=0;result=0;posted=1;
    } else if(op==3&&!runtime_length&&!first&&result==1&&expected==sequence&&
              !closed&&
              !mailbox) {
        app.runtime_operation=3;
#ifdef SUNSTONE_GPU_LIFECYCLE
        app.runtime_request_epoch=queue_epoch;
#endif
        atomic_store_explicit(&app.result,0,memory_order_release);
        atomic_store_explicit(&app.runtime_state,1,memory_order_release);result=0;posted=1;
#ifdef SUNSTONE_GPU_LIFECYCLE
    } else if((op==4 || op==5)&&!runtime_length&&!first&&expected==sequence&&
        (op==4?result==3:result==5)&&!mailbox) {
        app.runtime_operation=op;app.runtime_request_epoch=queue_epoch;
        atomic_store_explicit(&app.result,0,memory_order_release);
        atomic_store_explicit(&app.runtime_state,1,memory_order_release);result=0;posted=1;
#endif
    } else result=4;
    if(runtime_reserved)atomic_store_explicit(&app.runtime_state,0,memory_order_release);
    #ifndef SUNSTONE_GPU_LIFECYCLE
    if(!posted&&closed&&result!=4&&result!=2){result=3;count=0;}
#endif
    put32(response,0x50555047);put32(response+4,version);put32(response+8,result);put32(response+12,first/page);
    put32(response+16,count);put32(response+20,total);put32(response+24,sequence);
    put32(response+28,(uint32_t)app.session.epoch);put32(response+32,(uint32_t)(app.session.epoch>>32));
    put32(response+36,count?total:0);
    #ifdef SUNSTONE_GPU_LIFECYCLE
    put32(response+40,(uint32_t)current_epoch);put32(response+44,(uint32_t)(current_epoch>>32));
#endif
    for(unsigned i=0;i<count;++i)put32(response+RUNTIME_REPLY_BYTES+4*i,app.pixels[first+i]);
    response_pending=result==0;response_sent=response_transferred=0;response_bytes=RUNTIME_REPLY_BYTES+count*4;
    runtime_header_bytes=runtime_received=runtime_length=runtime_reserved=0;
}
#endif
static void exchange(void)
{
#ifdef SUNSTONE_GPU_RUNTIME_TEXTURE
    gpu_application_texture_usb();return;
#endif
    if(response_bytes){unsigned n=tud_vendor_write(response+response_sent,response_bytes-response_sent);
        response_sent+=n;tud_vendor_flush();return;}
#if defined(SUNSTONE_GPU_RUNTIME_INPUT) || defined(SUNSTONE_GPU_RUNTIME_DRAW)
    if(!tud_vendor_available()&&!runtime_header_bytes)return;
    runtime_exchange();return;
#else
    if(!tud_vendor_available())return;
    uint8_t request[4];if(tud_vendor_available()!=4||tud_vendor_read(request,4)!=4)stop();
    unsigned first=(unsigned)request[0]|(unsigned)request[1]<<8|(unsigned)request[2]<<16|(unsigned)request[3]<<24;
#ifdef SUNSTONE_GPU_COMPUTE_APPLICATION
#ifdef SUNSTONE_GPU_PROGRAM_APPLICATION
    if(first>=31)stop();
#else
    if(first>=2)stop();
#endif
#else
    if(first>=64)stop();
#endif
    unsigned result=atomic_load_explicit(&app.result,memory_order_acquire);
    if(!gpu_application_live(NULL))result=2;
    response_pending=result==0;
#ifdef SUNSTONE_GPU_COMPUTE_APPLICATION
#ifdef SUNSTONE_GPU_PROGRAM_APPLICATION
    unsigned total=gpu_application_package()->output_bytes/4;
    unsigned offset=first*32,count=offset<total?(total-offset<32?total-offset:32):0;
    if(!count)stop();
    put32(response,0x50555047);put32(response+4,1);put32(response+8,result);put32(response+12,first);
    put32(response+16,result==1?count:0);put32(response+20,total);put32(response+24,result==1?1:0);
    put32(response+28,(uint32_t)app.session.epoch);put32(response+32,(uint32_t)(app.session.epoch>>32));
    put32(response+36,result==1?total:0);
    if(result==1)for(unsigned i=0;i<count;++i)put32(response+40+4*i,app.pixels[offset+i]);
    response_sent=response_transferred=0;response_bytes=result==1?40+4*count:40;
#else
    /* Distinct magic and sequence identify one copied numerical submission. */
    put32(response,0x43555047);put32(response+4,1);put32(response+8,result);put32(response+12,first);
    put32(response+16,result==1?PVR_COMPUTE_JOB_WORDS:0);put32(response+20,2);
    put32(response+24,result==1?app.compute_sequence[first]:0);
    put32(response+28,(uint32_t)app.session.epoch);put32(response+32,(uint32_t)(app.session.epoch>>32));
    put32(response+36,result==1?2*PVR_COMPUTE_JOB_WORDS:0);
    if(result==1)for(unsigned i=0;i<PVR_COMPUTE_JOB_WORDS;++i)put32(response+40+4*i,app.compute_words[first][i]);
    response_sent=response_transferred=0;response_bytes=result==1?168:40;
#endif
#else
    put32(response,0x52555047);put32(response+4,1);put32(response+8,result);put32(response+12,first);
    put32(response+16,result==1?64:0);put32(response+20,64);put32(response+24,64);
    put32(response+28,(uint32_t)app.session.epoch);put32(response+32,(uint32_t)(app.session.epoch>>32));
    put32(response+36,result==1?4096:0);
    if(result==1)for(unsigned i=0;i<64;++i)put32(response+40+4*i,app.pixels[first*64+i]);
    response_sent=response_transferred=0;response_bytes=result==1?296:40;
#endif
#endif
}
void gpu_application_usb(void *unused)
{
    (void)unused;
    usb0_service_start();
    usb0_service_run(exchange);
}
