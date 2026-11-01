/* SPDX-License-Identifier: Apache-2.0 */
#include "sunstone_usb_serial.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static _Alignas(64) uint8_t memory[SS_USB_SERIAL_SLOTS][SS_USB_SERIAL_BUFFER];
static uint32_t word(FILE *f) {
 uint8_t b[4]; assert(fread(b,1,4,f)==4);
 return (uint32_t)b[0]|(uint32_t)b[1]<<8|(uint32_t)b[2]<<16|(uint32_t)b[3]<<24;
}
int main(int argc,char **argv) {
 assert(argc==3);
 FILE *input=fopen(argv[1],"rb"), *output=fopen(argv[2],"wb"); assert(input && output);
 uint8_t magic[8]; assert(fread(magic,1,8,input)==8 && !memcmp(magic,"SSUSBCP1",8));
 uint32_t packet=word(input), records=word(input); assert(packet==512 && records);
 uint8_t *buffers[SS_USB_SERIAL_SLOTS];
 for(unsigned i=0;i<SS_USB_SERIAL_SLOTS;i++) buffers[i]=memory[i];
 assert(ss_usb_serial_bind(0,buffers)); assert(ss_usb_serial_attach(0,1,(uint16_t)packet,true));
 size_t payload=0;
 for(uint32_t record=0;record<records;record++) {
  uint32_t bytes=word(input); assert(bytes>=2 && bytes<=SS_USB_SERIAL_BUFFER);
  uint64_t cookie; size_t capacity;
  uint8_t *p=ss_usb_serial_rx_buffer(0,&cookie,&capacity); assert(p && bytes<=capacity);
  assert(fread(p,1,bytes,input)==bytes); assert(ss_usb_serial_rx_complete(0,cookie,bytes,true));
  struct ss_usb_serial_view view;
  while(ss_usb_serial_peek(0,&view)) {
   /* The returned application view must still point into the submitted page. */
   assert(view.data>=p && view.data+view.size<=p+bytes && view.size<=packet-2);
   assert(fwrite(view.data,1,view.size,output)==view.size); payload+=view.size;
   assert(ss_usb_serial_rx_release(0,view.token));
  }
 }
 assert(fgetc(input)==EOF); assert(!fclose(input) && !fclose(output));
 struct ss_usb_serial_stats stats; assert(ss_usb_serial_stats(0,&stats));
 assert(!stats.errors && !stats.stale && !stats.dropped && !stats.backpressure);
 printf("Replayed %u captured FTDI IN completions; delivered %zu payload bytes without copying views.\n",records,payload);
}
