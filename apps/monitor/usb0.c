#include "monitor.h"
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
{(void)language;static uint16_t out[64];static const char *const text[]={"","sunstoneos","sunstoneos native monitor","sunstoneos-a733-monitor","Capture"};
 if(!i){out[0]=0x0304;out[1]=0x0409;return out;}if(i>=5)return NULL;size_t n=strlen(text[i]);out[0]=(uint16_t)(0x0300|2*n+2);for(size_t j=0;j<n;++j)out[j+1]=(uint8_t)text[i][j];return out;}

/* GET: byte0 followed by LE64 last seen generation; STOP: byte1.
 * Each copied frame has128-byte header and an optional192-byte snapshot. */
static uint8_t response[320],request[9];
static unsigned response_bytes,response_sent,response_transferred,request_bytes;
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
static void monitor_exchange(void)
{
 if(response_bytes){
  if(response[8]==MONITOR_SNAPSHOT&&!monitor_output_eligible())stop();
  unsigned n=tud_vendor_write(response+response_sent,response_bytes-response_sent);
  response_sent+=n;tud_vendor_flush();return;}
 if(!tud_vendor_available())return;
 unsigned wanted=request_bytes?9:1;
 unsigned n=tud_vendor_read(request+request_bytes,wanted-request_bytes);request_bytes+=n;
 if(!request_bytes)return;
 if(request[0]>1)stop();
 if(!request[0]&&request_bytes<9){
  n=tud_vendor_read(request+request_bytes,9-request_bytes);request_bytes+=n;if(request_bytes<9)return;}
 struct monitor_snapshot snapshot;enum monitor_status result;
 if(request[0])result=monitor_request_stop();
 else {uint64_t after=0;for(unsigned i=0;i<8;++i)after|=(uint64_t)request[i+1]<<(8*i);
  result=monitor_read_snapshot(&snapshot,after);}
 request_bytes=0;
 memset(response,0,sizeof(response));put32(response,0x314e4f4d);put32(response+4,1);put32(response+8,result);
 put32(response+12,result==MONITOR_SNAPSHOT?sizeof(snapshot):0);
 memcpy(response+16,&board.session.epoch,8);memcpy(response+24,&board.session.generation,8);
 memcpy(response+32,monitor.session_digest,32);
 if(result==MONITOR_SNAPSHOT){memcpy(response+128,&snapshot,sizeof(snapshot));storage_digest_bytes(&snapshot,sizeof(snapshot),response+64);}
 if(result==MONITOR_SNAPSHOT&&!monitor_output_eligible()){
  result=MONITOR_FAILED;put32(response+8,result);put32(response+12,0);memset(response+64,0,64);}
 response_sent=response_transferred=0;response_bytes=result==MONITOR_SNAPSHOT?sizeof(response):128;
}
void board_usb0_task(void *unused)
{
 (void)unused;
 for(;;){if(!board_live(NULL))stop();int r=a733_native_usb_step(&providers.shared);if(r<0)stop();if(r==1)break;vTaskDelay(1);}
 tusb_rhport_init_t init={.role=TUSB_ROLE_DEVICE,.speed=TUSB_SPEED_HIGH};if(!tusb_init(0,&init))stop();
 for(;;){if(!board_live(NULL))stop();tud_task();a733_usb_prepare();if(polling)dcd_int_handler(0);monitor_exchange();vTaskDelay(1);}
}
