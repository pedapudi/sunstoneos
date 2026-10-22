#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "boot.h"
#include "a733_native_usb.h"
#include "a733_range.h"
#include "a733_usb_platform_native.h"
#include "tusb.h"
#include "device/dcd.h"
#include <string.h>

/* A diagnostic input, not an authority grant. Physical admission belongs to
 * the coordinator. Copy it before starting either provider task. */
struct usb_input {
    uint64_t magic,version,bytes,generation;
    uint32_t fabric[3],rate[3],supply_uv[4],pins[2][3];
};
static struct usb_input input;
static struct a733_native_usb shared;
static StaticTask_t tasks[2];
static StackType_t stacks[2][4096];
static uint64_t deadline,observation_deadline;
static atomic_uint pmic_stop,pmic_parked;
static atomic_uint failed;
static unsigned prepared,polling,received,sent,transferred,complete;
static uint8_t response[40];
static int live(void *unused)
{(void)unused;return !failed&&counter()/24<deadline;}
static void fail(void *unused){(void)unused;failed=1;}
static void report(const char *name,uint64_t value)
{platform_puts(name);platform_hex(value);platform_puts("\n");}
static _Noreturn void retained(const char *reason)
{
    failed=1;platform_puts("INCOMPLETE USB: ");platform_puts(reason);platform_puts("\n");
    report("USB_STEP=",shared.usb_step);report("PMIC_STAGE=",shared.pmic.stage);
    report("USB_STAGE=",shared.usb.stage);
    const struct a733_usb_failure *fault=&shared.usb.failure;
    report("USB_FAULT_KIND=",fault->kind);report("USB_FAULT_STAGE=",fault->stage);
    report("USB_FAULT_STEP=",fault->step);report("USB_FAULT_WROTE=",fault->wrote);
    report("USB_FAULT_ADDRESS=",fault->address);report("USB_FAULT_MASK=",fault->mask);
    report("USB_FAULT_EXPECTED=",fault->expected);report("USB_FAULT_OBSERVED=",fault->observed);
    report("USB_FAULT_OBSERVED_VALID=",fault->observed_valid);
    report("USB_FAULT_CALLBACK_RESULT=",(uint64_t)(int64_t)fault->callback_result);
    report("USB_FAULT_WRITES_ATTEMPTED=",fault->writes_attempted);
    report("USB_FAULT_WRITES_COMPLETED=",fault->writes_completed);
    report("USB_FAULT_LAST_US=",fault->last_us);report("USB_FAULT_SAMPLE_US=",fault->sample_us);
    report("USB_FAULT_DEADLINE_US=",fault->deadline_us);
    report("USB_IO_DENIAL=",shared.usb_io_denial);
    report("USB_IO_PROVIDER_STATUS=",(uint64_t)(int64_t)shared.usb_io_provider_status);
    report("USB_PROVIDER_DENIAL=",shared.usb_provider_denial);
    report("USB_PROVIDER_RESULT=",(uint64_t)(int64_t)shared.usb_provider_result);
    report("PMIC_FAILED=",shared.pmic.failed);report("PMIC_HELD=",shared.pmic.held);
    report("PMIC_RETAINED=",shared.pmic.retained);
    report("USB_RECEIVED=",received);
    report("USB_SENT=",sent);report("USB_TRANSFERRED=",transferred);
    /* No lease release, resource restoration or successful-return marker. */
    platform_exit(1);
}
static int quiet(void)
{
    /* USB0 device/EHCI/OHCI INTIDs188..190, distributor bank5 bits28..30. */
    return !((mmio_read(0x03400114)|mmio_read(0x03400214)|mmio_read(0x03400314))&0x70000000);
}
static int irq_excluded(void)
{return !((mmio_read(0x03400114)|mmio_read(0x03400314))&0x70000000);}
static int no_dma(void)
{
    /* DEVCTL HOSTREQ/HOSTMODE exclude a host actor; SESSION is also
     * meaningful in peripheral mode and does not enable DMA. */
    if(mmio_read(0x04100040)&0x600)return 0;
    for(unsigned i=0;i<16;++i)if(mmio_read(0x04100540+i*16)&0x80000000)return 0;
    return 1;
}
static int clear(uintptr_t address,uint32_t mask)
{
    uint32_t value;
    return a733_usb_platform_native_read(NULL,address,&value)&&
        a733_usb_platform_native_write(NULL,address,(value&~mask)&~(address==0x04100400?0x70u:0u))&&
        a733_usb_platform_native_read(NULL,address,&value)&&!(value&mask);
}
/* Called only by the USB task after the last TinyUSB poll. This prevents
 * future native PIO accesses. The lifetime must have used PIO throughout;
 * clear DMA bits alone cannot establish drain after DMA cancellation. */
static int quiesce(void)
{
    if(!prepared||!pmic_parked||!irq_excluded()||!no_dma()||shared.pmic.held||shared.pmic.retained||
       shared.pmic.failed||shared.pmic.active!=A733_PMIC_REQUESTS||
       atomic_load_explicit(&shared.locks.retained,memory_order_acquire))return 0;
    for(unsigned i=0;i<A733_PMIC_REQUESTS;++i)
        if(shared.pmic.slots[i].state||shared.pmic.slots[i].borrow.id)return 0;
    if(xSemaphoreGetMutexHolder(shared.locks.mutex[0])||
       xSemaphoreGetMutexHolder(shared.locks.mutex[1]))return 0;
    polling=0;
    if(!clear(0x04100048,UINT32_MAX)||!clear(0x04100050,255)||
       !clear(0x04100500,65535)||!clear(0x04100400,15)||
       !clear(0x04100040,64))return 0;
    vTaskDelay(pdMS_TO_TICKS(150));
    /* Hardware remains powered, owned and detached until the monitor resets
     * the CPU and the reviewed forced-FEL path initializes USB0 anew. */
    return quiet()&&no_dma()&&!(mmio_read(0x04100040)&64)&&
        !mmio_read(0x04100048)&&!(mmio_read(0x04100050)&255)&&
        !(mmio_read(0x04100500)&65535)&&!(mmio_read(0x04100400)&15);
}
void a733_usb_prepare(void)
{for(;;){int result=a733_native_usb_health(&shared);
 if(!prepared||result<0||!live(NULL))retained("provider health");
 if(result==1)return;vTaskDelay(1);}}
void a733_usb_poll_enable(bool enable){polling=enable;}
void a733_usb_trace(uint32_t event,uint32_t a,uint32_t b){(void)event;(void)a;(void)b;}
void a733_usb_assert(void){retained("TinyUSB assertion");}
uint32_t tusb_time_millis_api(void){return (uint32_t)(counter()/24000);}
void tusb_time_delay_ms_api(uint32_t ms){if(ms>150)retained("USB delay");vTaskDelay(pdMS_TO_TICKS(ms));}
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
{(void)language;static uint16_t out[64];static const char *const text[]={"","sunstoneos","sunstoneos returning USB","sunstoneos-a733-usb","Observe"};
 if(!i){out[0]=0x0304;out[1]=0x0409;return out;}if(i>=5)return NULL;size_t n=strlen(text[i]);out[0]=(uint16_t)(0x0300|2*n+2);for(size_t j=0;j<n;++j)out[j+1]=(uint8_t)text[i][j];return out;}
void tud_mount_cb(void){}
void tud_umount_cb(void){if(received&&!complete)failed=1;}
void tud_suspend_cb(bool wake){(void)wake;}
void tud_vendor_rx_cb(uint8_t interface,const uint8_t *data,uint32_t bytes)
{(void)data;(void)bytes;if(interface)failed=1;}
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{if(interface||bytes>sent-transferred){failed=1;return;}transferred+=bytes;
 if(transferred==sizeof(response)&&sent==sizeof(response))complete=1;}
void tud_vendor_tx_complete_cb(uint8_t interface){if(interface)failed=1;}
static void exchange(void)
{
    if(received){if(sent<sizeof(response)){sent+=tud_vendor_write(response+sent,sizeof(response)-sent);tud_vendor_flush();}return;}
    unsigned count=tud_vendor_available();if(!count)return;
    uint8_t request[4];if(count!=4||tud_vendor_read(request,4)!=4||
        memcmp(request,"USB?",4)){failed=1;return;}
    received=1;
    const uint64_t words[5]={UINT64_C(0x3154455253425355),input.generation,1,shared.usb_step,shared.pmic.stage};
    memcpy(response,words,sizeof(words));
}
static void pmic_task(void *unused)
{
    (void)unused;for(;;){if(pmic_stop){pmic_parked=1;for(;;)vTaskDelay(1);}
        enum a733_pmic_status r=a733_pmic_poll(&shared.pmic);
        if(r==A733_PMIC_READY||r==A733_PMIC_COMPLETE)
            atomic_store_explicit(&shared.pmic_ready,1,memory_order_release);
        else if(r!=A733_PMIC_PENDING&&r!=A733_PMIC_BUSY)failed=1;
        vTaskDelay(1);}
}
static void usb_task(void *unused)
{
    (void)unused;for(;;){if(!live(NULL))retained("startup deadline/provider");
        int r=a733_native_usb_step(&shared);if(r<0)retained("USB preparation");
        if(r==1)break;vTaskDelay(1);}
    prepared=1;
    tusb_rhport_init_t init={.role=TUSB_ROLE_DEVICE,.speed=TUSB_SPEED_HIGH};
    if(!tusb_init(0,&init))retained("TinyUSB init");
    while(live(NULL)&&counter()/24<observation_deadline&&!complete){tud_task();a733_usb_prepare();if(polling)dcd_int_handler(0);exchange();vTaskDelay(1);}
    if(failed)retained("transport/provider");
    unsigned outcome=complete?0:1;
    pmic_stop=1;while(!pmic_parked){if(!live(NULL))retained("PMIC park deadline");vTaskDelay(1);}
    if(!quiesce()||!live(NULL))retained("quiescence");
    report("USB_OBSERVATION_OUTCOME=",outcome);report("USB_RECEIVED=",received);
    report("USB_SENT=",sent);report("USB_TRANSFERRED=",transferred);
    report("USB_QUIESCED=",1);
    platform_puts("COMPLETE native USB bounded observation\n");platform_exit(0);
}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name)
{(void)task;platform_panic(name,0);}
void kernel_main(void)
{
    memcpy(&input,(const void *)0x43000000,sizeof(input));platform_init();
    if(input.magic!=UINT64_C(0x31504e4953425355)||input.version!=1||
       input.bytes!=sizeof(input)||!input.generation)retained("input");
    uint64_t start=counter()/24;if(!a733_deadline_after(start,1800000,&deadline))retained("counter");observation_deadline=start+1400000;
    struct a733_native_usb_config config={.identity=input.generation};
    config.pmic.owner=2;config.pmic.identity=input.generation;config.pmic.startup_deadline=deadline;
    for(unsigned i=0;i<2;++i)config.pmic.pins[i]=(struct a733_pin_config){input.pins[i][0],input.pins[i][1],input.pins[i][2]};
    config.pins.authorized[A733_PIN_L]=3;
    config.usb.owner=3;config.usb.handoff=input.generation;config.usb.deadline_us=deadline;
    config.usb.cold_reference=A733_USB_ROM_DEVICE;
    memcpy(config.usb.fabric,input.fabric,sizeof(input.fabric));memcpy(config.usb.rate_hz,input.rate,sizeof(input.rate));
    memcpy(config.usb_supply_uv,input.supply_uv,sizeof(input.supply_uv));
    if(!quiet()||!a733_native_usb_init(&shared,&config,live,fail,NULL))retained("providers init");
    TaskHandle_t pmic=xTaskCreateStatic(pmic_task,"usb-pmic",4096,NULL,2,stacks[0],&tasks[0]);
    TaskHandle_t usb=xTaskCreateStatic(usb_task,"usb-observe",4096,NULL,2,stacks[1],&tasks[1]);
    if(!pmic||!usb||!a733_native_usb_bind(&shared,pmic,usb))retained("providers bind");
    vTaskStartScheduler();platform_panic("scheduler returned",0);
}
