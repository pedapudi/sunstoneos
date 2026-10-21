#include "encode.h"
#include "boot.h"
#include "platform.h"
#include "tusb.h"
#include "device/dcd.h"
#include <string.h>
#define USB0_PID 0xeff2
static _Noreturn void stop(void){board_fail(NULL);for(;;)vTaskDelay(1);}
#define USB0_PRODUCT "sunstoneos native H264 encoder"
#define USB0_SERIAL "sunstoneos-a733-h264-encode"
#define USB0_LIVE() board_live(NULL)
#define USB0_PROVIDERS (&providers.shared)
#include "../board/usb0_service.h"

/* Query:0,ordinal8. ACK:1,ordinalLE32,generationLE64,digest32. Each output
 * remains immutable after ACK. While a response awaits actual transferred-byte
 * completion, no queued request or ACK is consumed. */
static uint8_t response[H264_ENCODE_RESULT_HEADER],request[44];
static size_t response_bytes,sent,transferred,request_used;
static unsigned response_ordinal,request_kind,delivered[2];
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static uint32_t le32(const uint8_t *p)
{return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t le64(const uint8_t *p){return le32(p)|((uint64_t)le32(p+4)<<32);}
void tud_mount_cb(void){}
void tud_umount_cb(void){}
void tud_suspend_cb(bool wake){(void)wake;}
void tud_vendor_rx_cb(uint8_t interface,const uint8_t *buffer,uint32_t bytes)
{(void)buffer;(void)bytes;if(interface)stop();}
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{
 if(interface||bytes>sent-transferred)stop();transferred+=bytes;
 if(transferred==response_bytes&&sent==response_bytes){
  if(response_bytes>H264_ENCODE_RESULT_HEADER)delivered[response_ordinal]=1;response_bytes=0;
 }
}
void tud_vendor_tx_complete_cb(uint8_t interface){if(interface)stop();}
static int upload_now(void *unused,uint64_t *now)
{(void)unused;return board_live(NULL)&&a733_counter_clock_now(&providers.shared.counter,now)==1&&board_live(NULL)?1:-1;}
static void response_start(unsigned ordinal)
{
 if(ordinal>1)stop();
 unsigned ready=atomic_load_explicit(&h264_encode.result[ordinal],memory_order_acquire);
 unsigned failed=atomic_load_explicit(&h264_encode.fatal,memory_order_acquire);
 unsigned status=ready?1:failed?2:0;
 memset(response,0,sizeof(response));put32(response,0x45433248);put32(response+4,1);put32(response+8,status);
 put32(response+12,status==1?(uint32_t)h264_encode.output_bytes[ordinal]:0);
 put32(response+16,(uint32_t)board.session.epoch);put32(response+20,(uint32_t)(board.session.epoch>>32));
 put32(response+24,(uint32_t)board.session.generation);put32(response+28,(uint32_t)(board.session.generation>>32));
 memcpy(response+32,h264_encode.session.input_digest,32);
 if(status==1)memcpy(response+64,h264_encode.digest[ordinal],32);
 put32(response+96,ordinal);put32(response+100,2);put32(response+104,ordinal?0:1);
 put32(response+108,640);put32(response+112,480);put32(response+116,640);put32(response+120,66);
 put32(response+124,31);put32(response+128,26);put32(response+132,30);put32(response+136,1); /* AnnexB */
 sent=transferred=0;response_ordinal=ordinal;
 response_bytes=H264_ENCODE_RESULT_HEADER+(status==1?h264_encode.output_bytes[ordinal]:0);
}
static void exchange(void)
{
 if(response_bytes){
  const uint8_t *p;size_t left;
  if(sent<H264_ENCODE_RESULT_HEADER){p=response+sent;left=H264_ENCODE_RESULT_HEADER-sent;}
  else{p=h264_encode.storage.copies[response_ordinal]+sent-H264_ENCODE_RESULT_HEADER;left=response_bytes-sent;}
  if(left>512)left=512;unsigned n=tud_vendor_write(p,left);if(n>left)stop();sent+=n;tud_vendor_flush();return;
 }
 if(!tud_vendor_available())return;
 if(request_kind){
  size_t size=request_kind==1?1:44,n=size-request_used;if(n>tud_vendor_available())n=tud_vendor_available();
  if(tud_vendor_read(request+request_used,n)!=n)stop();request_used+=n;if(request_used!=size)return;
  if(request_kind==1){unsigned ordinal=request[0];request_kind=request_used=0;response_start(ordinal);return;}
  unsigned ordinal=le32(request);
  if(ordinal>1||le64(request+4)!=h264_encode.session.board.generation||!delivered[ordinal]||
    !atomic_load_explicit(&h264_encode.result[ordinal],memory_order_acquire)||
    atomic_load_explicit(&h264_encode.acked[ordinal],memory_order_acquire)||
    memcmp(request+12,h264_encode.digest[ordinal],32))stop();
  atomic_store_explicit(&h264_encode.acked[ordinal],1,memory_order_release);request_kind=request_used=0;return;
 }
 uint8_t incoming[512];size_t n=1;
 if(h264_encode.supplied.header_used&&!h264_encode_input_ready(&h264_encode.supplied)){
  n=h264_encode_input_remaining(&h264_encode.supplied);if(!n)stop();if(n>512)n=512;
  if(n>tud_vendor_available())n=tud_vendor_available();
 }
 if(tud_vendor_read(incoming,n)!=n)stop();
 if(n!=1||incoming[0]>1||(h264_encode.supplied.header_used&&!h264_encode_input_ready(&h264_encode.supplied))){
  if(!board_live(NULL)||h264_encode_input_feed(&h264_encode.supplied,&h264_encode.session,upload_now,NULL,
    board.session.deadline_us-10000,incoming,n)<0||!board_live(NULL))stop();return;
 }
 request_kind=incoming[0]?2:1;request_used=0;
}
void board_usb0_task(void *unused)
{
 (void)unused;
 usb0_service_start();
 usb0_service_run(exchange);
}
