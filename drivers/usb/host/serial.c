/* SPDX-License-Identifier: Apache-2.0 */
#include "sunstone_usb_serial.h"
#include <string.h>
enum { FREE, DMA, READY, LEASED, RETIRED };
struct slot {
 uint8_t *data;
 uint64_t cookie, lease, dma_token; size_t bytes, offset; unsigned state;
};
struct port {
 struct slot slots[SS_USB_SERIAL_SLOTS];
 struct ss_usb_serial_stats stats;
 const struct ss_usb_serial_buffer_ops *ops; void *context;
 uint64_t generation, next_cookie; uint16_t packet;
 bool attached, ftdi;
};
static struct port ports[SS_USB_SERIAL_PORTS];
__attribute__((weak)) void ss_usb_serial_ready(unsigned p) { (void)p; }
__attribute__((weak)) void ss_usb_serial_changed(unsigned p, bool a) { (void)p; (void)a; }
static struct port *get(unsigned p) { return p < SS_USB_SERIAL_PORTS ? &ports[p] : NULL; }
static void free_slot(struct port *p,unsigned n,struct slot *s) {
 if(p->ops && s->data) { p->ops->release(p->context,n,s->dma_token); s->data=NULL; }
 s->state=FREE;
}
bool ss_usb_serial_backend(unsigned n,const struct ss_usb_serial_buffer_ops *ops,void *ctx) {
 struct port *p=get(n); if(!p || p->attached || !ops || !ops->acquire || !ops->release) return false;
 for(unsigned i=0;i<SS_USB_SERIAL_SLOTS;i++) if(p->slots[i].state!=FREE) return false;
 p->ops=ops; p->context=ctx; return true;
}
bool ss_usb_serial_bind(unsigned n,uint8_t *buffers[SS_USB_SERIAL_SLOTS]) {
 struct port *p=get(n); if(!p || !buffers || p->attached) return false;
 for(unsigned i=0;i<SS_USB_SERIAL_SLOTS;i++) {
  if(p->slots[i].state!=FREE || !buffers[i] || ((uintptr_t)buffers[i]&63)) return false;
  for(unsigned j=0;j<i;j++) {
   uintptr_t a=(uintptr_t)buffers[i], b=(uintptr_t)buffers[j];
   if(a>b ? a-b<SS_USB_SERIAL_BUFFER : b-a<SS_USB_SERIAL_BUFFER) return false;
  }
 }
 for(unsigned i=0;i<SS_USB_SERIAL_SLOTS;i++) p->slots[i].data=buffers[i];
 return true;
}
bool ss_usb_serial_attach(unsigned n, uint64_t gen, uint16_t packet, bool ftdi) {
 struct port *p=get(n);
 if(!p || !gen || !packet || packet>1024 || (packet&(packet-1)) || p->attached || gen<=p->generation) return false;
 for(unsigned i=0;i<SS_USB_SERIAL_SLOTS;i++) if(p->slots[i].state!=FREE || (!p->ops && !p->slots[i].data)) return false;
 p->generation=gen; p->packet=packet; p->ftdi=ftdi; p->attached=true;
 ss_usb_serial_changed(n,true); return true;
}
void ss_usb_serial_detach(unsigned n) {
 struct port *p=get(n); if(!p || !p->attached) return;
 p->attached=false;
 for(unsigned i=0;i<SS_USB_SERIAL_SLOTS;i++) {
  struct slot *s=&p->slots[i];
  if(s->state==DMA) s->state=RETIRED;
  else if(s->state==READY) { p->stats.dropped+=s->bytes-s->offset; free_slot(p,n,s); }
 }
 ss_usb_serial_changed(n,false);
}
void ss_usb_serial_quiesced(unsigned n) {
 struct port *p=get(n); if(!p || p->attached) return;
 for(unsigned i=0;i<SS_USB_SERIAL_SLOTS;i++) if(p->slots[i].state==RETIRED) free_slot(p,n,&p->slots[i]);
}
uint8_t *ss_usb_serial_rx_buffer(unsigned n,uint64_t *cookie,size_t *capacity) {
 struct port *p=get(n); if(!p || !p->attached || !cookie || !capacity) return NULL;
 for(unsigned i=0;i<SS_USB_SERIAL_SLOTS;i++) if(p->slots[i].state==DMA) return NULL;
 for(unsigned i=0;i<SS_USB_SERIAL_SLOTS;i++) {
  struct slot *s=&p->slots[i]; if(s->state!=FREE) continue;
  if(p->next_cookie==UINT64_MAX) return NULL;
  if(p->ops) {
   size_t size=0;
   if(p->ops->acquire(p->context,n,&s->data,&size,&s->dma_token) || !s->data) return NULL;
   if(size<SS_USB_SERIAL_BUFFER) { free_slot(p,n,s); return NULL; }
  }
  s->cookie=++p->next_cookie; s->state=DMA; s->offset=0; s->bytes=0;
  *cookie=s->cookie; *capacity=SS_USB_SERIAL_BUFFER; return s->data;
 }
 p->stats.backpressure++; return NULL;
}
bool ss_usb_serial_rx_complete(unsigned n,uint64_t cookie,size_t bytes,bool success) {
 struct port *p=get(n); if(!p) return false;
 for(unsigned i=0;i<SS_USB_SERIAL_SLOTS;i++) {
  struct slot *s=&p->slots[i]; if(s->cookie!=cookie || (s->state!=DMA && s->state!=RETIRED)) continue;
  if(s->state==RETIRED || !p->attached) { free_slot(p,n,s); p->stats.stale++; return false; }
  if(!success || bytes>SS_USB_SERIAL_BUFFER) { free_slot(p,n,s); p->stats.errors++; return false; }
  s->bytes=bytes; s->offset=0; if(bytes) s->state=READY; else free_slot(p,n,s);
  p->stats.received+=bytes; if(bytes) ss_usb_serial_ready(n); return true;
 }
 p->stats.stale++; return false;
}
bool ss_usb_serial_peek(unsigned n,struct ss_usb_serial_view *v) {
 struct port *p=get(n); if(!p || !v || !p->attached) return false;
 struct slot *s=NULL;
 for(unsigned i=0;i<SS_USB_SERIAL_SLOTS;i++) {
  struct slot *t=&p->slots[i];
  if((t->state==READY || t->state==LEASED) && (!s || t->cookie<s->cookie)) s=t;
 }
 if(!s || s->state==LEASED) return false;
 while(s->offset<s->bytes) {
  size_t count=s->bytes-s->offset;
  if(p->ftdi) {
   if(count>p->packet) count=p->packet;
   if(count<=2) { s->offset+=count; continue; }
   v->data=s->data+s->offset+2; v->size=count-2;
  } else { v->data=s->data+s->offset; v->size=count; }
  if(p->next_cookie==UINT64_MAX) return false;
  v->token=s->lease=++p->next_cookie; s->state=LEASED; return true;
 }
 free_slot(p,n,s); return false;
}
bool ss_usb_serial_rx_release(unsigned n,uint64_t token) {
 struct port *p=get(n); if(!p) return false;
 for(unsigned i=0;i<SS_USB_SERIAL_SLOTS;i++) {
  struct slot *s=&p->slots[i]; if(s->lease!=token || s->state!=LEASED) continue;
  size_t count=s->bytes-s->offset; if(p->ftdi && count>p->packet) count=p->packet;
  p->stats.released+=count; s->offset+=count;
  if(!p->attached || s->offset==s->bytes) free_slot(p,n,s); else s->state=READY; return true;
 }
 return false;
}
bool ss_usb_serial_stats(unsigned n,struct ss_usb_serial_stats *stats) {
 struct port *p=get(n); if(!p || !stats) return false; *stats=p->stats; return true;
}
