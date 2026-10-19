#include "application.h"
#include "session.h"
#include "boot.h"
#include "platform.h"
#include "tusb.h"
#include "device/dcd.h"
#include <string.h>
static unsigned polling;
static _Noreturn void stop(void){gpu_application_fail(NULL);for(;;)vTaskDelay(1);}
void a733_usb_prepare(void)
{for(;;){int r=a733_native_usb_health(&app.shared);if(r<0||!gpu_application_live(NULL))stop();if(r==1)return;vTaskDelay(1);}}
void a733_usb_poll_enable(bool enable){if(!gpu_application_live(NULL))stop();polling=enable;}
void a733_usb_trace(uint32_t event,uint32_t a,uint32_t b)
{(void)event;(void)a;(void)b;if(!gpu_application_live(NULL))stop();}
void a733_usb_assert(void){stop();}
uint32_t tusb_time_millis_api(void){return (uint32_t)(a733_smp_counter()/24000);}
void tusb_time_delay_ms_api(uint32_t ms){if(ms>150||!gpu_application_live(NULL))stop();vTaskDelay(pdMS_TO_TICKS(ms));}
static const tusb_desc_device_t device={.bLength=sizeof(tusb_desc_device_t),.bDescriptorType=TUSB_DESC_DEVICE,
    .bcdUSB=0x200,.bMaxPacketSize0=64,.idVendor=0x1f3a,.idProduct=0xefe8,.bcdDevice=0x0100,
    .iManufacturer=1,.iProduct=2,.iSerialNumber=3,.bNumConfigurations=1};
static const uint8_t hs[]={TUD_CONFIG_DESCRIPTOR(1,1,0,TUD_CONFIG_DESC_LEN+TUD_VENDOR_DESC_LEN,0,100),TUD_VENDOR_DESCRIPTOR(0,4,0x02,0x81,512)};
static const uint8_t fs[]={TUD_CONFIG_DESCRIPTOR(1,1,0,TUD_CONFIG_DESC_LEN+TUD_VENDOR_DESC_LEN,0,100),TUD_VENDOR_DESCRIPTOR(0,4,0x02,0x81,64)};
static const tusb_desc_device_qualifier_t qualifier={.bLength=sizeof(tusb_desc_device_qualifier_t),.bDescriptorType=TUSB_DESC_DEVICE_QUALIFIER,.bcdUSB=0x200,.bMaxPacketSize0=64,.bNumConfigurations=1};
const uint8_t *tud_descriptor_device_cb(void){return (const uint8_t *)&device;}
const uint8_t *tud_descriptor_device_qualifier_cb(void){return (const uint8_t *)&qualifier;}
const uint8_t *tud_descriptor_configuration_cb(uint8_t i){return i?NULL:tud_speed_get()==TUSB_SPEED_HIGH?hs:fs;}
const uint8_t *tud_descriptor_other_speed_configuration_cb(uint8_t i)
{static uint8_t other[sizeof(hs)];if(i)return NULL;memcpy(other,tud_speed_get()==TUSB_SPEED_HIGH?fs:hs,sizeof(hs));other[1]=TUSB_DESC_OTHER_SPEED_CONFIG;return other;}
const uint16_t *tud_descriptor_string_cb(uint8_t i,uint16_t language)
{(void)language;static uint16_t out[64];static const char *const text[]={"","sunstoneos","sunstoneos native GPU","sunstoneos-a733-gpu","Capture"};
 if(!i){out[0]=0x0304;out[1]=0x0409;return out;}if(i>=5)return NULL;size_t n=strlen(text[i]);out[0]=(uint16_t)(0x0300|2*n+2);for(size_t j=0;j<n;++j)out[j+1]=(uint8_t)text[i][j];return out;}

/* Request is one LEu32 copied-output chunk index (0..63). Response is
 * a 40-byte header plus 256 copied pixel bytes only after complete validation. */
static uint8_t response[296];
static unsigned response_bytes,response_sent,response_transferred;
static unsigned response_pending;
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
void tud_mount_cb(void){}
void tud_umount_cb(void){}
void tud_suspend_cb(bool wake){(void)wake;}
void tud_vendor_rx_cb(uint8_t interface,const uint8_t *buffer,uint32_t bytes)
{(void)buffer;(void)bytes;if(interface)stop();}
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{if(interface||bytes>response_sent-response_transferred)stop();response_transferred+=bytes;
 if(response_transferred==response_bytes&&response_sent==response_bytes){
    if(response_pending&&response_bytes&&gpu_application_live(NULL))
        atomic_store_explicit(&app.usb_observed,1,memory_order_release);
    response_bytes=0;response_pending=0;}}
void tud_vendor_tx_complete_cb(uint8_t interface){(void)interface;}
#ifdef SUNSTONE_GPU_RUNTIME_INPUT
/* Version2 request: magic/version/op/expected-sequence/epoch-lo/epoch-hi/
 * first-word/input-bytes, then bounded input bytes for SUBMIT. A partial frame
 * owns the mailbox but never reaches the service. */
static uint8_t runtime_header[32];
static unsigned runtime_header_bytes,runtime_received,runtime_length,runtime_reserved;
static uint64_t runtime_deadline;
static uint32_t get32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void runtime_exchange(void)
{
    if(!runtime_header_bytes)runtime_deadline=a733_smp_counter()/24+1000000;
    if(a733_smp_counter()/24>=runtime_deadline)stop();
    if(runtime_header_bytes<32) {
        unsigned available=tud_vendor_available(),n=32-runtime_header_bytes;
        if(n>available)n=available;
        runtime_header_bytes+=tud_vendor_read(runtime_header+runtime_header_bytes,n);
        if(runtime_header_bytes<32)return;
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
    unsigned result=atomic_load_explicit(&app.result,memory_order_acquire);
    unsigned sequence=atomic_load_explicit(&app.runtime_sequence,memory_order_acquire);
    unsigned total=gpu_application_package()->output_bytes/4,count=0,posted=0;
    uint64_t epoch=(uint64_t)get32(runtime_header+16)|(uint64_t)get32(runtime_header+20)<<32;
    int valid=get32(runtime_header)==0x49525047&&get32(runtime_header+4)==2&&epoch==app.session.epoch;
    if(!gpu_application_live(NULL))result=2;
    else if(!valid)result=4;
    else if(op==1&&!runtime_length&&first<total&&!(first&31)) {
        if(expected!=sequence&&!(result==0&&expected==sequence+1))result=4;
        else if(result==1)count=total-first<32?total-first:32;
    } else if(op==2&&runtime_reserved&&runtime_length==gpu_application_package()->input_bytes&&
              get32(app.runtime_input)==gpu_application_package()->count&&first==0&&
              result==1&&sequence&&sequence!=UINT32_MAX&&expected==sequence&&
              !atomic_load_explicit(&app.runtime_closed,memory_order_acquire)) {
        app.runtime_operation=2;app.runtime_expected=expected;app.runtime_bytes=runtime_length;
        atomic_store_explicit(&app.result,0,memory_order_release);
        atomic_store_explicit(&app.runtime_state,1,memory_order_release);
        runtime_reserved=0;result=0;posted=1;
    } else if(op==3&&!runtime_length&&!first&&result==1&&expected==sequence&&
              !atomic_load_explicit(&app.runtime_closed,memory_order_acquire)&&
              !atomic_load_explicit(&app.runtime_state,memory_order_acquire)) {
        app.runtime_operation=3;
        atomic_store_explicit(&app.result,0,memory_order_release);
        atomic_store_explicit(&app.runtime_state,1,memory_order_release);result=0;posted=1;
    } else result=4;
    if(runtime_reserved)atomic_store_explicit(&app.runtime_state,0,memory_order_release);
    if(!posted&&atomic_load_explicit(&app.runtime_closed,memory_order_acquire)&&result!=4&&result!=2){result=3;count=0;}
    put32(response,0x50555047);put32(response+4,2);put32(response+8,result);put32(response+12,first/32);
    put32(response+16,count);put32(response+20,total);put32(response+24,sequence);
    put32(response+28,(uint32_t)app.session.epoch);put32(response+32,(uint32_t)(app.session.epoch>>32));
    put32(response+36,count?total:0);
    for(unsigned i=0;i<count;++i)put32(response+40+4*i,app.pixels[first+i]);
    response_pending=result==0;response_sent=response_transferred=0;response_bytes=40+count*4;
    runtime_header_bytes=runtime_received=runtime_length=runtime_reserved=0;
}
#endif
static void exchange(void)
{
    if(response_bytes){unsigned n=tud_vendor_write(response+response_sent,response_bytes-response_sent);
        response_sent+=n;tud_vendor_flush();return;}
#ifdef SUNSTONE_GPU_RUNTIME_INPUT
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
    for(;;){if(!gpu_application_live(NULL))stop();int r=a733_native_usb_step(&app.shared);if(r<0)stop();if(r==1)break;vTaskDelay(1);}
    tusb_rhport_init_t init={.role=TUSB_ROLE_DEVICE,.speed=TUSB_SPEED_HIGH};
    if(!tusb_init(0,&init))stop();
    for(;;){if(!gpu_application_live(NULL))stop();tud_task();a733_usb_prepare();if(polling)dcd_int_handler(0);exchange();vTaskDelay(1);}
}
