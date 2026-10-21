#include "application.h"
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
static unsigned protocol,response_owner,gpu_received,gpu_length,gpu_reserved;
static uint64_t frame_deadline;
enum { GPU_PROTOCOL=1,NPU_PROTOCOL=2 };
static uint64_t ack_id,ack_offset;
static int payload;
static unsigned payload_admitted,tx_zlp;
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
    static const char *const text[]={"","sunstoneos","sunstoneos GPU and NPU input","sunstoneos-a733-accelerators","Result"};
    if(!index){out[0]=0x0304;out[1]=0x0409;return out;}
    if(index>=sizeof(text)/sizeof(*text))return NULL;
    unsigned n=(unsigned)strlen(text[index]);if(n>63)return NULL;out[0]=(uint16_t)(0x0300|2*n+2);
    for(unsigned i=0;i<n;++i)out[i+1]=(uint8_t)text[index][i];return out;
}
void npu_runtime_expire(void);
static void transport_fail(unsigned reason)
{accelerator_shared_fail(reason);}
void tud_umount_cb(void)
{if(sequence||received||payload||response_bytes||protocol)transport_fail(41);}
void tud_suspend_cb(bool wake)
{(void)wake;if(sequence||protocol||response_bytes)transport_fail(41);}
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{
 if(interface||!response_bytes||bytes>sent-transferred||(tx_zlp&&bytes)){transport_fail(42);return;}
 transferred+=bytes;
 if(transferred==response_bytes&&sent==response_bytes){
  unsigned packet=tud_speed_get()==TUSB_SPEED_HIGH?512:64;
  if(bytes&&!(bytes&(packet-1))){tx_zlp=1;return;}
  if(!bytes&&!tx_zlp){transport_fail(42);return;}
  tx_zlp=0;
  if(response_owner==NPU_PROTOCOL)npu_runtime_ack(ack_state,ack_id,ack_offset);
  else if(response_owner==GPU_PROTOCOL)
   atomic_store_explicit(&accelerator_gpu.transport_ready,1,memory_order_release);
  else{transport_fail(42);return;}
  response_bytes=sent=transferred=ack_state=response_owner=0;
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
 response_bytes=sizeof(response);sent=transferred=received=protocol=0;response_owner=NPU_PROTOCOL;
}
static uint32_t get32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(uint8_t *p,uint32_t n)
{for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static void gpu_respond(void)
{
 struct accelerator_gpu *g=&accelerator_gpu;
 unsigned op=get32(request+8),expected=get32(request+12),first=get32(request+24);
 unsigned result=atomic_load_explicit(&g->result,memory_order_acquire);
 unsigned seq=atomic_load_explicit(&g->sequence,memory_order_acquire);
 unsigned total=accelerator_gpu_package()->output_bytes/4,count=0,posted=0;
 uint64_t epoch=get32(request+16)|(uint64_t)get32(request+20)<<32;
 if(!accelerator_gpu_live())result=2;
 else if(get32(request+4)!=2||epoch!=a733_observe_identity.epoch)result=4;
 else if(op==1&&!gpu_length&&first<total&&!(first&31)){
  if(expected!=seq&&!(result==0&&seq!=UINT32_MAX&&expected==seq+1))result=4;
  else if(result==1)count=total-first<32?total-first:32;
 }else if(op==2&&gpu_reserved&&gpu_length==accelerator_gpu_package()->input_bytes&&
          get32(g->input)==accelerator_gpu_package()->count&&!first&&result==1&&
          seq&&seq!=UINT32_MAX&&expected==seq&&!atomic_load_explicit(&g->closed,memory_order_acquire)){
  g->operation=2;g->expected=expected;g->bytes=gpu_length;
  atomic_store_explicit(&g->result,0,memory_order_release);
  atomic_store_explicit(&g->mailbox,1,memory_order_release);
  gpu_reserved=0;result=0;posted=1;
 }else if(op==3&&!gpu_length&&!first&&result==1&&expected==seq&&
          !atomic_load_explicit(&g->closed,memory_order_acquire)&&
          !atomic_load_explicit(&g->mailbox,memory_order_acquire)){
  g->operation=3;
  atomic_store_explicit(&g->result,0,memory_order_release);
  atomic_store_explicit(&g->mailbox,1,memory_order_release);result=0;posted=1;
 }else result=4;
 if(gpu_reserved)atomic_store_explicit(&g->mailbox,0,memory_order_release);
 if(!posted&&atomic_load_explicit(&g->closed,memory_order_acquire)&&result!=4&&result!=2){result=3;count=0;}
 put32(response,0x50555047);put32(response+4,2);put32(response+8,result);put32(response+12,first/32);
 put32(response+16,count);put32(response+20,total);put32(response+24,seq);
 put32(response+28,(uint32_t)a733_observe_identity.epoch);put32(response+32,(uint32_t)(a733_observe_identity.epoch>>32));
 put32(response+36,count?total:0);
 for(unsigned i=0;i<count;++i)put32(response+40+4*i,g->output[first+i]);
 response_bytes=40+count*4;sent=transferred=received=protocol=gpu_received=gpu_length=gpu_reserved=0;
 response_owner=GPU_PROTOCOL;
}
void a733_observe_privileged(void)
{
 if(!accelerator_shared_live())return;
 extern int npu_single_core_usb_poll(void);int prepared=npu_single_core_usb_poll();
 if(prepared<0){transport_fail(13);return;}if(!prepared)return;
 if(!a733_npu_application_observer_admit()){transport_fail(12);return;}
 a733_observe_deadline();
 if(!initialized){tusb_rhport_init_t init={.role=TUSB_ROLE_DEVICE,.speed=TUSB_SPEED_HIGH};
  if(!tusb_init(0,&init)){transport_fail(10);return;}initialized=1;}
 a733_usb_poll();tud_task_ext(0,false);a733_observe_deadline();
 /* Latch request expiry while a host stalls; frames are built only for replies. */
 npu_runtime_expire();
 if(response_bytes){
  if(response_owner==NPU_PROTOCOL&&ack_state&&atomic_load_explicit(&npu_runtime.state,memory_order_acquire)==NR_FAILED){
   if(!sent)npu_runtime_frame(response+16,&ack_state,&ack_id,&ack_offset);
   else{
    /* Keep the already copied complete frame immutable and drain it. A local
     * NPU failure cannot strand the common transport or truncate its framing.
     * Revocation removes acknowledgment; the next NPU reply reports failure. */
    ack_state=0;
   }
  }
  if(sent<response_bytes){unsigned n=tud_vendor_write(response+sent,response_bytes-sent);
   if(n>response_bytes-sent){transport_fail(42);return;}sent+=n;tud_vendor_write_flush();}
 }else{
  uint64_t now=a733_smp_counter();
  if((received||payload||protocol)&&now>=frame_deadline){transport_fail(45);return;}
  if(!tud_mounted()||!tud_vendor_available())return;
  if(!received&&!protocol&&!payload){
   uint64_t end=a733_observe_identity.deadline;
   if(now>=end){transport_fail(45);return;}
   frame_deadline=end-now<24000000?end:now+24000000;
  }
  if(payload>0){
   unsigned n=tud_vendor_available();if(n>(unsigned)payload)n=(unsigned)payload;if(n>NPU_RUNTIME_CHUNK)n=NPU_RUNTIME_CHUNK;
   uint8_t part[NPU_RUNTIME_CHUNK];unsigned got=tud_vendor_read(part,n);
   if(got>n){transport_fail(43);return;}
   if(got){
    payload-=(int)got;
    if(payload_admitted && npu_runtime_payload(part,got)!=payload)payload_admitted=0;
    /* Transport owns the advertised bounded chunk even if NPU admission or
     * execution fails. Drain its remainder before reading another magic. */
    if(!payload)respond();
   }
  }else{
   unsigned target=protocol==GPU_PROTOCOL?32:protocol==NPU_PROTOCOL?64:4;
   if(received<target){
    unsigned n=tud_vendor_available();if(n>target-received)n=target-received;
    unsigned got=tud_vendor_read(request+received,n);if(got>n){transport_fail(43);return;}received+=got;
    if(received<target)return;
    if(!protocol){
     uint32_t magic=get32(request);
     protocol=magic==0x49525047?GPU_PROTOCOL:magic==A733_OBSERVE_MESSAGE_MAGIC?NPU_PROTOCOL:0;
     if(!protocol)transport_fail(46);return;
    }
    if(protocol==NPU_PROTOCOL){
     uint32_t next=get32(request+4);
     if(sequence==UINT32_MAX||next!=sequence+1)npu_runtime_fail(44);else sequence=next;
     int admitted=npu_runtime_header(request);
     unsigned bytes=get32(request+48);
     if(get32(request+8)==NPU_RUNTIME_UPLOAD&&bytes){
      if(bytes>NPU_RUNTIME_CHUNK){transport_fail(46);return;}
      payload=(int)bytes;payload_admitted=admitted==payload;
     }else{payload=0;payload_admitted=0;respond();}
     return;
    }
    gpu_length=get32(request+28);
    if(gpu_length>PVR_COMPUTE_PACKAGE_INPUT){transport_fail(46);return;}
    if(gpu_length){unsigned idle=0;gpu_reserved=atomic_compare_exchange_strong_explicit(
      &accelerator_gpu.mailbox,&idle,2,memory_order_acquire,memory_order_relaxed);}
   }
   if(gpu_received<gpu_length){
    unsigned n=gpu_length-gpu_received,available=tud_vendor_available();if(n>available)n=available;
    uint8_t discard[64];if(!gpu_reserved&&n>sizeof(discard))n=sizeof(discard);
    unsigned got=tud_vendor_read(gpu_reserved?accelerator_gpu.input+gpu_received:discard,n);
    if(got>n){transport_fail(43);return;}gpu_received+=got;if(gpu_received<gpu_length)return;
   }
   gpu_respond();
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
void accelerator_usb_start(void)
{configASSERT(xTaskCreateStaticAffinitySet(usb_task,"accelerator-usb",2048,NULL,4,task_stack,&task_state,1));}

BaseType_t accelerator_usb_task_privileged(TaskFunction_t entry,void *argument)
{return entry==usb_task&&!argument?pdTRUE:pdFALSE;}
