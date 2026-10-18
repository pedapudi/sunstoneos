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
#ifdef SUNSTONE_BLUETOOTH_HEADSET
#include "headset.h"
#endif
extern struct bluetooth_application bluetooth;
const uint8_t *tud_descriptor_device_cb(void)
{static tusb_desc_device_t descriptor;descriptor=device;descriptor.idProduct=0xefe8;return (const uint8_t *)&descriptor;}
const uint16_t *tud_descriptor_string_cb(uint8_t i,uint16_t language)
{(void)language;static uint16_t value[64];static const char *const names[]={"","sunstoneos","sunstoneos Bluetooth HCI","sunstoneos-a733-bluetooth","Capture"};
 if(!i){value[0]=0x0304;value[1]=0x0409;return value;}if(i>=5)return NULL;
 size_t n=strlen(names[i]);value[0]=(uint16_t)(0x0300+2*n+2);for(size_t j=0;j<n;++j)value[j+1]=(uint8_t)names[i][j];return value;}
static uint8_t identity_frame[1232];
static unsigned identity_bytes,identity_sent,identity_transferred;
#ifdef SUNSTONE_BLUETOOTH_HEADSET
static void put64(uint8_t *p,uint64_t x){for(unsigned i=0;i<8;++i)p[i]=(uint8_t)(x>>(8*i));}
#ifdef SUNSTONE_BLUETOOTH_RECOGNITION
static void recognition_exchange(const uint8_t request[8])
{
    size_t bytes=request[2]|(unsigned)request[3]<<8;
    uint32_t offset=request[4]|(uint32_t)request[5]<<8|(uint32_t)request[6]<<16|(uint32_t)request[7]<<24;
    if(request[1]||!bytes||bytes>1024||bluetooth.profile->version!=3)stop();
    struct bluetooth_recognition_snapshot s;size_t copied;uint8_t text[1024];
    int r=bluetooth_recognition_copy(&bluetooth,offset,bytes,text,&copied,&s);
    memset(identity_frame,0,sizeof(identity_frame));
    put32(identity_frame,0x52544242);put32(identity_frame+4,1);put32(identity_frame+8,r<0?2:r?1:0);
    put32(identity_frame+12,r==1?(uint32_t)copied:0);
    put64(identity_frame+16,board.session.epoch);put64(identity_frame+24,board.session.generation);
    memcpy(identity_frame+32,board.session.sid_evidence,32);
    if(r==1){put64(identity_frame+64,s.firmware);put64(identity_frame+72,s.uart);
        put64(identity_frame+80,s.acl);put64(identity_frame+88,s.audio);
        put32(identity_frame+96,s.bytes);put32(identity_frame+100,s.input_samples);put32(identity_frame+104,s.output_samples);
        memcpy(identity_frame+112,s.pcm_sha256,32);memcpy(identity_frame+144,s.wav_sha256,32);memcpy(identity_frame+176,s.text_sha256,32);
        memcpy(identity_frame+208,text,copied);}
    identity_sent=identity_transferred=0;identity_bytes=(unsigned)(208+(r==1?copied:0));
}
#endif
static void pcm_exchange(const uint8_t request[8])
{
#ifdef SUNSTONE_BLUETOOTH_RECOGNITION
    if(request[0]==3){recognition_exchange(request);return;}
#endif
    unsigned kind=request[0]-1;size_t bytes=request[2]|(unsigned)request[3]<<8;
    uint32_t offset=request[4]|(uint32_t)request[5]<<8|(uint32_t)request[6]<<16|(uint32_t)request[7]<<24;
    if(kind>1||request[1]||!bytes||bytes>1024||bluetooth.profile->version<2)stop();
    struct bluetooth_pcm_snapshot s;size_t copied;uint8_t pcm[1024];
    int r=bluetooth_headset_copy(&bluetooth,kind,offset,bytes,pcm,&copied,&s);
    memset(identity_frame,0,sizeof(identity_frame));
    put32(identity_frame,0x4d435442);put32(identity_frame+4,1);put32(identity_frame+8,r<0?2:r?1:0);
    put32(identity_frame+12,r==1?(uint32_t)copied:0);
    put64(identity_frame+16,board.session.epoch);put64(identity_frame+24,board.session.generation);
    memcpy(identity_frame+32,board.session.sid_evidence,32);
    if(r==1){put64(identity_frame+64,s.firmware);put64(identity_frame+72,s.uart);
        put64(identity_frame+80,s.acl);put64(identity_frame+88,s.audio);
        put32(identity_frame+96,s.bytes);put32(identity_frame+100,s.records);
        put32(identity_frame+104,s.loss_packets);put32(identity_frame+108,kind);put32(identity_frame+112,offset);
        memcpy(identity_frame+116,s.sha256,32);memcpy(identity_frame+160,pcm,copied);}
    identity_sent=identity_transferred=0;identity_bytes=(unsigned)(160+(r==1?copied:0));
}
#endif
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{if(interface||bytes>identity_sent-identity_transferred)stop();identity_transferred+=bytes;
 if(identity_transferred==identity_bytes&&identity_sent==identity_bytes)identity_bytes=0;}
static void identity_exchange(void)
{
    if(identity_bytes){identity_sent+=tud_vendor_write(identity_frame+identity_sent,identity_bytes-identity_sent);tud_vendor_flush();return;}
    if(!tud_vendor_available())return;
    unsigned available=tud_vendor_available();
#ifdef SUNSTONE_BLUETOOTH_HEADSET
    if(available==8){uint8_t request[8];if(tud_vendor_read(request,8)!=8)stop();pcm_exchange(request);return;}
#endif
    uint8_t request;if(available!=1||tud_vendor_read(&request,1)!=1||request)stop();
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
