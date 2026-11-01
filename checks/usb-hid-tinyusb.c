/* Exercise pinned TinyUSB HID host enumeration, receive, short input and close.
 * Host endpoint/control completion is modeled; no USB device is accessed. */
#define main parser_unit_main
#include "usb-hid.c"
#undef main
#include "tusb.h"
#include "host/usbh_pvt.h"
static struct sunstone_usb_hid decoded;
static uint8_t enum_buffer[1024],*pending_buffer;
static unsigned pending_bytes,configured,protocol_sets;
uint8_t *usbh_get_enum_buf(void){return enum_buffer;}
bool tuh_edpt_open(uint8_t address,const tusb_desc_endpoint_t *ep)
{assert(address==2&&ep->bEndpointAddress==0x81);return true;}
void usbh_driver_set_config_complete(uint8_t address,uint8_t itf)
{assert(address==2&&itf==0);++configured;}
bool tuh_control_xfer(tuh_xfer_t *x)
{
 assert(x->daddr==2&&x->setup->wIndex==0);
 if(x->setup->bRequest==HID_REQ_CONTROL_SET_PROTOCOL){assert(x->setup->wValue==HID_PROTOCOL_REPORT);++protocol_sets;}
 else assert(x->setup->bRequest==HID_REQ_CONTROL_SET_IDLE);
 x->result=XFER_RESULT_SUCCESS;x->actual_len=0;x->complete_cb(x);return true;
}
bool tuh_descriptor_get_hid_report(uint8_t address,uint8_t itf,uint8_t type,uint8_t index,
 void *buffer,uint16_t bytes,tuh_xfer_cb_t cb,uintptr_t user)
{
 assert(address==2&&itf==0&&type==0x22&&index==0&&bytes==sizeof keyboard);
 memcpy(buffer,keyboard,bytes);tusb_control_request_t setup={.bRequest=6,.wIndex=itf,.wLength=bytes};
 tuh_xfer_t x={.daddr=address,.setup=&setup,.buffer=buffer,.result=XFER_RESULT_SUCCESS,.actual_len=bytes,.complete_cb=cb,.user_data=user};
 cb(&x);return true;
}
bool usbh_edpt_claim(uint8_t address,uint8_t ep){assert(address==2&&ep==0x81);return !pending_buffer;}
bool usbh_edpt_release(uint8_t address,uint8_t ep){assert(address==2&&ep==0x81);return true;}
bool usbh_edpt_xfer_with_callback(uint8_t address,uint8_t ep,uint8_t *buffer,uint16_t bytes,
 tuh_xfer_cb_t cb,uintptr_t user)
{assert(address==2&&ep==0x81&&!pending_buffer&&!cb&&!user);pending_buffer=buffer;pending_bytes=bytes;return true;}
void tuh_hid_mount_cb(uint8_t address,uint8_t instance,const uint8_t *d,uint16_t bytes)
{assert(address==2&&!instance&&tuh_hid_get_protocol(address,instance)==HID_PROTOCOL_REPORT);assert(mount(&decoded,d,bytes));}
void tuh_hid_report_received_cb(uint8_t address,uint8_t instance,const uint8_t *p,uint16_t bytes)
{assert(address==2&&!instance);assert(sunstone_usb_hid_report(&decoded,11,p,bytes)==(bytes==8?1:0));}
void tuh_hid_umount_cb(uint8_t address,uint8_t instance)
{assert(address==2&&!instance);assert(sunstone_usb_hid_unmount(&decoded,11));}
int main(void)
{
 uint8_t d[]={9,4,0,0,1,3,1,1,0,9,0x21,0x11,1,0,1,0x22,sizeof keyboard,0,7,5,0x81,3,8,0,10};
 assert(hidh_init());tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);
 assert(hidh_open(0,2,(const tusb_desc_interface_t *)d,sizeof d)==sizeof d);
 assert(hidh_set_config(2,0)&&configured==1&&protocol_sets==1&&decoded.mounted);
 assert(tuh_hid_receive_report(2,0)&&pending_bytes==8);memset(pending_buffer,0,8);pending_buffer[2]=4;
 pending_buffer=NULL;assert(hidh_xfer_cb(2,0x81,XFER_RESULT_SUCCESS,8));expect(1,SUNSTONE_HID_KEY,4,1);
 assert(tuh_hid_receive_report(2,0));pending_buffer=NULL;
 assert(hidh_xfer_cb(2,0x81,XFER_RESULT_SUCCESS,1));assert(count==2); /* Short report cannot release key. */
 hidh_close(2);expect(2,SUNSTONE_HID_KEY,4,0);expect(3,SUNSTONE_HID_DETACH,1,0);
 puts("Pinned TinyUSB HID class report protocol, receive, short report and detach passed");return 0;
}
