#include "application.h"
#include "../hdmi-audio/usb.h"
#include "../hdmi-audio/observation.h"
#include "a733_storage_digest.h"
struct hdmi_audio_usb usb;
#include "a733_usb_platform_native.h"
#include "aarch64_device_memory.h"
#include "boot.h"
#include "tusb.h"
#include "device/dcd.h"
#include <string.h>
_Static_assert(CFG_TUD_VENDOR_TX_BUFSIZE>=sizeof(struct hdmi_audio_observation),"Observation reply requires one bounded FIFO write");
static unsigned polling,received,sent,transferred,tx_active,tx_zlp;
static uint64_t exchange_deadline;
static uint8_t request[32];
static int terminal_idle_expired(void)
{
    return atomic_load_explicit(&app.state,memory_order_acquire)==2&&
        !received&&!tx_active&&!tx_zlp&&
        !atomic_load_explicit(&observation.state,memory_order_acquire)&&
        a733_smp_counter()/24>=app.deadline;
}
static _Noreturn void park(void)
{for(;;)vTaskSuspend(NULL);}
static _Noreturn void stop(void)
{if(terminal_idle_expired())park();hdmi_fail(NULL);park();}
static int owner(void)
{
    if(xTaskGetCurrentTaskHandle()!=app.handles[2])return 0;
    /* End a fully delivered healthy observation lifetime before hdmi_live can
     * revoke the common providers merely because the original window ended.
     * Partial requests/TX/ZLP still fail and retain everything. */
    if(terminal_idle_expired())park();return hdmi_live(NULL);
}
static int now(void *v,uint64_t *out)
{return v==&app&&owner()&&a733_counter_clock_now(&app.counter,out);}
static int providers(void)
{
    const struct hdmi_audio_usb_authority *a=usb.authority;
    if(!a||!owner()||usb.rail_index!=4||
       atomic_load_explicit(&app.locks.retained,memory_order_acquire)||
       usb.platform.token.owner!=a->owner||usb.rail.owner!=a->owner)return -1;
    enum a733_clock_result c=a733_clock_token_status(&app.clocks,&usb.platform.token);
    if(c==A733_CLOCK_BUSY)return 0;if(c!=A733_CLOCK_OK)return -1;
    enum a733_rail_result r=a733_rail_status(&app.rails,&usb.rail);
    if(r==A733_RAIL_BUSY)return 0;if(r!=A733_RAIL_READY)return -1;
    /* After startup this is only a locked metadata observation. No new PMIC
     * request can appear after the worker publishes USB readiness. */
    enum a733_pmic_status p=a733_pmic_live(&app.pmic,&usb.power);
    if(p==A733_PMIC_BUSY)return 0;return p==A733_PMIC_READY&&owner()?1:-1;
}
static int authorize(void *v,const struct a733_usb_platform_config *c,const struct a733_clock_token *t)
{
    const struct hdmi_audio_usb_authority *a=usb.authority;
    if(v!=&app||!a||c!=&usb.platform.config||t!=&usb.platform.token||
       c->owner!=a->owner||c->handoff!=a->handoff||c->deadline_us!=app.deadline||
       c->cold_reference!=A733_USB_COLD_REFERENCE||
       memcmp(c->fabric,app.session.usb.fabric,sizeof(c->fabric))||
       memcmp(c->rate_hz,app.session.usb.fabric_hz,sizeof(c->rate_hz)))return -1;
    int r=providers();if(r!=1)return r;
    const uintptr_t pages[]={0x02002000,0x04100000,0x07090160};
    for(unsigned i=0;i<3;++i){struct aarch64_translation tr;
        if(!aarch64_device_translate(pages[i],1,&tr)||tr.physical!=pages[i]||tr.attributes||tr.shareability!=2)return -1;}
    return 1;
}
static int enter(void *v,uint64_t deadline)
{return v==&app&&owner()&&deadline==app.deadline?
    a733_platform_lock_enter(&app.locks,A733_PLATFORM_REGISTER_LOCK,deadline):-1;}
static void leave(void *v)
{configASSERT(v==&app);a733_platform_lock_leave(&app.locks,A733_PLATFORM_REGISTER_LOCK);}
static void retain(void *v)
{/* enter may deny before acquisition. Retain only actual ownership, even
 * when liveness has already failed; do not call an asserting unowned retain. */
 if(v==&app&&xTaskGetSchedulerState()==taskSCHEDULER_RUNNING&&
    xTaskGetCurrentTaskHandle()==app.handles[2]&&
    xSemaphoreGetMutexHolder(app.locks.mutex[0])==app.handles[2])
    (void)a733_platform_lock_retain(&app.locks,A733_PLATFORM_REGISTER_LOCK);
 hdmi_fail(NULL);}
static int io_ready(void *v)
{
    if(v!=&app||!owner()||xSemaphoreGetMutexHolder(app.locks.mutex[0])!=app.handles[2])return 0;
    int r=providers();return r==0?A733_USB_PLATFORM_IO_BUSY:r==1?1:0;
}
static int read32(void *v,uintptr_t address,uint32_t *out)
{int r=io_ready(v);return r==1?a733_usb_platform_native_read(NULL,address,out):r;}
static int write32(void *v,uintptr_t address,uint32_t value)
{int r=io_ready(v);return r==1?a733_usb_platform_native_write(NULL,address,value):r;}
int hdmi_audio_usb_init(void)
{
    const struct hdmi_audio_usb_authority *a=&app.session.usb;
    if(!a||usb.authority||!app.handles[2]||!a->owner||!a->handoff||a->reserved||
       a->owner==app.session.owner||a->owner==app.session.pmic.owner||
       a->owner==app.session.audio.authority||a->owner==app.session.audio.dma.allowed.owner||a->cold_reference!=A733_USB_COLD_REFERENCE||
       a->supply_uv[0]!=3300000||a->supply_uv[1]!=3300000||
       a->supply_uv[2]!=1200000||a->supply_uv[3]!=900000)return 0;
    usb.authority=a;storage_digest_bytes(&app.session,sizeof(app.session),usb.session_sha256);
    struct a733_usb_platform_config c={.owner=a->owner,.handoff=a->handoff,
        .deadline_us=app.deadline,.cold_reference=a->cold_reference};
    memcpy(c.fabric,app.session.usb.fabric,sizeof(c.fabric));
    memcpy(c.rate_hz,app.session.usb.fabric_hz,sizeof(c.rate_hz));
    struct a733_usb_platform_ops ops={now,authorize,enter,leave,retain,read32,write32};
    return a733_usb_platform_init(&usb.platform,&c,&ops,&app,&app.clocks);
}
static int prepare(void)
{
    static const enum a733_rail rail[]={A733_RAIL_DCDC1,A733_RAIL_DC1SW2,A733_RAIL_DCDC9,A733_RAIL_ELDO6};
    if(!owner())return -1;
    if(!atomic_load_explicit(&app.pmic_ready,memory_order_acquire))return 0;
    if(!usb.step){
        a733_rail_mask_t mask=0;for(unsigned i=0;i<4;++i)mask|=A733_RAIL_BIT(rail[i]);
        enum a733_rail_result r=a733_rail_acquire(&app.rails,usb.authority->owner,mask,0,app.deadline,&usb.rail);
        if(r==A733_RAIL_BUSY)return 0;if(r!=A733_RAIL_PENDING)return -1;usb.step=1;return 0;
    }
    if(usb.step==1){
        enum a733_rail_result r=a733_rail_poll(&app.rails,&usb.rail);
        if(r==A733_RAIL_PENDING||r==A733_RAIL_BUSY)return 0;if(r!=A733_RAIL_READY)return -1;
        enum a733_pmic_status p=a733_pmic_identity(&app.pmic,&usb.power);
        if(p==A733_PMIC_BUSY)return 0;if(p!=A733_PMIC_READY)return -1;usb.step=2;return 0;
    }
    if(usb.step==2){
        enum a733_pmic_status p=a733_pmic_submit(&app.pmic,usb.authority->owner,&usb.rail,
            rail[usb.rail_index],app.deadline,&usb.request);
        if(p==A733_PMIC_BUSY)return 0;if(p!=A733_PMIC_PENDING)return -1;
        if(usb.request.id)usb.step=3;return 0;
    }
    if(usb.step==3){
        struct a733_pmic_snapshot *s=&usb.snapshots[usb.rail_index];
        enum a733_pmic_status p=a733_pmic_result(&app.pmic,&usb.request,s);
        if(p==A733_PMIC_PENDING||p==A733_PMIC_BUSY)return 0;
        if(p!=A733_PMIC_COMPLETE||s->rail!=rail[usb.rail_index]||s->page_before||s->page_after||
           s->chip!=3||s->revision||s->decoded!=1||s->enabled!=1||
           s->programmed_uv!=usb.authority->supply_uv[usb.rail_index])return -1;
        usb.step=4;return 0;
    }
    if(usb.step==4){
        enum a733_pmic_status p=a733_pmic_reap(&app.pmic,&usb.request);
        if(p==A733_PMIC_BUSY)return 0;if(p!=A733_PMIC_COMPLETE)return -1;
        usb.request=(struct a733_pmic_request){0};usb.step=++usb.rail_index==4?5:2;return 0;
    }
    enum a733_usb_platform_result r=a733_usb_platform_poll(&usb.platform);
    return r==A733_USB_PLATFORM_PREPARED?1:r==A733_USB_PLATFORM_PENDING||r==A733_USB_PLATFORM_BUSY?0:-1;
}
void a733_usb_prepare(void)
{for(;;){int r=providers();if(r<0)stop();if(r==1)return;vTaskDelay(1);}}
void a733_usb_poll_enable(bool enable){if(!owner())stop();polling=enable;}
void a733_usb_trace(uint32_t event,uint32_t a,uint32_t b)
{(void)event;(void)a;(void)b;if(!owner())stop();}
void a733_usb_assert(void){stop();}
uint32_t tusb_time_millis_api(void){return (uint32_t)(a733_smp_counter()/24000);}
void tusb_time_delay_ms_api(uint32_t ms){if(ms>150||!owner())stop();vTaskDelay(pdMS_TO_TICKS(ms));}
static const tusb_desc_device_t device={.bLength=sizeof(tusb_desc_device_t),.bDescriptorType=TUSB_DESC_DEVICE,
    .bcdUSB=0x200,.bMaxPacketSize0=64,.idVendor=0x1f3a,.idProduct=0xefe8,.bcdDevice=0x0111,
    .iManufacturer=1,.iProduct=2,.iSerialNumber=3,.bNumConfigurations=1};
static const uint8_t hs[]={TUD_CONFIG_DESCRIPTOR(1,1,0,TUD_CONFIG_DESC_LEN+TUD_VENDOR_DESC_LEN,0,100),TUD_VENDOR_DESCRIPTOR(0,4,0x02,0x81,512)};
static const uint8_t fs[]={TUD_CONFIG_DESCRIPTOR(1,1,0,TUD_CONFIG_DESC_LEN+TUD_VENDOR_DESC_LEN,0,100),TUD_VENDOR_DESCRIPTOR(0,4,0x02,0x81,64)};
static const tusb_desc_device_qualifier_t qualifier={.bLength=sizeof(tusb_desc_device_qualifier_t),.bDescriptorType=TUSB_DESC_DEVICE_QUALIFIER,.bcdUSB=0x200,.bMaxPacketSize0=64,.bNumConfigurations=1};
uint8_t const *tud_descriptor_device_cb(void){return (const uint8_t *)&device;}
uint8_t const *tud_descriptor_device_qualifier_cb(void){return (const uint8_t *)&qualifier;}
uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{return index?NULL:(tud_speed_get()==TUSB_SPEED_HIGH?hs:fs);}
uint8_t const *tud_descriptor_other_speed_configuration_cb(uint8_t index)
{static uint8_t other[sizeof(hs)];if(index)return NULL;memcpy(other,tud_speed_get()==TUSB_SPEED_HIGH?fs:hs,sizeof(hs));other[1]=TUSB_DESC_OTHER_SPEED_CONFIG;return other;}
uint16_t const *tud_descriptor_string_cb(uint8_t index,uint16_t language)
{
    (void)language;static uint16_t out[64];
    static const char *const text[]={"","sunstoneos","sunstoneos HDMI audio capture","sunstoneos-a733-hdmi-audio","Observation"};
    if(!index){out[0]=0x0304;out[1]=0x0409;return out;}
    if(index>=sizeof(text)/sizeof(*text))return NULL;
    unsigned count=(unsigned)strlen(text[index]);if(count>63)return NULL;out[0]=(uint16_t)(0x0300|2*count+2);
    for(unsigned i=0;i<count;++i)out[i+1]=(uint8_t)text[index][i];return out;
}
void tud_umount_cb(void){if(received||tx_active||observation.requests)stop();}
void tud_suspend_cb(bool wake){(void)wake;if(received||tx_active||observation.requests)stop();}
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{
    uint64_t time;
    if(!a733_counter_clock_now(&app.counter,&time)||time>=exchange_deadline)stop();
    if(interface||!owner()||!tx_active||bytes>sent-transferred||(tx_zlp&&bytes)||
       atomic_load_explicit(&observation.state,memory_order_acquire)!=3)stop();
    transferred+=bytes;
    if(transferred==sizeof(observation.reply)&&sent==sizeof(observation.reply)){
        /* Pinned vendor_device.c:916–921 calls this before deciding whether
         * to queue its automatic ZLP for an empty FIFO. Keep the copied reply
         * owned through that subsequent zero-length completion callback. */
        unsigned packet=tud_speed_get()==TUSB_SPEED_HIGH?512:64;
        if(bytes&&!(bytes&(packet-1))){tx_zlp=1;return;}
        if(!bytes&&!tx_zlp)stop();
        tx_active=tx_zlp=sent=transferred=received=0;
        atomic_store_explicit(&observation.state,0,memory_order_release);
    }
}
static void exchange(void)
{
    uint64_t time;if(!owner()||!a733_counter_clock_now(&app.counter,&time)||time>=app.deadline)stop();
    unsigned state=atomic_load_explicit(&observation.state,memory_order_acquire);
    if((received||state||tx_active)&&time>=exchange_deadline)stop();
    if(state==2){
        if(tx_active)stop();tx_active=1;sent=transferred=tx_zlp=0;
        atomic_store_explicit(&observation.state,3,memory_order_release);state=3;
    }
    if(state==3){
        if(!tx_active)stop();
        if(!sent&&tud_vendor_write_available()>=sizeof(observation.reply)){
            unsigned count=tud_vendor_write(&observation.reply,sizeof(observation.reply));
            if(count!=sizeof(observation.reply))stop();sent=count;tud_vendor_flush();
        }
        return;
    }
    if(state||!tud_mounted()||!tud_vendor_available())return;
    if(!received)exchange_deadline=app.deadline-time<1000000?app.deadline:time+1000000;
    unsigned count=tud_vendor_available();if(count>sizeof(request)-received)count=sizeof(request)-received;
    unsigned got=tud_vendor_read(request+received,count);if(got>count)stop();received+=got;
    if(received==sizeof(request)&&!hdmi_audio_observation_request(request,sizeof(request),exchange_deadline))stop();
}
void hdmi_audio_usb_task(void *unused)
{
    (void)unused;if(!owner())stop();
    for(;;){int r=prepare();if(r<0)stop();if(r==1)break;vTaskDelay(1);}
    tusb_rhport_init_t init={.role=TUSB_ROLE_DEVICE,.speed=TUSB_SPEED_HIGH};
    if(!tusb_init(0,&init))stop();
    atomic_store_explicit(&usb.ready,1,memory_order_release);
    for(;;){
        if(!owner())stop();int r=providers();if(r<0)stop();if(!r){vTaskDelay(1);continue;}
        for(unsigned i=0;i<8;++i){tud_task_ext(0,false);if(polling)dcd_int_handler(0);exchange();}
        vTaskDelay(1);
    }
}
