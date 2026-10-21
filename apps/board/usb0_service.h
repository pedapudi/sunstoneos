/* Shared USB0 vendor device service for native board applications.
 *
 * One FreeRTOS task owns TinyUSB, the polled sunxi DCD and one vendor
 * interface (bulk OUT 0x02, bulk IN 0x81). Include this header once from the
 * application's USB translation unit after defining its identity and owner:
 *   USB0_PID, USB0_PRODUCT, USB0_SERIAL   required
 *   USB0_LIVE(), USB0_PROVIDERS          required
 *   USB0_BCD (0x0100), USB0_INTERFACE ("Capture")   optional
 * and a static _Noreturn stop(void) carrying the application's own failure
 * and retention policy. The header supplies the descriptors, the TinyUSB
 * platform hooks, provider startup and usb0_service_run().
 *
 * Include it only from images whose build identity hashes the apps/board
 * headers (board_inputs and the board single-core glob); in any other image a
 * change here would not change the build ID.
 *
 * The service burst repeats one pass of TinyUSB, controller polling and the
 * application exchange while a pass makes progress: a queued controller event,
 * or a changed vendor RX/TX FIFO level. After progress it keeps polling for one
 * high-speed microframe so the host can collect a written packet or send the
 * next one. A burst stops after 200 us of counter time or 64 passes and then
 * yields one tick, so same-core work keeps its share and an idle or abandoned
 * host costs one pass per tick. The response protocol and its incremental
 * write/transfer accounting stay in the application. */
#ifndef SUNSTONE_USB0_SERVICE_H
#define SUNSTONE_USB0_SERVICE_H
#include "tusb.h"
#include "device/dcd.h"
#include <stdint.h>
#include <string.h>
#ifndef USB0_BCD
#define USB0_BCD 0x0100
#endif
#ifndef USB0_INTERFACE
#define USB0_INTERFACE "Capture"
#endif
#define USB0_SERVICE_BUDGET 4800u /* 200 us of the 24 MHz counter */
#define USB0_SERVICE_IDLE 3000u   /* 125 us: one high-speed microframe */
#define USB0_SERVICE_PASSES 64u
uint64_t a733_smp_counter(void);

static const tusb_desc_device_t usb0_device={.bLength=sizeof(tusb_desc_device_t),.bDescriptorType=TUSB_DESC_DEVICE,
    .bcdUSB=0x200,.bMaxPacketSize0=64,.idVendor=0x1f3a,.idProduct=USB0_PID,.bcdDevice=USB0_BCD,
    .iManufacturer=1,.iProduct=2,.iSerialNumber=3,.bNumConfigurations=1};
static const uint8_t usb0_hs[]={TUD_CONFIG_DESCRIPTOR(1,1,0,TUD_CONFIG_DESC_LEN+TUD_VENDOR_DESC_LEN,0,100),TUD_VENDOR_DESCRIPTOR(0,4,0x02,0x81,512)};
static const uint8_t usb0_fs[]={TUD_CONFIG_DESCRIPTOR(1,1,0,TUD_CONFIG_DESC_LEN+TUD_VENDOR_DESC_LEN,0,100),TUD_VENDOR_DESCRIPTOR(0,4,0x02,0x81,64)};
static const tusb_desc_device_qualifier_t usb0_qualifier={.bLength=sizeof(tusb_desc_device_qualifier_t),.bDescriptorType=TUSB_DESC_DEVICE_QUALIFIER,.bcdUSB=0x200,.bMaxPacketSize0=64,.bNumConfigurations=1};
const uint8_t *tud_descriptor_device_cb(void){return (const uint8_t *)&usb0_device;}
const uint8_t *tud_descriptor_device_qualifier_cb(void){return (const uint8_t *)&usb0_qualifier;}
const uint8_t *tud_descriptor_configuration_cb(uint8_t i){return i?NULL:tud_speed_get()==TUSB_SPEED_HIGH?usb0_hs:usb0_fs;}
const uint8_t *tud_descriptor_other_speed_configuration_cb(uint8_t i)
{static uint8_t other[sizeof(usb0_hs)];if(i)return NULL;memcpy(other,tud_speed_get()==TUSB_SPEED_HIGH?usb0_fs:usb0_hs,sizeof(usb0_hs));other[1]=TUSB_DESC_OTHER_SPEED_CONFIG;return other;}
const uint16_t *tud_descriptor_string_cb(uint8_t i,uint16_t language)
{
    (void)language;static uint16_t out[64];
    static const char *const text[]={"","sunstoneos",USB0_PRODUCT,USB0_SERIAL,USB0_INTERFACE};
    if(!i){out[0]=0x0304;out[1]=0x0409;return out;}
    if(i>=sizeof(text)/sizeof(*text))return NULL;
    size_t n=strlen(text[i]);if(n>63)return NULL;out[0]=(uint16_t)(0x0300|2*n+2);
    for(size_t j=0;j<n;++j)out[j+1]=(uint8_t)text[i][j];return out;
}

/* One pass reports progress when it consumed or produced controller events or
 * moved bytes through either vendor FIFO. */
static inline int usb0_service_progress(void (*pass)(void))
{
    int pending=tud_task_event_ready();uint32_t rx=tud_vendor_available(),tx=tud_vendor_write_available();
    pass();
    return pending||tud_task_event_ready()||rx!=tud_vendor_available()||tx!=tud_vendor_write_available();
}
static inline void usb0_service_burst(void (*pass)(void))
{
    uint64_t start=a733_smp_counter(),active=start;int moved=0;
    for(unsigned i=0;i<USB0_SERVICE_PASSES;++i){
        int progress=usb0_service_progress(pass);uint64_t now=a733_smp_counter();
        if(progress){moved=1;active=now;}
        if(!moved||now-start>=USB0_SERVICE_BUDGET||now-active>=USB0_SERVICE_IDLE)return;
    }
}

static unsigned polling;
static void (*usb0_service_exchange)(void);
void a733_usb_prepare(void)
{for(;;){int r=a733_native_usb_health(USB0_PROVIDERS);if(r<0||!USB0_LIVE())stop();if(r==1)return;vTaskDelay(1);}}
void a733_usb_poll_enable(bool enable){if(!USB0_LIVE())stop();polling=enable;}
void a733_usb_trace(uint32_t event,uint32_t a,uint32_t b)
{(void)event;(void)a;(void)b;if(!USB0_LIVE())stop();}
void a733_usb_assert(void){stop();}
uint32_t tusb_time_millis_api(void){return (uint32_t)(a733_smp_counter()/24000);}
void tusb_time_delay_ms_api(uint32_t ms){if(ms>150||!USB0_LIVE())stop();vTaskDelay(pdMS_TO_TICKS(ms));}
/* Acquire the USB rails and controller through the shared providers, then
 * start the device stack. Any provider or stack failure takes stop(). */
static inline void usb0_service_start(void)
{
    for(;;){if(!USB0_LIVE())stop();int r=a733_native_usb_step(USB0_PROVIDERS);if(r<0)stop();if(r==1)break;vTaskDelay(1);}
    tusb_rhport_init_t init={.role=TUSB_ROLE_DEVICE,.speed=TUSB_SPEED_HIGH};
    if(!tusb_init(0,&init))stop();
}
/* Liveness and provider health are rechecked on every pass. */
static void usb0_service_pass(void)
{if(!USB0_LIVE())stop();tud_task_ext(0,false);a733_usb_prepare();if(polling)dcd_int_handler(0);usb0_service_exchange();}
static inline _Noreturn void usb0_service_run(void (*exchange)(void))
{usb0_service_exchange=exchange;for(;;){usb0_service_burst(usb0_service_pass);vTaskDelay(1);}}
#endif
