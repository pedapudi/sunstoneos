#include "application.h"
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
static _Noreturn void stop(void){for(;;)vTaskDelay(1);}
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
{(void)language;static uint16_t out[64];static const char *const text[]={"","sunstoneos","sunstoneos native DisplayPort","sunstoneos-a733-displayport","Capture"};
 if(!i){out[0]=0x0304;out[1]=0x0409;return out;}if(i>=5)return NULL;size_t n=strlen(text[i]);out[0]=(uint16_t)(0x0300|2*n+2);for(size_t j=0;j<n;++j)out[j+1]=(uint8_t)text[i][j];return out;}

/* Each query byte0 returns one complete private1024-byte copy. Terminal
 * publication is immutable and requires no ACK. Keep the local copy owned
 * through the pinned TinyUSB automatic terminal ZLP completion. */
_Static_assert(CFG_TUD_VENDOR_TX_BUFSIZE>=1024,"DP reply fits one FIFO write");
static struct a733_displayport_reply response;
static unsigned active,sent,transferred,zlp;
void tud_mount_cb(void){}
void tud_umount_cb(void){if(active)stop();}
void tud_suspend_cb(bool wake){(void)wake;if(active)stop();}
void tud_vendor_rx_cb(uint8_t interface,const uint8_t *buffer,uint32_t bytes)
{(void)buffer;(void)bytes;if(interface)stop();}
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{
 if(interface||!active||bytes>sent-transferred||(zlp&&bytes))stop();
 transferred+=bytes;
 if(transferred==sizeof(response)&&sent==sizeof(response)){
  unsigned packet=tud_speed_get()==TUSB_SPEED_HIGH?512:64;
  if(bytes&&bytes%packet==0){zlp=1;return;}
  if(!bytes&&!zlp)stop();active=sent=transferred=zlp=0;
 }
}
void tud_vendor_tx_complete_cb(uint8_t interface){if(interface)stop();}
static void exchange(void)
{
 if(active){
  if(!sent&&tud_vendor_write_available()>=sizeof(response)){
   unsigned count=tud_vendor_write(&response,sizeof(response));if(count!=sizeof(response))stop();
   sent=count;tud_vendor_flush();
  }return;
 }
 if(!tud_mounted()||!tud_vendor_available())return;
 uint8_t query;if(tud_vendor_read(&query,1)!=1||query)stop();
 if(atomic_load_explicit(&dp_app.terminal,memory_order_acquire))response=dp_app.reply;
 else{
  memset(&response,0,sizeof(response));response.magic=A733_DISPLAYPORT_RESULT_MAGIC;
  response.version=1;response.bytes=sizeof(response);response.retained=1;
  response.epoch=dp_app.session.board.epoch;response.generation=dp_app.session.board.generation;response.lease=dp_app.session.board.lease;
  memcpy(response.session_digest,displayport_admitted_session_digest(),32);
  memcpy(response.build_id,a733_smp_build_id,32);
 }
 active=1;sent=transferred=zlp=0;
}
void board_usb0_task(void *unused)
{
 (void)unused;for(;;){if(!board_live(NULL))stop();int r=a733_native_usb_step(&providers.shared);if(r<0)stop();if(r==1)break;vTaskDelay(1);}
 tusb_rhport_init_t init={.role=TUSB_ROLE_DEVICE,.speed=TUSB_SPEED_HIGH};if(!tusb_init(0,&init))stop();
 for(;;){if(!board_live(NULL))stop();tud_task();a733_usb_prepare();if(polling)dcd_int_handler(0);exchange();vTaskDelay(1);}
}
