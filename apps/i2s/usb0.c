#include "application.h"
#include "boot.h"
#include "platform.h"
#include "a733_storage_digest.h"
#include "tusb.h"
#include "device/dcd.h"
#include <string.h>
static unsigned polling;
static _Noreturn void stop(void){i2s_fail(NULL);for(;;)vTaskDelay(1);}
void a733_usb_prepare(void)
{for(;;){int r=a733_native_usb_health(&app.shared);if(r<0||!i2s_live(NULL))stop();if(r==1)return;vTaskDelay(1);}}
void a733_usb_poll_enable(bool enable){if(!i2s_live(NULL))stop();polling=enable;}
void a733_usb_trace(uint32_t event,uint32_t a,uint32_t b)
{(void)event;(void)a;(void)b;if(!i2s_live(NULL))stop();}
void a733_usb_assert(void){stop();}
uint32_t tusb_time_millis_api(void){return (uint32_t)(a733_smp_counter()/24000);}
void tusb_time_delay_ms_api(uint32_t ms){if(ms>150||!i2s_live(NULL))stop();vTaskDelay(pdMS_TO_TICKS(ms));}
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
{(void)language;static uint16_t out[64];static const char *const text[]={"","sunstoneos","sunstoneos native I2S","sunstoneos-a733-i2s","Capture"};
 if(!i){out[0]=0x0304;out[1]=0x0409;return out;}if(i>=5)return NULL;size_t n=strlen(text[i]);out[0]=(uint16_t)(0x0300|2*n+2);for(size_t j=0;j<n;++j)out[j+1]=(uint8_t)text[i][j];return out;}

/* Fixed 12-byte requests: LEu32 operation(0=status,1=read,2=cancel),
 * byte offset, payload bytes(up to4096). Response: magic/version/state/offset/
 * payload bytes/total bytes, epoch/generation, immutable session SHA256,
 * then payload. One request remains in flight until the device reports TX. */
static uint8_t response[4168],request[12];
static unsigned response_bytes,response_sent,response_transferred,request_bytes;
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static uint32_t get32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
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
 unsigned available=tud_vendor_available();if(!available)return;
 if(available>sizeof(request)-request_bytes)stop();
 request_bytes+=tud_vendor_read(request+request_bytes,available);
 if(request_bytes<sizeof(request))return;request_bytes=0;
 uint32_t op=get32(request),offset=get32(request+4),bytes=get32(request+8);
 if(op>2||bytes>4096||offset>I2S_PCM_BYTES||bytes>I2S_PCM_BYTES-offset||
    (op!=1&&(offset||bytes)))stop();
 if(op==2)atomic_store_explicit(&app.cancel,1,memory_order_release);
 unsigned result=atomic_load_explicit(&app.result,memory_order_acquire);
 if(atomic_load_explicit(&app.cancel,memory_order_acquire))result=3;
 unsigned count=0;if(op==1&&result==2&&bytes){
  if(i2s_output(offset,response+72,bytes))count=bytes;else result=3;
 }
 put32(response,0x53493241);put32(response+4,1);put32(response+8,result);put32(response+12,offset);
 put32(response+16,count);put32(response+20,I2S_PCM_BYTES);
 put32(response+24,(uint32_t)app.admitted.epoch);put32(response+28,(uint32_t)(app.admitted.epoch>>32));
 put32(response+32,(uint32_t)app.admitted.generation);put32(response+36,(uint32_t)(app.admitted.generation>>32));
 storage_digest_bytes(&app.admitted,sizeof(app.admitted),response+40);
 response_sent=response_transferred=0;response_bytes=72+count;
}
void i2s_usb0_task(void *unused)
{
    (void)unused;
    for(;;){if(!i2s_live(NULL))stop();int r=a733_native_usb_step(&app.shared);if(r<0)stop();if(r==1)break;vTaskDelay(1);}
    tusb_rhport_init_t init={.role=TUSB_ROLE_DEVICE,.speed=TUSB_SPEED_HIGH};
    if(!tusb_init(0,&init))stop();
    for(;;){if(!i2s_live(NULL))stop();tud_task();a733_usb_prepare();if(polling)dcd_int_handler(0);exchange();vTaskDelay(1);}
}
