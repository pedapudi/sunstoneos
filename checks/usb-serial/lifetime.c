/* SPDX-License-Identifier: Apache-2.0 */
#include "sunstone_usb_serial.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static _Alignas(64) uint8_t memory[4][4096];
int main(void) {
 uint8_t *buffers[]={memory[0],memory[1],memory[2],memory[3]};
 assert(ss_usb_serial_bind(0,buffers)); assert(ss_usb_serial_attach(0,1,64,true));
 uint64_t cookie; size_t size; struct ss_usb_serial_view view;
 uint8_t *p=ss_usb_serial_rx_buffer(0,&cookie,&size); assert(p && size==4096);
 memset(p,0x55,130); p[0]=0x11; p[1]=0x22; p[64]=0x33; p[65]=0x44; p[128]=0x66; p[129]=0x77;
 assert(ss_usb_serial_rx_complete(0,cookie,130,true));
 assert(ss_usb_serial_peek(0,&view) && view.data==p+2 && view.size==62);
 uint64_t first=view.token; assert(!ss_usb_serial_peek(0,&view));
 assert(ss_usb_serial_rx_release(0,first));
 assert(ss_usb_serial_peek(0,&view) && view.data==p+66 && view.size==62);
 assert(!ss_usb_serial_rx_release(0,first)); assert(ss_usb_serial_rx_release(0,view.token));
 assert(!ss_usb_serial_peek(0,&view)); /* final packet contains only FTDI status */
 for(unsigned i=0;i<4;i++) { assert(ss_usb_serial_rx_buffer(0,&cookie,&size)); assert(ss_usb_serial_rx_complete(0,cookie,64,true)); }
 assert(!ss_usb_serial_rx_buffer(0,&cookie,&size));
 assert(ss_usb_serial_peek(0,&view));
 ss_usb_serial_detach(0); assert(!ss_usb_serial_attach(0,2,64,false));
 assert(ss_usb_serial_rx_release(0,view.token)); assert(ss_usb_serial_attach(0,2,64,false));
 p=ss_usb_serial_rx_buffer(0,&cookie,&size); assert(p);
 ss_usb_serial_detach(0); assert(!ss_usb_serial_attach(0,3,64,false));
 ss_usb_serial_quiesced(0); assert(ss_usb_serial_attach(0,3,64,false));
 assert(!ss_usb_serial_rx_complete(0,cookie,64,true));
 uint64_t fresh; assert(ss_usb_serial_rx_buffer(0,&fresh,&size)); assert(fresh!=cookie);
 assert(!ss_usb_serial_rx_complete(0,cookie,64,true)); assert(ss_usb_serial_rx_complete(0,fresh,64,true));
 struct ss_usb_serial_stats stats; assert(ss_usb_serial_stats(0,&stats));
 assert(stats.backpressure==1 && stats.stale==2 && stats.dropped==192);
 puts("serial packet views, backpressure, stale completion and disconnect lifetime: PASS");
}
