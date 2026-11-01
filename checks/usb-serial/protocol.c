/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include "cdc-native.c"
static tusb_control_request_t captured;
static uint8_t payload[32];
static tuh_xfer_t pending;
static unsigned completions,failures;
static void failed(tuh_xfer_t *x) { assert(x->result==XFER_RESULT_FAILED); failures++; }
bool usbh_edpt_claim(uint8_t a,uint8_t e) { (void)a;(void)e; return true; }
bool usbh_edpt_release(uint8_t a,uint8_t e) { (void)a;(void)e; return true; }
bool usbh_edpt_xfer_with_callback(uint8_t a,uint8_t e,uint8_t *b,uint16_t l,tuh_xfer_cb_t cb,uintptr_t u) { (void)a;(void)e;(void)b;(void)l;(void)cb;(void)u; return true; }
void usbh_driver_set_config_complete(uint8_t a,uint8_t i) { (void)a;(void)i; }
bool sunstone_serial_prepare(unsigned n,uint8_t a,uint8_t i,uint8_t o) { (void)n;(void)a;(void)i;(void)o; return false; }
static uint16_t vid,pid; static unsigned opened;
bool tuh_vid_pid_get(uint8_t addr,uint16_t *v,uint16_t *p) { (void)addr; *v=vid; *p=pid; return true; }
bool tuh_edpt_open(uint8_t addr,const tusb_desc_endpoint_t *ep) { (void)addr; assert(ep->wMaxPacketSize); opened++; return true; }
bool tuh_descriptor_get_device_local(uint8_t addr,tusb_desc_device_t *d) { (void)addr; memset(d,0,sizeof(*d)); d->bcdDevice=0x600; return true; }
static void recorded_descriptors(void) {
 const struct { uint16_t vid,pid; uint8_t subclass,protocol,count,in,out,packet; } facts[]={
  {0x10c4,0xea60,0,0,2,0x81,0x01,64},
  {0x1a86,0x7523,1,2,3,0x82,0x02,32},
  {0x0403,0x6001,255,255,2,0x81,0x02,64}
 };
 for(unsigned i=0;i<3;i++) {
  memset(cdch_data,0,sizeof(cdch_data)); vid=facts[i].vid; pid=facts[i].pid; opened=0;
  uint8_t desc[]={9,4,0,0,facts[i].count,255,facts[i].subclass,facts[i].protocol,0,
   7,5,facts[i].in,2,facts[i].packet,0,0,7,5,facts[i].out,2,facts[i].packet,0,0,
   7,5,0x81,3,8,0,1};
  unsigned size=9+facts[i].count*7;
  assert(cdch_open(0,1,(const tusb_desc_interface_t *)desc,(uint16_t)size)==size);
  assert(opened==facts[i].count);
 }
 memset(cdch_data,0,sizeof(cdch_data));
}
static void done(tuh_xfer_t *x) { assert(x->result==XFER_RESULT_SUCCESS); assert(x->user_data==123); completions++; }
static void complete(void) { tuh_xfer_t x=pending; tusb_control_request_t setup=captured; x.setup=&setup; x.result=XFER_RESULT_SUCCESS; x.actual_len=captured.wLength; x.complete_cb(&x); }
bool tuh_control_xfer(tuh_xfer_t *x) {
 pending=*x; captured=*x->setup; assert(captured.wLength<=sizeof(payload));
 if(captured.wLength && x->buffer) memcpy(payload,x->buffer,captured.wLength);
 return true;
}
int main(void) {
 recorded_descriptors();
 cdch_interface_t *p=&cdch_data[0]; p->daddr=1; p->bInterfaceNumber=2;
 const uint32_t rates[]={230400,460800,1000000};
 for(unsigned i=0;i<3;i++) {
  p->requested_line.coding.bit_rate=rates[i]; p->requested_line.coding.data_bits=8;
  assert(cp210x_set_baudrate(p,NULL,0)); assert(captured.bRequest==0x1e && captured.wIndex==2 && captured.wLength==4);
  uint32_t wire; memcpy(&wire,payload,4); assert(wire==rates[i]);
  p->ftdi.chip_type=FTDI_FT232R; p->ftdi.channel=0;
  assert(ftdi_set_baudrate(p,NULL,0)); assert(captured.bRequest==3);
  static const uint16_t values[]={13,0x4006,3};
  assert(captured.wValue==values[i]);
  assert(ch34x_set_baudrate(p,NULL,0)); assert(captured.bRequest==0x9a && captured.wValue==0x1312);
  static const uint16_t ch_div[]={0xe683,0xf383,0xfa83}; assert(captured.wIndex==ch_div[i]);
  p->pl2303.type=PL2303_TYPE_HX;
  assert(pl2303_set_line_coding(p,NULL,0)); assert(captured.bRequest==0x20 && captured.wLength==7);
  memcpy(&wire,payload,4); assert(wire==(i==2?0x80000180u:rates[i]));
 }
 p->serial_drid=SERIAL_DRIVER_FTDI; p->ftdi.channel=2;
 assert(sunstone_cdch_flow(0,true,done,123)); assert(captured.bRequest==2 && captured.wIndex==0x102); complete();
 p->serial_drid=SERIAL_DRIVER_CH34X;
 assert(sunstone_cdch_flow(0,true,done,123)); assert(captured.bRequest==0x9a && captured.wValue==0x2727 && captured.wIndex==0x101); complete();
 p->serial_drid=SERIAL_DRIVER_CP210X;
 assert(sunstone_cdch_flow(0,true,done,123)); assert(captured.bmRequestType==0xc1 && captured.bRequest==0x14 && captured.wLength==16);
 memset(pending.buffer,0,16); pending.buffer[8]=99; complete();
 assert(captured.bRequest==0x13 && payload[0]==9 && payload[4]==0x80 && payload[8]==99); complete();
 p->serial_drid=SERIAL_DRIVER_PL2303; p->pl2303.type=PL2303_TYPE_HX; p->pl2303.quirks=0;
 assert(sunstone_cdch_flow(0,true,done,123)); pending.buffer[0]=3; complete();
 assert(captured.bRequest==1 && captured.wValue==0 && captured.wIndex==0x63); complete();
 assert(completions==4);
 p->serial_drid=SERIAL_DRIVER_CP210X;
 assert(sunstone_cdch_flow(0,true,failed,0));
 tuh_xfer_t short_read=pending; short_read.result=XFER_RESULT_SUCCESS; short_read.actual_len=3; short_read.complete_cb(&short_read);
 assert(failures==1 && captured.bRequest==0x14); /* Never program a partial flow structure. */
 p->requested_line.coding.bit_rate=1000000; p->ftdi.chip_type=FTDI_FT2232C; p->ftdi.channel=2;
 assert(ftdi_set_baudrate(p,NULL,0)); assert(captured.wValue==3 && captured.wIndex==2);
 puts("serial upstream baud and bridge flow wire values: PASS");
}
