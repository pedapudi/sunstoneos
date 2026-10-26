#define A733_STORAGE_NATIVE_IMPLEMENTATION
#include "a733_storage_update_native.h"
#include "task.h"
#include "tusb.h"
#include "device/dcd.h"
#include "device/usbd_pvt.h"
#include <string.h>

#define MESSAGE_MAGIC UINT32_C(0x53534f53)
#define UPDATE_COMMAND UINT32_C(0x53555044)
static struct a733_storage_native *runtime;
static uint8_t request[16+A733_STORAGE_REQUEST_BYTES],response[16+A733_STORAGE_BODY];
static unsigned received,wanted=16,response_bytes,sent,transferred,polling,initialized;
static uint32_t sequence;
static uint32_t get32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(uint8_t *p,uint32_t v)
{for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(8*i));}
static void publish_begin(void)
{atomic_fetch_add_explicit(&runtime->transport.version,1,memory_order_seq_cst);}
static void publish_end(void)
{atomic_fetch_add_explicit(&runtime->transport.version,1,memory_order_seq_cst);}
static _Noreturn void stop(void)
{if(runtime)a733_storage_native_fail(runtime);for(;;)vTaskDelay(1);}
static int live(void)
{
    return runtime&&xTaskGetCurrentTaskHandle()==runtime->task_handle[2]&&
        a733_storage_native_live(runtime);
}
static void wait_health(void)
{
    for(;;) {
        if(!live())stop();
        int ready=a733_storage_native_usb_health(runtime);
        if(ready<0)stop();if(ready==1)return;
        vTaskDelay(1);
    }
}
uint32_t tusb_time_millis_api(void)
{uint64_t now;if(!live()||!a733_counter_clock_now(&runtime->providers.counter,&now))stop();return (uint32_t)(now/1000);}
void tusb_time_delay_ms_api(uint32_t milliseconds)
{
    uint64_t now;
    if(!live()||milliseconds>150||!a733_counter_clock_now(&runtime->providers.counter,&now)||
       now>=runtime->boot.grant.deadline_us||
       (uint64_t)milliseconds*1000>=runtime->boot.grant.deadline_us-now)stop();
    TickType_t ticks=pdMS_TO_TICKS(milliseconds);if(ticks)vTaskDelay(ticks);
    if(!live())stop();
}
/* The controller driver calls this hook after detach. Its only authority is
 * the actual prepared token; all clock and PHY writes belong to the platform. */
void a733_usb_prepare(void)
{
    wait_health();
    for(;;) {
        enum a733_usb_platform_result r=a733_usb_platform_poll(&runtime->providers.usb);
        if(r==A733_USB_PLATFORM_PREPARED)return;
        if(r!=A733_USB_PLATFORM_BUSY)stop();vTaskDelay(1);wait_health();
    }
}
void a733_usb_poll_enable(bool enable){if(!live())stop();polling=enable;}
void a733_usb_poll(void){wait_health();if(polling)dcd_int_handler(0);if(!live())stop();}
void a733_usb_trace(uint32_t event,uint32_t first,uint32_t second)
{(void)event;(void)first;(void)second;if(!live())stop();}
void a733_usb_assert(void){stop();}
static const tusb_desc_device_t device={
    .bLength=sizeof(tusb_desc_device_t),.bDescriptorType=TUSB_DESC_DEVICE,.bcdUSB=0x200,
    .bMaxPacketSize0=64,.idVendor=0x1f3a,.idProduct=0xefe8,.bcdDevice=0x0102,
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
    static const char *const text[]={"","sunstoneos","sunstoneos storage maintenance","sunstoneos-a733-storage","Storage"};
    if(!index){out[0]=0x0304;out[1]=0x0409;return out;}
    if(index>=sizeof(text)/sizeof(*text))return NULL;
    unsigned n=(unsigned)strlen(text[index]);if(n>63)return NULL;out[0]=(uint16_t)(0x0300|2*n+2);
    for(unsigned i=0;i<n;++i)out[i+1]=(uint8_t)text[index][i];return out;
}
void tud_mount_cb(void)
{if(!live())stop();publish_begin();atomic_store_explicit(&runtime->transport.connected,1,memory_order_seq_cst);publish_end();}
void tud_umount_cb(void)
{
    if(!runtime)return;
    publish_begin();atomic_store_explicit(&runtime->transport.connected,0,memory_order_seq_cst);publish_end();
    if(sequence||received||response_bytes)a733_storage_native_fail(runtime);
}
void tud_suspend_cb(bool remote_wakeup_en)
{(void)remote_wakeup_en;if(runtime&&(sequence||received||response_bytes))a733_storage_native_fail(runtime);}
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{
    if(!live()||interface||!response_bytes||bytes>sent-transferred) {
        if(runtime)a733_storage_native_fail(runtime);return;
    }
    transferred+=bytes;
    if(transferred==response_bytes&&sent==response_bytes) {
        publish_begin();atomic_store_explicit(&runtime->transport.completed_sequence,sequence,memory_order_seq_cst);publish_end();
    }
}
static void accept(void)
{
    uint8_t digest[32];struct storage_digest hash;
    uint32_t next=get32(request+4),command=get32(request+16+36);
    if(sequence==UINT32_MAX||next!=sequence+1||get32(request+16+32)!=next||
       command<A733_STORAGE_INFO||command>A733_STORAGE_FINISH||
       memcmp(request+16,runtime->boot.grant.nonce,32)||get32(request+16+52)||
       !tud_mounted()||atomic_load_explicit(&runtime->channel.state,memory_order_acquire))stop();
    storage_digest_init(&hash);if(!storage_digest_add(&hash,request,wanted))stop();storage_digest_end(&hash,digest);
    /* Publish transport identity before releasing the mailbox to its consumer. */
    publish_begin();
    atomic_store_explicit(&runtime->transport.accepted_sequence,next,memory_order_seq_cst);
    atomic_store_explicit(&runtime->transport.accepted_command,command,memory_order_seq_cst);
    for(unsigned i=0;i<4;++i) {
        uint64_t word=0;for(unsigned j=0;j<8;++j)word|=(uint64_t)digest[i*8+j]<<(j*8);
        atomic_store_explicit(&runtime->transport.accepted_hash[i],word,memory_order_seq_cst);
    }
    publish_end();
    if(!a733_storage_channel_submit(&runtime->channel,request+16,wanted-16))stop();
    sequence=next;received=0;wanted=16;
}
static void receive(void)
{
    unsigned available=tud_vendor_available();
    if(!available)return;
    if(response_bytes||atomic_load_explicit(&runtime->channel.state,memory_order_acquire))stop();
    unsigned count=wanted-received;if(count>available)count=available;
    unsigned got=tud_vendor_read(request+received,count);if(got!=count)stop();received+=got;
    if(received==16&&wanted==16) {
        unsigned body=get32(request+12);
        if(get32(request)!=MESSAGE_MAGIC||get32(request+8)!=UPDATE_COMMAND||body<88||body>A733_STORAGE_REQUEST_BYTES)stop();
        wanted=16+body;
    }
    if(received==wanted) {
        if(tud_vendor_available())stop();accept();
    }
}
/* Same 200 us / 64-pass budget and 125 us recent-progress window as the
 * shared board USB0 service. Our controller-before-TinyUSB order may consume
 * a completion and refill the FIFO in one pass, so count actual byte/sequence
 * changes rather than relying on FIFO levels. No queued byte is a completed IN. */
static void service_pass(void)
{
    if(!live()||!initialized)stop();
    a733_usb_poll();wait_health();tud_task_ext(0,false);if(!live())stop();
    if(sequence&&!tud_mounted())stop();
    if(response_bytes&&transferred==response_bytes) {
        if(atomic_load_explicit(&runtime->transport.accepted_command,memory_order_seq_cst)==A733_STORAGE_FINISH)stop();
        response_bytes=sent=transferred=0;
    }
    if(!response_bytes&&atomic_load_explicit(&runtime->channel.state,memory_order_acquire)==3) {
        size_t bytes;
        if(!a733_storage_channel_response(&runtime->channel,response+16,A733_STORAGE_BODY,&bytes)||
           bytes<56||get32(response+16)!=sequence||get32(response+20))stop();
        put32(response,MESSAGE_MAGIC);put32(response+4,sequence);
        put32(response+8,UPDATE_COMMAND|UINT32_C(0x80000000));put32(response+12,(uint32_t)bytes);
        response_bytes=(unsigned)bytes+16;sent=transferred=0;
        publish_begin();atomic_store_explicit(&runtime->transport.response_sequence,sequence,memory_order_seq_cst);publish_end();
    }
    if(response_bytes&&sent<response_bytes) {
        wait_health();
        sent+=tud_vendor_write(response+sent,response_bytes-sent);tud_vendor_write_flush();
    }
    wait_health();
    receive();
    if(!a733_maintenance_poll(runtime->maintenance))stop();
}
static uint64_t service_now(void)
{
    uint64_t now;
    if(!a733_counter_clock_now(&runtime->providers.counter,&now)||now>=runtime->boot.grant.deadline_us)stop();
    return now;
}
static void service_burst(void)
{
    uint64_t start=service_now(),active=start,previous=start;unsigned moved=0;
    for(unsigned pass=0;pass<64;++pass) {
        unsigned rx=received,tx=sent,done=transferred,seq=sequence;
        int pending=tud_task_event_ready();bool armed=usbd_edpt_busy(0,0x81);
        service_pass();
        int progress=rx!=received||tx!=sent||done!=transferred||seq!=sequence||
            pending||tud_task_event_ready()||(!armed&&usbd_edpt_busy(0,0x81));
        uint64_t now=service_now();if(now<previous)stop();previous=now;
        if(progress){moved=1;active=now;}
        if(!moved||now-start>=200||now-active>=125)return;
    }
}
void a733_storage_native_usb_task(void *p)
{
    runtime=p;
    while(!atomic_load_explicit(&runtime->active,memory_order_acquire))vTaskDelay(1);
    for(;;) {
        int r=a733_storage_native_usb_step(runtime);if(r<0)stop();if(r)break;vTaskDelay(1);
    }
    while(!atomic_load_explicit(&runtime->service_ready,memory_order_acquire)) {
        if(!live())stop();vTaskDelay(1);
    }
    tusb_rhport_init_t init={.role=TUSB_ROLE_DEVICE,.speed=TUSB_SPEED_HIGH};
    wait_health();
    if(!live()||!tusb_init(0,&init))stop();initialized=1;
    for(;;){service_burst();vTaskDelay(1);}
}
