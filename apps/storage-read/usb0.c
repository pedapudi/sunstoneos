#include "storage_read.h"
#include "boot.h"
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
{(void)language;static uint16_t out[64];static const char *const text[]={"","sunstoneos",
#ifdef SUNSTONE_EMMC_READ
 "sunstoneos native eMMC read","sunstoneos-a733-emmc-read",
#else
 "sunstoneos native SD0 read","sunstoneos-a733-storage-read",
#endif
 "Capture"};
 if(!i){out[0]=0x0304;out[1]=0x0409;return out;}if(i>=5)return NULL;size_t n=strlen(text[i]);out[0]=(uint16_t)(0x0300|2*n+2);for(size_t j=0;j<n;++j)out[j+1]=(uint8_t)text[i][j];return out;}

/* One host command and one copied response at a time. USB-owned bytes remain
 * immutable until actual TX callbacks account for every queued byte. */
static uint8_t response[STORAGE_READ_HEADER+STORAGE_READ_OUTPUT],input[32];
static unsigned response_bytes,response_sent,response_transferred,input_bytes,want_copy,rejected;
static uint64_t ack_pending,cancel_pending;
static struct storage_read_request rejected_request;
static uint32_t get32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t get64(const uint8_t *p){return get32(p)|(uint64_t)get32(p+4)<<32;}
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static void put64(uint8_t *p,uint64_t n){put32(p,(uint32_t)n);put32(p+4,(uint32_t)(n>>32));}
static void abandon(void)
{response_bytes=response_sent;if(response_transferred==response_sent)response_bytes=0;}
void tud_mount_cb(void){}
/* Disconnect/suspend cannot prove drain. Revoke permanently and retain the
 * staging buffer and counters; remount never resumes this service lifetime. */
void tud_umount_cb(void){storage_read_revoke(9);abandon();}
void tud_suspend_cb(bool wake){(void)wake;storage_read_revoke(9);abandon();}
void tud_vendor_rx_cb(uint8_t i,const uint8_t *b,uint32_t n)
{(void)b;(void)n;if(i)stop();}
void tud_vendor_tx_cb(uint8_t i,uint32_t n)
{
 if(i||n>response_sent-response_transferred)stop();response_transferred+=n;
 if(response_transferred==response_bytes&&response_sent==response_bytes){
  if(response[8]==STORAGE_READ_SUCCESS&&!atomic_load_explicit(&storage_read.cancel,memory_order_acquire))
   ack_pending=get64(response+240);
  response_bytes=0;
 }
}
void tud_vendor_tx_complete_cb(uint8_t i){(void)i;}
static void storage_read_exchange(void)
{
 if(ack_pending){if(storage_read_ack(ack_pending))ack_pending=0;
  else if(atomic_load_explicit(&storage_read.cancel,memory_order_acquire)||
   atomic_load_explicit(&storage_read.result,memory_order_acquire)==STORAGE_READ_RETAINED)ack_pending=0;}
 if(cancel_pending){int canceled=storage_read_cancel(cancel_pending);if(canceled==1){cancel_pending=0;abandon();}
  else if(canceled<0)cancel_pending=0;
  else if(atomic_load_explicit(&storage_read.cancel,memory_order_acquire))cancel_pending=0;}
 /* Assemble bounded commands across fragmented USB delivery. Cancellation
  * remains admissible while a previous response is partially transferred. */
 if(tud_vendor_available()){
  unsigned need=input_bytes?(input[0]<=1?1:32):1;
  unsigned count=tud_vendor_available();if(count>need-input_bytes)count=need-input_bytes;
  unsigned got=tud_vendor_read(input+input_bytes,count);if(got>count)stop();input_bytes+=got;
  if(input_bytes&&(input[0]>3))stop();need=input_bytes&&input[0]>1?32:1;
  if(input_bytes==need){
   unsigned op=input[0];input_bytes=0;
   if(op==1){storage_read_revoke(1);cancel_pending=ack_pending=0;abandon();}
   else if(op==0){if(!response_bytes&&!ack_pending)want_copy=1;}
   else {
    if(input[1]!=2||input[2]!=32||input[3]||get64(input+24))stop();
    if(op==3){if(get32(input+4)||get64(input+16))stop();cancel_pending=get64(input+8);}
    else {
     if(response_bytes||ack_pending||want_copy)stop();
     struct storage_read_request request={.first_sector=get64(input+16),.request_id=get64(input+8),.blocks=get32(input+4)};
     uint64_t ticket;int r=storage_read_submit(&request,&ticket);
     rejected=r==0||r==-1;rejected_request=request;want_copy=1;
    }
   }
  }
 }
 if(want_copy&&!response_bytes&&!ack_pending){
  if(!storage_read_copy(response,sizeof(response),&response_bytes))return;
  response_sent=response_transferred=0;want_copy=0;
  if(rejected&&get32(response+8)!=STORAGE_READ_RETAINED){put32(response+8,STORAGE_READ_REJECTED);put32(response+12,0);memset(response+32,0,32);
   put64(response+240,0);put64(response+248,rejected_request.first_sector);put64(response+256,rejected_request.request_id);
   put32(response+264,rejected_request.blocks);response_bytes=STORAGE_READ_HEADER;}
  rejected=0;
 }
 if(response_bytes&&response_sent<response_bytes){
  if(response[8]==STORAGE_READ_SUCCESS&&!storage_read_output_ready()){abandon();return;}
  unsigned count=tud_vendor_write(response+response_sent,response_bytes-response_sent);
  if(count>response_bytes-response_sent)stop();response_sent+=count;tud_vendor_flush();
 }
}
void board_usb0_task(void *unused)
{
    (void)unused;
    for(;;){if(!board_live(NULL))stop();int r=a733_native_usb_step(&providers.shared);if(r<0)stop();if(r==1)break;vTaskDelay(1);}
    tusb_rhport_init_t init={.role=TUSB_ROLE_DEVICE,.speed=TUSB_SPEED_HIGH};
    if(!tusb_init(0,&init))stop();
    for(;;){if(!board_live(NULL))stop();tud_task();a733_usb_prepare();if(polling)dcd_int_handler(0);storage_read_exchange();vTaskDelay(1);}
}
