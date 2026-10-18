#include "h264.h"
#include "boot.h"
#include "platform.h"
#include "tusb.h"
#include "device/dcd.h"
#include <string.h>
#ifdef SUNSTONE_BOARD_SUPERVISED
#define BOARD_USB0_PID 0xefe8
#else
#define BOARD_USB0_PID 0xefea
#endif
static unsigned polling;
static _Noreturn void stop(void){board_fail(NULL);for(;;)vTaskDelay(1);}
void a733_usb_prepare(void)
{for(;;){int r=a733_native_usb_health(&providers.shared);if(r<0||!board_live(NULL))stop();if(r==1)return;vTaskDelay(1);}}
void a733_usb_poll_enable(bool enable){if(!board_live(NULL))stop();polling=enable;}
void a733_usb_trace(uint32_t event,uint32_t a,uint32_t b)
{(void)event;(void)a;(void)b;if(!board_live(NULL))stop();}
void a733_usb_assert(void){stop();}
uint32_t tusb_time_millis_api(void){return (uint32_t)(a733_smp_counter()/24000);}
void tusb_time_delay_ms_api(uint32_t ms){if(ms>150||!board_live(NULL))stop();vTaskDelay(pdMS_TO_TICKS(ms));}
static const tusb_desc_device_t device={.bLength=sizeof(tusb_desc_device_t),.bDescriptorType=TUSB_DESC_DEVICE,
    .bcdUSB=0x200,.bMaxPacketSize0=64,.idVendor=0x1f3a,.idProduct=BOARD_USB0_PID,.bcdDevice=0x0100,
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
{(void)language;static uint16_t out[64];static const char *const text[]={"","sunstoneos","sunstoneos native H.264","sunstoneos-a733-h264","Capture"};
 if(!i){out[0]=0x0304;out[1]=0x0409;return out;}if(i>=5)return NULL;size_t n=strlen(text[i]);out[0]=(uint16_t)(0x0300|2*n+2);for(size_t j=0;j<n;++j)out[j+1]=(uint8_t)text[i][j];return out;}

/* One zero-valued byte requests a coherent private result. Header128bytes:
 * LE32 magic/version/status/bytes, LE64 epoch/generation, fixture/reference/
 * copied-payload SHA256. Only status1 appends2304 verified copied Y/Cb/Cr bytes.
 * Device buffers remain DEVICE-owned; status2 carries no payload. */
static uint8_t h264_response[128+A733_H264_OUTPUT_BYTES];
static unsigned h264_response_bytes,h264_response_sent,h264_response_transferred;
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
void tud_mount_cb(void){}
void tud_umount_cb(void){}
void tud_suspend_cb(bool wake){(void)wake;}
void tud_vendor_rx_cb(uint8_t interface,const uint8_t *buffer,uint32_t bytes)
{(void)buffer;(void)bytes;if(interface)stop();}
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{if(interface||bytes>h264_response_sent-h264_response_transferred)stop();h264_response_transferred+=bytes;
 if(h264_response_transferred==h264_response_bytes&&h264_response_sent==h264_response_bytes)h264_response_bytes=0;}
void tud_vendor_tx_complete_cb(uint8_t interface){(void)interface;}
static void h264_exchange(void)
{
    if(h264_response_bytes){unsigned n=tud_vendor_write(h264_response+h264_response_sent,h264_response_bytes-h264_response_sent);
        h264_response_sent+=n;tud_vendor_flush();return;}
    if(!tud_vendor_available())return;
    uint8_t request;if(tud_vendor_available()!=1||tud_vendor_read(&request,1)!=1||request)stop();
    unsigned result=atomic_load_explicit(&h264.result,memory_order_acquire);
    put32(h264_response,0x34363248);put32(h264_response+4,1);put32(h264_response+8,result);
    put32(h264_response+12,result==1?A733_H264_OUTPUT_BYTES:0);
    put32(h264_response+16,(uint32_t)board.session.epoch);put32(h264_response+20,(uint32_t)(board.session.epoch>>32));
    put32(h264_response+24,(uint32_t)board.session.generation);put32(h264_response+28,(uint32_t)(board.session.generation>>32));
    memcpy(h264_response+32,h264.session.fixture,32);memcpy(h264_response+64,h264.session.reference,32);
    if(result==1){memcpy(h264_response+96,h264.digest,32);memcpy(h264_response+128,h264.payload,A733_H264_OUTPUT_BYTES);}
    else memset(h264_response+96,0,32);
    h264_response_sent=h264_response_transferred=0;h264_response_bytes=result==1?sizeof(h264_response):128;
}
void board_usb0_task(void *unused)
{
    (void)unused;
    for(;;){if(!board_live(NULL))stop();int r=a733_native_usb_step(&providers.shared);if(r<0)stop();if(r==1)break;vTaskDelay(1);}
    tusb_rhport_init_t init={.role=TUSB_ROLE_DEVICE,.speed=TUSB_SPEED_HIGH};
    if(!tusb_init(0,&init))stop();
    for(;;){if(!board_live(NULL))stop();tud_task();a733_usb_prepare();if(polling)dcd_int_handler(0);h264_exchange();vTaskDelay(1);}
}
