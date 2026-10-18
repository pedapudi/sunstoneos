#include "native.h"
#include "session.h"
#include "cached.h"
#include "boot.h"
#include "a733_native_usb.h"
#include "a733_maintenance_watchdog_internal.h"
#include "a733_cpu_coherence.h"
#include "a733_storage_digest.h"
#include "platform.h"
#include "task.h"
#include "tusb.h"
#include "device/dcd.h"
#include <string.h>

#define LOG_CAPACITY (256u*1024u)
static struct a733_native_usb providers;
static struct a733_watchdog_window window;
static StaticTask_t task[3];
static StackType_t stack[3][2048];
static TaskHandle_t handles[3];
static atomic_uint state,finished,log_length,acknowledged,drain_complete,input_waiting;
static atomic_uint_fast64_t inference_progress;
static uint8_t log_bytes[LOG_CAPACITY];
static uint64_t live_until;
#ifndef SUNSTONE_MIMUS_SUPERVISED
static uint64_t minimum,seen;
#endif
static unsigned paced,polling;
#ifdef SUNSTONE_MIMUS_SUPERVISED
static uint8_t session_sha256[32];
static uint64_t session_epoch,session_generation;
#endif
static void exchange(void);
void a733_usb_poll(void);
static int live(void *unused)
{
    (void)unused;
    return (atomic_load_explicit(&state,memory_order_acquire)&5)==1 &&
        a733_smp_counter()/24<__atomic_load_n(&live_until,__ATOMIC_ACQUIRE) &&
        a733_smp_counter()/24<window.deadline_us;
}
static void fail(void *unused)
{(void)unused;atomic_fetch_or_explicit(&state,4,memory_order_seq_cst);
#ifdef SUNSTONE_MIMUS_SUPERVISED
 atomic_store_explicit(&providers.counter.failed,1,memory_order_release);
#endif
}
void a733_storage_maintenance_stop(void){fail(NULL);}
int a733_storage_native_task_privileged(void (*entry)(void *),void *context);
static _Noreturn void stop(void)
{fail(NULL);for(;;)vTaskDelay(1);}
void mimus_native_checkpoint(void)
{if(!live(NULL))stop();}
void platform_puts(const char *text)
{
#ifdef SUNSTONE_MIMUS_SUPERVISED
    mimus_native_checkpoint();
#endif
    taskENTER_CRITICAL();
    unsigned used=atomic_load_explicit(&log_length,memory_order_relaxed);
    size_t bytes=strlen(text);
    if(bytes>LOG_CAPACITY-used){fail(NULL);taskEXIT_CRITICAL();platform_panic("mimus capture full",0);}
    memcpy(log_bytes+used,text,bytes);
    atomic_store_explicit(&log_length,used+(unsigned)bytes,memory_order_release);
    taskEXIT_CRITICAL();
}
void platform_hex(uint64_t v)
{
    static const char digits[]="0123456789abcdef";char text[17];
    for(unsigned i=0;i<16;++i)text[i]=digits[(v>>(60-4*i))&15];text[16]=0;platform_puts(text);
}
void mimus_native_inference_progress(void)
{atomic_fetch_add_explicit(&inference_progress,1,memory_order_release);}
void mimus_native_finished(int success)
{atomic_store_explicit(&finished,success?1:2,memory_order_release);if(!success)fail(NULL);}
int mimus_native_paced(void){return paced;}
void mimus_native_waiting(int waiting){atomic_store_explicit(&input_waiting,waiting,memory_order_release);}
void mimus_native_input_progress(void)
{if(atomic_load_explicit(&input_waiting,memory_order_acquire))mimus_native_inference_progress();}
static void pmic(void *unused)
{
    (void)unused;
    for(;;){
        if(!live(NULL))stop();
        enum a733_pmic_status r=a733_pmic_poll(&providers.pmic);
        if(r==A733_PMIC_READY||r==A733_PMIC_COMPLETE)atomic_store_explicit(&providers.pmic_ready,1,memory_order_release);
        else if(r!=A733_PMIC_PENDING&&r!=A733_PMIC_BUSY)stop();
        vTaskDelay(1);
    }
}
static void supervise(void *unused)
{
    (void)unused;
    for(;;){
        if(!live(NULL))stop();
        uint64_t progress=atomic_load_explicit(&inference_progress,memory_order_acquire);
        uint64_t now=a733_smp_counter()/24;
#ifndef SUNSTONE_MIMUS_SUPERVISED
        if(progress!=seen && !atomic_load_explicit(&finished,memory_order_acquire)){
            uint64_t before,after;
            /* Only completed parallel ranges earn a renewal. Task execution,
             * USB polls and PMIC polls never advance this counter. */
            if(!a733_watchdog_hw_reload(&window,&providers.counter,&providers.locks,
                handles[2],&state,live_until,&before,&after))stop();
            uint64_t next=before+minimum-window.guard_us-A733_MAINTENANCE_OPERATION_US;
            if(next>window.deadline_us-window.guard_us)next=window.deadline_us-window.guard_us;
            __atomic_store_n(&live_until,next,__ATOMIC_RELEASE);seen=progress;
        }
#else
        (void)progress;
#endif
        if(atomic_load_explicit(&drain_complete,memory_order_acquire)){
            /* Acknowledgement ends work, never memory ownership. The default
             * watchdog or explicit supervised power removal owns recovery. */
            fail(NULL);
        }
        if(now>=window.deadline_us)stop();vTaskDelay(pdMS_TO_TICKS(20));
    }
}
static void usb(void *unused)
{
    (void)unused;
    for(;;){if(!live(NULL))stop();int r=a733_native_usb_step(&providers);if(r<0)stop();if(r==1)break;vTaskDelay(1);}
    tusb_rhport_init_t init={.role=TUSB_ROLE_DEVICE,.speed=TUSB_SPEED_HIGH};
    if(!tusb_init(0,&init))stop();
    for(;;){if(!live(NULL))stop();tud_task();a733_usb_poll();exchange();vTaskDelay(1);}
}
int a733_storage_native_task_privileged(void (*entry)(void *),void *context)
{return !context&&(entry==pmic||entry==usb||entry==supervise);}
uint32_t tusb_time_millis_api(void){if(!live(NULL))stop();return (uint32_t)(a733_smp_counter()/24000);}
void tusb_time_delay_ms_api(uint32_t ms){if(ms>150||!live(NULL))stop();vTaskDelay(pdMS_TO_TICKS(ms));if(!live(NULL))stop();}
void a733_usb_prepare(void)
{for(;;){int r=a733_native_usb_health(&providers);if(r<0||!live(NULL))stop();if(r==1)return;vTaskDelay(1);}}
void a733_usb_poll_enable(bool enable){if(!live(NULL))stop();polling=enable;}
void a733_usb_poll(void){a733_usb_prepare();if(polling)dcd_int_handler(0);}
void a733_usb_trace(uint32_t event,uint32_t first,uint32_t second){(void)event;(void)first;(void)second;if(!live(NULL))stop();}
void a733_usb_assert(void){stop();}
static const tusb_desc_device_t device={.bLength=sizeof(tusb_desc_device_t),.bDescriptorType=TUSB_DESC_DEVICE,
    .bcdUSB=0x200,.bMaxPacketSize0=64,.idVendor=0x1f3a,.idProduct=
#ifdef SUNSTONE_MIMUS_SUPERVISED
    0xefe8
#else
    0xefea
#endif
    ,.bcdDevice=0x0100,
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
{(void)language;static uint16_t out[64];static const char *const text[]={"","sunstoneos","sunstoneos native mimus","sunstoneos-a733-mimus","Capture"};
 if(!i){out[0]=0x0304;out[1]=0x0409;return out;}if(i>=5)return NULL;size_t n=strlen(text[i]);out[0]=(uint16_t)(0x0300|2*n+2);for(size_t j=0;j<n;++j)out[j+1]=(uint8_t)text[i][j];return out;}
/* Host sends {offset,acknowledged bytes} as two little-endian u32 values.
 * A response is {total,finished,offset,payload bytes}, followed by up to 1024
 * captured bytes. Supervised responses append epoch/generation/session SHA256
 * before payload; default response framing remains unchanged. The host
 * acknowledges only bytes already fsynced to disk. */
#ifdef SUNSTONE_MIMUS_SUPERVISED
#define RESPONSE_HEADER 64u
#else
#define RESPONSE_HEADER 16u
#endif
static uint8_t response[RESPONSE_HEADER+1024];static unsigned response_size,response_sent,response_done,response_transferred;
static uint32_t get32(const uint8_t *p){return a733_storage_wire32(p);}
static void put32(uint8_t *p,uint32_t x){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(x>>(8*i));}
void tud_mount_cb(void){}
void tud_umount_cb(void){fail(NULL);}
void tud_suspend_cb(bool wake){(void)wake;fail(NULL);}
void tud_vendor_rx_cb(uint8_t interface,const uint8_t *buffer,uint32_t bytes)
{(void)buffer;(void)bytes;if(interface)stop();}
static void exchange(void)
{
    static uint8_t request[8];static unsigned received;
    if(response_size){
        if(response_sent<response_size){unsigned n=tud_vendor_write(response+response_sent,response_size-response_sent);response_sent+=n;tud_vendor_flush();}
        return;
    }
    if(!tud_vendor_available())return;
    if(tud_vendor_available()>8-received)stop();
    received+=tud_vendor_read(request+received,8-received);if(received!=8)return;received=0;
    unsigned complete=atomic_load_explicit(&finished,memory_order_acquire);
    unsigned total=atomic_load_explicit(&log_length,memory_order_acquire),offset=get32(request),ack=get32(request+4);
    if(offset>total||ack>response_done||ack<atomic_load(&acknowledged))stop();
    atomic_store_explicit(&acknowledged,ack,memory_order_release);
    unsigned bytes=total-offset;if(bytes>1024)bytes=1024;
    put32(response,total);put32(response+4,complete);put32(response+8,offset);put32(response+12,bytes);
#ifdef SUNSTONE_MIMUS_SUPERVISED
    for(unsigned i=0;i<8;++i){response[16+i]=(uint8_t)(session_epoch>>(8*i));response[24+i]=(uint8_t)(session_generation>>(8*i));}
    memcpy(response+32,session_sha256,32);
#endif
    memcpy(response+RESPONSE_HEADER,log_bytes+offset,bytes);response_size=bytes+RESPONSE_HEADER;response_sent=0;response_transferred=0;response_done=offset;
}
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{if(interface||!response_size||bytes>response_sent-response_transferred)stop();response_transferred+=bytes;if(response_transferred==response_size&&response_sent==response_size){response_done+=response_size-RESPONSE_HEADER;
 if(response_size==RESPONSE_HEADER&&get32(response+4)==1&&atomic_load_explicit(&acknowledged,memory_order_acquire)==get32(response))atomic_store_explicit(&drain_complete,1,memory_order_release);
 response_size=0;}}
void tud_vendor_tx_complete_cb(uint8_t interface){(void)interface;}
void a733_smp_diagnostic_start(void)
{
    uint8_t raw[A733_STORAGE_SESSION_BYTES];
    if(!a733_smp_device_preparation_admitted())platform_panic("mimus bootstrap",1);
    memcpy(raw,(void *)A733_STORAGE_SESSION_ADDRESS,sizeof(raw));
    if(!a733_mimus_wire_valid(raw)||a733_storage_wire64(raw)!=A733_STORAGE_SESSION_MAGIC||
       a733_storage_wire64(raw+24)!=a733_cached_runtime[0].epoch||
#ifndef SUNSTONE_MIMUS_SUPERVISED
       a733_storage_wire64(raw+256)*24!=a733_cached_runtime[0].diagnostic_deadline
#else
       a733_storage_wire64(raw+8)!=2||a733_storage_wire64(raw+16)!=704||
       !a733_smp_contract_valid((const void *)A733_SMP_CONTRACT_ADDRESS,a733_smp_counter())
#endif
       )platform_panic("mimus session",2);
#ifndef SUNSTONE_MIMUS_SUPERVISED
    window=(struct a733_watchdog_window){a733_storage_wire64(raw+256),a733_storage_wire64(raw+128),a733_storage_wire32(raw+144),a733_storage_wire32(raw+148),a733_storage_wire32(raw+152)};
    minimum=a733_storage_wire64(raw+112);uint64_t before=a733_storage_wire64(raw+640)/24;
    if(!a733_storage_wire_bounds(raw,a733_smp_counter())||minimum<=window.guard_us+A733_MAINTENANCE_OPERATION_US||
       !a733_watchdog_hw_admit(&window))platform_panic("mimus watchdog",3);
    live_until=before+minimum-window.guard_us-A733_MAINTENANCE_OPERATION_US;
    if(live_until>window.deadline_us-window.guard_us)live_until=window.deadline_us-window.guard_us;
#else
    window.deadline_us=a733_cached_runtime[0].diagnostic_deadline/24;
    live_until=window.deadline_us;
    storage_digest_bytes(raw,sizeof(raw),session_sha256);
    session_epoch=a733_storage_wire64(raw+24);session_generation=a733_storage_wire64(raw+32);
#endif
    struct a733_native_usb_config b={.identity=a733_storage_wire64(raw+32)};
    b.pmic.owner=a733_storage_wire64(raw+512);b.pmic.identity=b.identity;b.pmic.startup_deadline=live_until;
    for(unsigned i=0;i<2;++i)b.pmic.pins[i]=(struct a733_pin_config){raw[632+3*i],raw[633+3*i],raw[634+3*i]};
    b.pins.authorized[A733_PIN_L]=3;b.usb.owner=a733_storage_wire64(raw+520);b.usb.handoff=b.identity;b.usb.deadline_us=live_until;
    b.usb.cold_reference=a733_storage_wire32(raw+556);
    for(unsigned i=0;i<4;++i)b.usb_supply_uv[i]=a733_storage_wire32(raw+488+4*i);
    for(unsigned i=0;i<3;++i){b.usb.fabric[i]=a733_storage_wire32(raw+528+4*i);b.usb.rate_hz[i]=a733_storage_wire32(raw+540+4*i);}
    if(!a733_native_usb_init(&providers,&b,live,fail,NULL))platform_panic("mimus providers",4);
    TaskFunction_t functions[]={pmic,usb,supervise};
    for(unsigned i=0;i<3;++i){handles[i]=xTaskCreateStatic(functions[i],i==0?"mimus-pmic":i==1?"mimus-usb":"mimus-watchdog",2048,NULL,4,stack[i],&task[i]);if(!handles[i])platform_panic("mimus tasks",5);}
    if(!a733_native_usb_bind(&providers,handles[0],handles[1]))platform_panic("mimus bind",6);
    paced=(unsigned)a733_storage_wire64(raw+168);
    uint8_t hash[32];storage_digest_bytes((void *)0x43000000,(size_t)a733_storage_wire64(raw+176),hash);
    if(memcmp(hash,raw+216,32))platform_panic("mimus assets",7);
    const uint8_t *assets=(void *)0x43000000;uint64_t offset=128,extent=a733_storage_wire64(raw+176);
    if(memcmp(assets,"SUNMIM01",8))platform_panic("mimus asset header",7);
    for(unsigned i=0;i<3;++i){uint64_t bytes=a733_storage_wire64(assets+8+8*i);
        if(!bytes||offset>extent||bytes>extent-offset)platform_panic("mimus asset extent",7);
        offset=(offset+bytes+63)&~UINT64_C(63);if(offset>extent)platform_panic("mimus asset padding",7);}

    atomic_store_explicit(&state,1,memory_order_release);
    mimus_application_start();vTaskStartScheduler();platform_panic("scheduler returned",8);
}
