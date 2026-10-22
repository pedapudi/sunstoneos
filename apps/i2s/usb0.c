#include "application.h"
#include "boot.h"
#include "platform.h"
#include "a733_range.h"
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
static uint8_t response[4240],request[12];
static unsigned response_bytes,response_sent,response_transferred,request_bytes;
static uint64_t delivered_ticket;
static unsigned delivered_offset,delivered_count;
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static uint32_t get32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
void tud_mount_cb(void){}
void tud_umount_cb(void){}
void tud_suspend_cb(bool wake){(void)wake;}
void tud_vendor_rx_cb(uint8_t interface,const uint8_t *buffer,uint32_t bytes)
{(void)buffer;(void)bytes;if(interface)stop();}
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{if(interface||bytes>response_sent-response_transferred)stop();response_transferred+=bytes;
 if(response_transferred==response_bytes&&response_sent==response_bytes){
  if(delivered_ticket&&!i2s_repeated_delivered(delivered_ticket,delivered_offset,delivered_count))stop();
  delivered_ticket=0;response_bytes=0;
 }}
void tud_vendor_tx_complete_cb(uint8_t interface){(void)interface;}
static void put64(uint8_t *p,uint64_t value)
{put32(p,(uint32_t)value);put32(p+4,(uint32_t)(value>>32));}
static void repeated_exchange(uint32_t operation,uint32_t offset,uint32_t bytes)
{
 struct i2s_repeated_period period={0};unsigned count=0;uint64_t acknowledged=0;
 if(operation==4){
  if(app.admitted.direction%3==1||offset>4096||bytes>4096-offset)stop();
  int got=i2s_repeated_copy(offset,bytes,response+128,&period);if(got<0)stop();
  if(got){count=bytes;delivered_ticket=period.ticket;delivered_offset=offset;delivered_count=bytes;}
 }else if(operation==5){
  acknowledged=(uint64_t)offset|((uint64_t)bytes<<32);
  if(!i2s_repeated_ack(acknowledged))stop();offset=bytes=0;
 }else if(operation==0||operation==2){
  if(offset||bytes)stop();if(operation==2)atomic_store_explicit(&app.cancel,1,memory_order_release);
 }else stop();
 unsigned result=atomic_load_explicit(&app.result,memory_order_acquire);
 put32(response,0x53493241);put32(response+4,3);put32(response+8,result);put32(response+12,offset);
 put32(response+16,count);put32(response+20,4096);
 put64(response+24,app.admitted.epoch);put64(response+32,app.admitted.generation);
 storage_digest_bytes(&app.admitted,sizeof(app.admitted),response+40);
 put64(response+72,operation==5?acknowledged:period.ticket);put64(response+80,period.first_frame);
 put32(response+88,period.frames);put32(response+92,period.counter);
 put64(response+96,atomic_load_explicit(&app.repeated.rx_frames,memory_order_acquire));
 put64(response+104,atomic_load_explicit(&app.repeated.tx_frames,memory_order_acquire));
 put32(response+112,atomic_load_explicit(&app.repeated.drained,memory_order_acquire));
 put32(response+116,atomic_load_explicit(&app.repeated.flags,memory_order_acquire));put64(response+120,0);
 response_sent=response_transferred=0;response_bytes=128+count;
}
/* Version4: one exact period follows op6/sequence-low/sequence-high. USB owns
 * unpublished copied input only; it never calls the DMA service or a provider. */
static unsigned pcm_upload_active,pcm_upload_bytes,pcm_upload_ok,pcm_upload_extent,pcm_file_response;
static uint64_t pcm_upload_sequence,pcm_upload_deadline;
static void pcm_response(unsigned operation,unsigned upload_result)
{
 unsigned file=pcm_file_response||atomic_load_explicit(&app.runtime.file_mode,memory_order_acquire);
 memset(response,0,file?160:128);put32(response,0x53493241);put32(response+4,file?5:4);
 put32(response+8,atomic_load_explicit(&app.result,memory_order_acquire));
 put32(response+12,operation);put32(response+20,4096);
 put64(response+24,app.admitted.epoch);put64(response+32,app.admitted.generation);
 storage_digest_bytes(&app.admitted,sizeof(app.admitted),response+40);
 put64(response+72,atomic_load_explicit(&app.runtime.head,memory_order_acquire));
 uint64_t consumed=atomic_load_explicit(&app.runtime.consumed,memory_order_acquire);
 uint64_t committed=atomic_load_explicit(&app.runtime.committed,memory_order_acquire);
 put64(response+80,committed);put64(response+88,consumed);
 put32(response+96,i2s_runtime_credits());
 put32(response+100,atomic_load_explicit(&app.runtime.flags,memory_order_acquire));
 put32(response+104,atomic_load_explicit(&app.runtime.drained,memory_order_acquire));
 put32(response+108,upload_result);
 if(file){
  uint64_t total=app.runtime.file_frames,head=atomic_load_explicit(&app.runtime.head,memory_order_acquire);
  put64(response+112,total);put64(response+120,head>total/I2S_RUNTIME_FRAMES?total:head*I2S_RUNTIME_FRAMES);
  put32(response+128,atomic_load_explicit(&app.runtime.eof,memory_order_acquire));
  unsigned done=atomic_load_explicit(&app.runtime.file_done,memory_order_acquire);
  put32(response+132,done);put32(response+136,done);
  put32(response+144,atomic_load_explicit(&app.runtime.stopped,memory_order_acquire));
 }
 response_sent=response_transferred=0;response_bytes=file?160:128;
}
static void pcm_ingest(void)
{
 if(a733_smp_counter()>=pcm_upload_deadline)stop();
 unsigned available=tud_vendor_available();if(!available)return;
 uint8_t fragment[512];unsigned n=pcm_upload_extent-pcm_upload_bytes;
 if(n>sizeof(fragment))n=sizeof(fragment);if(n>available)n=available;
 unsigned got=tud_vendor_read(fragment,n);if(!got||got>n)stop();
 if(pcm_upload_ok){int r=i2s_runtime_upload(pcm_upload_sequence,pcm_upload_bytes,fragment,got);
  if(r<0)stop();if(!r)pcm_upload_ok=0;}
 pcm_upload_bytes+=got;
 if(pcm_upload_bytes==pcm_upload_extent){
  if(!pcm_upload_ok)i2s_runtime_upload_discard();
  pcm_upload_active=0;pcm_response(6,pcm_upload_ok?1:2);
 }
}
static void exchange(void)
{
 if(response_bytes){unsigned n=tud_vendor_write(response+response_sent,response_bytes-response_sent);
  response_sent+=n;tud_vendor_flush();return;}
 if(pcm_upload_active){pcm_ingest();return;}
 unsigned available=tud_vendor_available();if(!available)return;
 if(available>sizeof(request)-request_bytes){
  if(app.admitted.direction!=I2S_RUNTIME_DIRECTION)stop();available=sizeof(request)-request_bytes;
 }
 request_bytes+=tud_vendor_read(request+request_bytes,available);
 if(request_bytes<sizeof(request))return;request_bytes=0;
 uint32_t op=get32(request),offset=get32(request+4),bytes=get32(request+8);
 if(app.admitted.direction==I2S_RUNTIME_DIRECTION){
  if(op==7||op==8){
   pcm_file_response=1;
   uint64_t frames=(uint64_t)offset|((uint64_t)bytes<<32);
   pcm_response(op,(op==7?i2s_runtime_begin(frames):i2s_runtime_end(frames))?1:3);return;
  }
  if(op==6){
   pcm_upload_sequence=(uint64_t)offset|((uint64_t)bytes<<32);
   uint64_t head=atomic_load_explicit(&app.runtime.head,memory_order_acquire),now=a733_smp_counter(),deadline;
   if(!pcm_upload_sequence||head>=UINT64_MAX/I2S_RUNTIME_FRAMES||pcm_upload_sequence!=head+1||
      !a733_deadline_after(now,24000000,&deadline))stop();
   pcm_upload_extent=i2s_runtime_upload_bytes(pcm_upload_sequence);if(!pcm_upload_extent)stop();
   pcm_upload_deadline=deadline;pcm_upload_bytes=0;
   pcm_upload_ok=i2s_runtime_credits()!=0;pcm_upload_active=1;return;
  }
  if((op!=0&&op!=2)||offset||bytes)stop();
  if(op==2)atomic_store_explicit(&app.cancel,1,memory_order_release);
  pcm_response(op,0);return;
 }
 if(app.admitted.direction>=3){repeated_exchange(op,offset,bytes);return;}
#ifdef SUNSTONE_I2S_TRANSCRIBE
 if(op==3){
  if(bytes>4096||offset>I2S_RECOGNITION_LOG||bytes>I2S_RECOGNITION_LOG-offset)stop();
  struct i2s_recognition_snapshot snapshot={0};
  int r=i2s_recognition_copy(offset,bytes,response+144,&snapshot);
  unsigned state=r<0?3:r?2:0,count=r==1?bytes:0;
  if(r!=1)memset(&snapshot,0,sizeof(snapshot));
  put32(response,0x54533249);put32(response+4,1);put32(response+8,state);put32(response+12,offset);
  put32(response+16,count);put32(response+20,r==1?snapshot.bytes:0);
  put32(response+24,(uint32_t)app.admitted.epoch);put32(response+28,app.admitted.epoch>>32);
  put32(response+32,(uint32_t)app.admitted.generation);put32(response+36,app.admitted.generation>>32);
  storage_digest_bytes(&app.admitted,sizeof(app.admitted),response+40);
  memcpy(response+72,&snapshot,sizeof(snapshot));
  response_sent=response_transferred=0;response_bytes=144+count;return;
 }
#endif
 if(op>2||bytes>4096||offset>i2s_output_bytes()||bytes>i2s_output_bytes()-offset||
    (op!=1&&(offset||bytes)))stop();
 if(op==2)atomic_store_explicit(&app.cancel,1,memory_order_release);
 unsigned result=atomic_load_explicit(&app.result,memory_order_acquire);
 if(atomic_load_explicit(&app.cancel,memory_order_acquire))result=3;
 unsigned duplex=app.admitted.direction==2,base=duplex?96:72;
 unsigned count=0;if(op==1&&result==2&&bytes){
  if(i2s_output(offset,response+base,bytes))count=bytes;else result=3;
 }
 put32(response,0x53493241);put32(response+4,duplex?2:1);put32(response+8,result);put32(response+12,offset);
 put32(response+16,count);put32(response+20,i2s_output_bytes());
 put32(response+24,(uint32_t)app.admitted.epoch);put32(response+28,(uint32_t)(app.admitted.epoch>>32));
 put32(response+32,(uint32_t)app.admitted.generation);put32(response+36,(uint32_t)(app.admitted.generation>>32));
 storage_digest_bytes(&app.admitted,sizeof(app.admitted),response+40);
 if(duplex){
  memset(response+72,0,24);
  if(result==2){
   put32(response+72,app.output_bytes);put32(response+76,app.playback_bytes);
   put32(response+80,app.periods);put32(response+84,app.playback_periods);
   put32(response+88,app.duplex_drained);
  }
 }
 response_sent=response_transferred=0;response_bytes=base+count;
}
void i2s_usb0_task(void *unused)
{
    (void)unused;
    for(;;){if(!i2s_live(NULL))stop();int r=a733_native_usb_step(&app.shared);if(r<0)stop();if(r==1)break;vTaskDelay(1);}
    tusb_rhport_init_t init={.role=TUSB_ROLE_DEVICE,.speed=TUSB_SPEED_HIGH};
    if(!tusb_init(0,&init))stop();
    for(;;){if(!i2s_live(NULL))stop();tud_task();a733_usb_prepare();if(polling)dcd_int_handler(0);exchange();vTaskDelay(1);}
}
