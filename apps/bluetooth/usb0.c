/* Reuse the actual USB0 provider/task and descriptor mechanics, with an
 * application-specific copied result exchange and device identity. */
#define board_usb0_task bluetooth_usb0_base_task
#define exchange bluetooth_unused_exchange
#define tud_descriptor_device_cb bluetooth_unused_device_descriptor
#define tud_descriptor_string_cb bluetooth_unused_string_descriptor
#define tud_vendor_tx_cb bluetooth_unused_tx
#include "bluetooth-usb-base.c"
#undef board_usb0_task
#undef exchange
#undef tud_descriptor_device_cb
#undef tud_descriptor_string_cb
#undef tud_vendor_tx_cb
#include "bluetooth.h"
#include "result.h"
extern struct bluetooth_application bluetooth;
const uint8_t *tud_descriptor_device_cb(void)
{static tusb_desc_device_t descriptor;descriptor=device;descriptor.idProduct=0xefe8;return (const uint8_t *)&descriptor;}
const uint16_t *tud_descriptor_string_cb(uint8_t i,uint16_t language)
{(void)language;static uint16_t value[64];static const char *const names[]={"","sunstoneos","sunstoneos Bluetooth HCI","sunstoneos-a733-bluetooth","Capture"};
 if(!i){value[0]=0x0304;value[1]=0x0409;return value;}if(i>=5)return NULL;
 size_t n=strlen(names[i]);value[0]=(uint16_t)(0x0300+2*n+2);for(size_t j=0;j<n;++j)value[j+1]=(uint8_t)names[i][j];return value;}
static uint8_t identity_frame[177];
static unsigned identity_bytes,identity_sent,identity_transferred;
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{if(interface||bytes>identity_sent-identity_transferred)stop();identity_transferred+=bytes;
 if(identity_transferred==identity_bytes&&identity_sent==identity_bytes)identity_bytes=0;}
static void identity_exchange(void)
{
    if(identity_bytes){identity_sent+=tud_vendor_write(identity_frame+identity_sent,identity_bytes-identity_sent);tud_vendor_flush();return;}
    if(!tud_vendor_available())return;
    uint8_t request;if(tud_vendor_available()!=1||tud_vendor_read(&request,1)!=1||request)stop();
    struct bt_hci_identity id;int result=bluetooth_application_result(&bluetooth,&id);
    size_t length;
    if(!bluetooth_result_frame(identity_frame,&length,result<0?2:result?1:0,board.session.epoch,
        board.session.generation,board.session.sid_evidence,result==1?bluetooth.firmware_generation:0,
        result==1?bluetooth.uart_generation:0,result==1?&id:NULL))stop();
    identity_sent=identity_transferred=0;identity_bytes=(unsigned)length;
}
void board_usb0_task(void *unused)
{
    (void)unused;for(;;){if(!board_live(NULL))stop();int r=a733_native_usb_step(&providers.shared);
        if(r<0)stop();if(r==1)break;vTaskDelay(1);}
    tusb_rhport_init_t init={.role=TUSB_ROLE_DEVICE,.speed=TUSB_SPEED_HIGH};if(!tusb_init(0,&init))stop();
    for(;;){if(!board_live(NULL))stop();tud_task();a733_usb_prepare();if(polling)dcd_int_handler(0);identity_exchange();vTaskDelay(1);}
}
