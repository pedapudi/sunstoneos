/* SPDX-License-Identifier: Apache-2.0 */
#include "sunstone_usb_serial.h"
#include "sunstone_usb_transport.h"
#include "sunstone_usb_service.h"
#include "tusb.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
extern bool sunstone_serial_prepare(unsigned,uint8_t,uint8_t,uint8_t);
static _Alignas(64) uint8_t pages[4][4096];
static bool used[4]; static unsigned releases, writes, connected, disconnected;
static sunstone_usb_complete completion; static void *completion_ctx;
static uint8_t *submitted; static struct ss_usb_serial_buffer published;
static int acquire(struct sunstone_usb_transport *t,uint8_t a,uint8_t e,uint8_t **p,size_t *c,uint64_t *token) {
 (void)t;(void)a;(void)e; for(unsigned i=0;i<4;i++) if(!used[i]) { used[i]=true; *p=pages[i]; *c=4096; *token=i+1; return 1; } return 0;
}
static int release(struct sunstone_usb_transport *t,uint8_t a,uint8_t e,uint64_t token) {
 (void)t;(void)a;(void)e; assert(token && token<=4 && used[token-1]); used[token-1]=false; releases++; return 1;
}
static int submit(struct sunstone_usb_transport *t,uint8_t a,uint8_t e,uint8_t *p,size_t c,sunstone_usb_complete cb,void *ctx) {
 (void)t;(void)a;(void)e; assert(c); submitted=p; completion=cb; completion_ctx=ctx; return 1;
}
bool sunstone_cdch_rx_start(uint8_t n) { (void)n; return true; }
void sunstone_cdch_control_quiesced(uint8_t n) { (void)n; }
bool sunstone_cdch_flow(uint8_t n,bool enabled,tuh_xfer_cb_t cb,uintptr_t u) { (void)n;(void)enabled;(void)cb;(void)u; return false; }
bool tuh_cdc_set_line_coding(uint8_t n,const cdc_line_coding_t *l,tuh_xfer_cb_t cb,uintptr_t u) { (void)n;(void)l;(void)cb;(void)u; return false; }
bool tuh_cdc_set_control_line_state(uint8_t n,uint16_t l,tuh_xfer_cb_t cb,uintptr_t u) { (void)n;(void)l;(void)cb;(void)u; return false; }
void os_usb_service_connected(unsigned d) { assert(d==SS_DEVICE_USB_SERIAL); connected++; }
void os_usb_service_disconnected(unsigned d) { assert(d==SS_DEVICE_USB_SERIAL); disconnected++; }
int os_usb_service_serial(const struct ss_usb_serial_buffer *b) { published=*b; return 1; }
void os_usb_service_write_complete(uint64_t seq,int result,size_t actual) { assert(seq==7 && result==1 && actual==62); writes++; }
int main(void) {
 const struct sunstone_usb_transport_ops ops={.acquire=acquire,.release=release,.submit=submit};
 struct sunstone_usb_transport t={.ops=&ops,.generation=1};
 assert(os_usb_serial_native_start(&t)==1); assert(sunstone_serial_prepare(0,1,0x81,2));
 assert(ss_usb_serial_attach(0,1,64,true)); assert(connected==1);
 uint64_t cookie; size_t cap; uint8_t *p=ss_usb_serial_rx_buffer(0,&cookie,&cap); assert(p==pages[0]);
 memset(p,0xa5,64); assert(ss_usb_serial_rx_complete(0,cookie,64,true));
 assert(published.data==p+2 && published.bytes==62);
 assert(os_usb_serial_native_write(7,published.data,published.bytes)==1); assert(submitted==p+2);
 os_usb_serial_native_release(published.token); assert(releases==0); /* OUT still owns this loan. */
 completion(completion_ctx,0,62); assert(writes==1 && releases==1);
 uint64_t old=published.token;
 p=ss_usb_serial_rx_buffer(0,&cookie,&cap); assert(p);
 ss_usb_serial_detach(0); assert(disconnected==1 && releases==1);
 assert(!sunstone_serial_prepare(0,1,0x81,2));
 os_usb_serial_native_quiesce(1); assert(releases==2);
 assert(sunstone_serial_prepare(0,1,0x81,2)); assert(ss_usb_serial_attach(0,2,64,false));
 p=ss_usb_serial_rx_buffer(0,&cookie,&cap); assert(p); assert(ss_usb_serial_rx_complete(0,cookie,4,true));
 assert(published.token>old); os_usb_serial_native_release(old); assert(releases==2);
 os_usb_serial_native_release(published.token); assert(releases==3);
 puts("serial native NC lease broker, borrowed echo, deferred release and quiescence: PASS");
}
