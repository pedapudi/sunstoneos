#include "dsi.h"
#include "boot.h"
#include "a733_storage_digest.h"
#include "platform.h"
#include "tusb.h"
#include "device/dcd.h"
#include <string.h>
#ifdef SUNSTONE_BOARD_SUPERVISED
#define BOARD_USB0_PID 0xefe8
#else
#define BOARD_USB0_PID 0xefe5
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
{(void)language;static uint16_t out[64];static const char *const text[]={"","sunstoneos","sunstoneos native DSI","sunstoneos-a733-dsi","Capture"};
 if(!i){out[0]=0x0304;out[1]=0x0409;return out;}if(i>=5)return NULL;size_t n=strlen(text[i]);out[0]=(uint16_t)(0x0300|2*n+2);for(size_t j=0;j<n;++j)out[j+1]=(uint8_t)text[i][j];return out;}

/* One zero-valued byte requests a coherent private result. Header of 128 bytes:
 * LE32 magic/version/status/bytes, LE64 epoch/generation, session/payload SHA256
 * and 32 reserved zero bytes. Only status 1 appends 256 copied observation bytes.
 * No application buffer or DMA ownership is transferred; status 2 has no payload. */
static uint8_t dsi_response[128+DSI_OBSERVATION_BYTES];
static unsigned dsi_response_bytes,dsi_response_sent,dsi_response_transferred;
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
void tud_mount_cb(void){}
void tud_umount_cb(void){}
void tud_suspend_cb(bool wake){(void)wake;}
void tud_vendor_rx_cb(uint8_t interface,const uint8_t *buffer,uint32_t bytes)
{(void)buffer;(void)bytes;if(interface)stop();}
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{if(interface||bytes>dsi_response_sent-dsi_response_transferred)stop();dsi_response_transferred+=bytes;
 if(dsi_response_transferred==dsi_response_bytes&&dsi_response_sent==dsi_response_bytes)dsi_response_bytes=0;}
void tud_vendor_tx_complete_cb(uint8_t interface){(void)interface;}
static void dsi_exchange(void)
{
    if(dsi_response_bytes){if(dsi_response[8]==1&&!dsi_live())stop();unsigned n=tud_vendor_write(dsi_response+dsi_response_sent,dsi_response_bytes-dsi_response_sent);
        dsi_response_sent+=n;tud_vendor_flush();return;}
    if(!tud_vendor_available())return;
    uint8_t request;if(tud_vendor_available()!=1||tud_vendor_read(&request,1)!=1||request)stop();
    unsigned result=atomic_load_explicit(&dsi.result,memory_order_acquire);
    if(result==1&&!dsi_live())result=2;
    put32(dsi_response,0x31525344);put32(dsi_response+4,1);put32(dsi_response+8,result);
    put32(dsi_response+12,result==1?DSI_OBSERVATION_BYTES:0);
    put32(dsi_response+16,(uint32_t)board.session.epoch);put32(dsi_response+20,(uint32_t)(board.session.epoch>>32));
    put32(dsi_response+24,(uint32_t)board.session.generation);put32(dsi_response+28,(uint32_t)(board.session.generation>>32));
    uint8_t digest[32]={0};if(result==1)storage_digest_bytes(dsi.observation,sizeof(dsi.observation),digest);
    memcpy(dsi_response+32,dsi.profile_hash,32);if(result==1)memcpy(dsi_response+64,digest,32);else memset(dsi_response+64,0,32);
    if(result==1){memset(dsi_response+96,0,32);memcpy(dsi_response+128,dsi.observation,DSI_OBSERVATION_BYTES);}
    else memset(dsi_response+96,0,32);
    if(result==1&&!dsi_live()){result=2;put32(dsi_response+8,2);put32(dsi_response+12,0);memset(dsi_response+64,0,64);}
    dsi_response_sent=dsi_response_transferred=0;dsi_response_bytes=result==1?sizeof(dsi_response):128;
}
void board_usb0_task(void *unused)
{
    (void)unused;
    for(;;){if(!board_live(NULL))stop();int r=a733_native_usb_step(&providers.shared);if(r<0)stop();if(r==1)break;vTaskDelay(1);}
    tusb_rhport_init_t init={.role=TUSB_ROLE_DEVICE,.speed=TUSB_SPEED_HIGH};
    if(!tusb_init(0,&init))stop();
    for(;;){if(!board_live(NULL))stop();tud_task();a733_usb_prepare();if(polling)dcd_int_handler(0);dsi_exchange();vTaskDelay(1);}
}
