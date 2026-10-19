#include "FreeRTOS.h"
#include "runtime.h"
#include "task.h"
#include "boot.h"
#include "result.h"
#include "a733_usb.h"
#include "tusb.h"
#include <string.h>
static StaticTask_t task_state;
static StackType_t task_stack[2048] __attribute__((aligned(16)));
static unsigned initialized,received,sent,response_bytes,transferred,ack_state;
static uint64_t ack_id,ack_offset;
static int payload;
static uint32_t sequence;
static uint8_t request[64],response[1040] __attribute__((aligned(64)));
static const tusb_desc_device_t device={
    .bLength=sizeof(tusb_desc_device_t),.bDescriptorType=TUSB_DESC_DEVICE,.bcdUSB=0x200,
    .bMaxPacketSize0=64,.idVendor=0x1f3a,.idProduct=0xefe8,.bcdDevice=0x0105,
    .iManufacturer=1,.iProduct=2,.iSerialNumber=3,.bNumConfigurations=1};
static const uint8_t hs[]={TUD_CONFIG_DESCRIPTOR(1,1,0,TUD_CONFIG_DESC_LEN+TUD_VENDOR_DESC_LEN,0,100),
    TUD_VENDOR_DESCRIPTOR(0,4,0x02,0x81,512)};
static const uint8_t fs[]={TUD_CONFIG_DESCRIPTOR(1,1,0,TUD_CONFIG_DESC_LEN+TUD_VENDOR_DESC_LEN,0,100),
    TUD_VENDOR_DESCRIPTOR(0,4,0x02,0x81,64)};
static const tusb_desc_device_qualifier_t qualifier={.bLength=sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType=TUSB_DESC_DEVICE_QUALIFIER,.bcdUSB=0x200,.bMaxPacketSize0=64,.bNumConfigurations=1};
uint8_t const *tud_descriptor_device_cb(void){return (const uint8_t *)&device;}
uint8_t const *tud_descriptor_device_qualifier_cb(void){return (const uint8_t *)&qualifier;}
uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{return index?NULL:(tud_speed_get()==TUSB_SPEED_HIGH?hs:fs);}
uint8_t const *tud_descriptor_other_speed_configuration_cb(uint8_t index)
{static uint8_t other[sizeof(hs)];if(index)return NULL;memcpy(other,tud_speed_get()==TUSB_SPEED_HIGH?fs:hs,sizeof(hs));other[1]=TUSB_DESC_OTHER_SPEED_CONFIG;return other;}
uint16_t const *tud_descriptor_string_cb(uint8_t index,uint16_t language)
{
    (void)language;static uint16_t out[64];
    static const char *const text[]={"","sunstoneos","sunstoneos NPU large graph input","sunstoneos-a733-npu-large","Result"};
    if(!index){out[0]=0x0304;out[1]=0x0409;return out;}
    if(index>=sizeof(text)/sizeof(*text))return NULL;
    unsigned n=(unsigned)strlen(text[index]);if(n>63)return NULL;out[0]=(uint16_t)(0x0300|2*n+2);
    for(unsigned i=0;i<n;++i)out[i+1]=(uint8_t)text[index][i];return out;
}
void tud_umount_cb(void)
{if(sequence||received||payload||response_bytes)npu_runtime_fail(41);}
void tud_suspend_cb(bool wake)
{(void)wake;if(sequence)npu_runtime_fail(41);}
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{
 if(interface||!response_bytes||bytes>sent-transferred){npu_runtime_fail(42);return;}
 transferred+=bytes;
 if(transferred==response_bytes&&sent==response_bytes){
  npu_runtime_ack(ack_state,ack_id,ack_offset);response_bytes=sent=transferred=ack_state=0;
 }
}
static void respond(void)
{
 uint32_t header[4]={A733_OBSERVE_MESSAGE_MAGIC,sequence,
  UINT32_C(0x80000034),1024};memcpy(response,header,16);
 npu_runtime_frame(response+16,&ack_state,&ack_id,&ack_offset);
 /* A SUBMIT/CLOSE acceptance response cannot consume an already completed
  * request. Only explicit READ with its matching ID acknowledges a result. */
 if(request[8]!=NPU_RUNTIME_READ&&request[8]!=NPU_RUNTIME_FINISH)ack_state=0;
 response_bytes=sizeof(response);sent=transferred=received=0;
}
void a733_observe_privileged(void)
{
 uint64_t el,mpidr;__asm__ volatile("mrs %0,CurrentEL\n mrs %1,mpidr_el1":"=r"(el),"=r"(mpidr));
 if(el!=4||(mpidr&~UINT64_C(0xc1000000)))a733_observe_failure(9);
 extern int npu_single_core_usb_poll(void);int prepared=npu_single_core_usb_poll();
 if(prepared<0)a733_observe_failure(13);if(!prepared)return;
 if(!a733_npu_application_observer_admit())a733_observe_failure(12);
 a733_observe_deadline();
 if(!initialized){tusb_rhport_init_t init={.role=TUSB_ROLE_DEVICE,.speed=TUSB_SPEED_HIGH};
  if(!tusb_init(0,&init))a733_observe_failure(10);initialized=1;}
 a733_usb_poll();tud_task_ext(0,false);a733_observe_deadline();
 /* Poll request expiry even when a host abandons an incomplete upload or a
  * completed response. This only latches failure; it does not reuse storage. */
 uint8_t health[1024];unsigned ignored;uint64_t ignored_id,ignored_offset;
 npu_runtime_frame(health,&ignored,&ignored_id,&ignored_offset);
 if(response_bytes){
  if(ack_state&&atomic_load_explicit(&npu_runtime.state,memory_order_acquire)==NR_FAILED){
   if(!sent){npu_runtime_frame(response+16,&ack_state,&ack_id,&ack_offset);}
   else{
    /* Already queued bytes cannot be recalled or mutated. Stop the unsent
     * remainder and retain the request; host framing rejects truncation. */
    response_bytes=sent;ack_state=0;
   }
  }
  if(sent<response_bytes){unsigned n=tud_vendor_write(response+sent,response_bytes-sent);
   if(n>response_bytes-sent){npu_runtime_fail(42);return;}sent+=n;tud_vendor_write_flush();}
 }else if(tud_mounted()&&tud_vendor_available()){
  if(payload>0){
   unsigned n=tud_vendor_available();if(n>(unsigned)payload)n=(unsigned)payload;if(n>NPU_RUNTIME_CHUNK)n=NPU_RUNTIME_CHUNK;
   uint8_t part[NPU_RUNTIME_CHUNK];unsigned got=tud_vendor_read(part,n);
   if(got>n){npu_runtime_fail(43);respond();return;}
   if(got){payload=npu_runtime_payload(part,got);if(payload<=0)respond();}
  }else{
   unsigned n=tud_vendor_available();if(n>64-received)n=64-received;
   unsigned got=tud_vendor_read(request+received,n);if(got>n){npu_runtime_fail(43);return;}received+=got;
   if(received==64){
    uint32_t next=(uint32_t)request[4]|(uint32_t)request[5]<<8|(uint32_t)request[6]<<16|(uint32_t)request[7]<<24;
    if(sequence==UINT32_MAX||next!=sequence+1)npu_runtime_fail(44);else sequence=next;
    payload=npu_runtime_header(request);if(payload<=0)respond();
   }
  }
 }
 a733_observe_deadline();
}
static void usb_task(void *unused)
{(void)unused;for(;;){
 /* A bounded batch services headers, a full upload chunk and response progress
  * without imposing a tick per64 bytes. Always yield after eight polls. */
 for(unsigned i=0;i<8;++i)a733_observe_privileged();
 vTaskDelay(1);
}}
void a733_observe_service_start(void)
{configASSERT(xTaskCreateStaticAffinitySet(usb_task,"npu-runtime-usb",2048,NULL,4,task_stack,&task_state,1));}
