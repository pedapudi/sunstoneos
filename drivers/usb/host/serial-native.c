/* SPDX-License-Identifier: Apache-2.0 */
#include "sunstone_usb_serial.h"
#include "sunstone_usb_transport.h"
#include "sunstone_usb_service.h"
#include "tusb.h"
#include <limits.h>
#include <string.h>
struct native_port { uint8_t address,in,out; bool attached; };
static struct native_port ports[SS_USB_SERIAL_PORTS];
static struct sunstone_usb_transport *transport;
static struct { unsigned port; uint64_t token, core_token; const uint8_t *data; size_t bytes; bool deferred; } loan;
static uint64_t sequence;
static bool stopping;
static struct { bool busy; unsigned port; uint64_t sequence; uint64_t token; bool borrowed; } tx;
struct config { bool busy, flow; unsigned step; int result; cdc_line_coding_t line; };
static struct config configs[SS_USB_SERIAL_PORTS];
extern bool sunstone_cdch_rx_start(uint8_t);
static int acquire(void *ctx,unsigned n,uint8_t **p,size_t *size,uint64_t *token) {
 (void)ctx;
 return transport->ops->acquire(transport,ports[n].address,ports[n].in,p,size,token)==1?0:-1;
}
static void release(void *ctx,unsigned n,uint64_t token) {
 (void)ctx;
 (void)transport->ops->release(transport,ports[n].address,ports[n].in,token);
}
static const struct ss_usb_serial_buffer_ops buffer_ops={acquire,release};
int os_usb_serial_native_start(struct sunstone_usb_transport *t) {
 if(!t || !t->ops || !t->ops->acquire || !t->ops->release || !t->ops->submit) return -1;
 transport=t; return 1;
}
bool sunstone_serial_prepare(unsigned n,uint8_t address,uint8_t in,uint8_t out) {
 if(stopping || !transport || n>=SS_USB_SERIAL_PORTS || (tx.busy && tx.port==n) || configs[n].busy || !transport->ops->acquire || !transport->ops->release) return false;
 /* Retired callbacks retain the old address until quiescence and release. */
 if(!ss_usb_serial_backend(n,&buffer_ops,NULL)) return false;
 ports[n]=(struct native_port){address,in,out,false}; return true;
}
void ss_usb_serial_changed(unsigned n,bool attached) {
 ports[n].attached=attached;
 if(attached) os_usb_service_connected(SS_DEVICE_USB_SERIAL);
 else {
  bool any=false; for(unsigned i=0;i<SS_USB_SERIAL_PORTS;i++) any|=ports[i].attached;
  if(!any) os_usb_service_disconnected(SS_DEVICE_USB_SERIAL);
 }
}
static void publish(void) {
 if(stopping || loan.token) return;
 for(unsigned n=0;n<SS_USB_SERIAL_PORTS;n++) {
  struct ss_usb_serial_view v;
  if(!ss_usb_serial_peek(n,&v)) continue;
  if(sequence==UINT64_MAX) { ss_usb_serial_rx_release(n,v.token); return; }
  loan.port=n; loan.core_token=v.token; loan.token=++sequence; loan.data=v.data; loan.bytes=v.size; loan.deferred=false;
  struct ss_usb_serial_buffer b={loan.token,sequence,v.data,v.size};
  if(os_usb_service_serial(&b)!=1) {
   ss_usb_serial_rx_release(n,v.token); loan.token=0;
  }
  return;
 }
}
void ss_usb_serial_ready(unsigned n) { (void)n; publish(); }
void os_usb_serial_native_release(uint64_t token) {
 if(!loan.token || loan.token!=token) return;
 if(tx.busy && tx.borrowed) { loan.deferred=true; return; }
 unsigned n=loan.port;
 ss_usb_serial_rx_release(n,loan.core_token); loan.token=0;
 if(!stopping) { (void)sunstone_cdch_rx_start((uint8_t)n); publish(); }
}
static void write_done(void *ctx,unsigned status,size_t bytes) {
 (void)ctx;
 if(!tx.busy) return;
 if(!tx.borrowed) (void)transport->ops->release(transport,ports[tx.port].address,ports[tx.port].out,tx.token);
 uint64_t seq=tx.sequence; tx.busy=false;
 os_usb_service_write_complete(seq,status==0?1:-1,bytes);
 if(loan.deferred) os_usb_serial_native_release(loan.token);
}
int os_usb_serial_native_write(uint64_t seq,const void *data,size_t bytes) {
 if(stopping || !transport || !data || !bytes || bytes>SS_USB_SERIAL_BUFFER) return -1;
 if(tx.busy) return 0;
 for(unsigned n=0;n<SS_USB_SERIAL_PORTS;n++) if(ports[n].attached) {
  if(loan.token && loan.port==n && (uintptr_t)data>=(uintptr_t)loan.data &&
     (uintptr_t)data-(uintptr_t)loan.data<=loan.bytes && bytes<=loan.bytes-((uintptr_t)data-(uintptr_t)loan.data)) {
   tx.busy=true; tx.borrowed=true; tx.port=n; tx.sequence=seq;
   int result=transport->ops->submit_direct?transport->ops->submit_direct(transport,ports[n].address,ports[n].out,(uint8_t *)(uintptr_t)data,bytes,write_done,NULL):
       transport->ops->submit(transport,ports[n].address,ports[n].out,(uint8_t *)(uintptr_t)data,bytes,write_done,NULL);
   if(result!=1) { tx.busy=false; return result<0?-1:0; }
   return 1;
  }
  uint8_t *buffer; size_t capacity; uint64_t token;
  if(transport->ops->acquire(transport,ports[n].address,ports[n].out,&buffer,&capacity,&token)!=1) return -1;
  if(bytes>capacity) { transport->ops->release(transport,ports[n].address,ports[n].out,token); return -1; }
  /* Transport owns the NC page. TX staging is one bounded copy from caller memory. */
  memcpy(buffer,data,bytes);
  tx.busy=true; tx.borrowed=false; tx.port=n; tx.sequence=seq; tx.token=token;
  int result=transport->ops->submit_direct?transport->ops->submit_direct(transport,ports[n].address,ports[n].out,buffer,bytes,write_done,NULL):
      transport->ops->submit(transport,ports[n].address,ports[n].out,buffer,bytes,write_done,NULL);
  if(result!=1) {
   tx.busy=false; transport->ops->release(transport,ports[n].address,ports[n].out,token); return result<0?-1:0;
  }
  return 1;
 }
 return -1;
}
void tuh_cdc_mount_cb(uint8_t idx) { (void)idx; }
void tuh_cdc_umount_cb(uint8_t idx) { (void)idx; }
extern bool sunstone_cdch_flow(uint8_t,bool,tuh_xfer_cb_t,uintptr_t);
static void configured(tuh_xfer_t *x) {
 unsigned n=(unsigned)x->user_data; if(n>=SS_USB_SERIAL_PORTS || !configs[n].busy) return;
 struct config *c=&configs[n];
 if(x->result!=XFER_RESULT_SUCCESS || !ports[n].attached) { c->busy=false; c->result=-1; return; }
 bool ok;
 if(c->step++==0) ok=tuh_cdc_set_control_line_state((uint8_t)n,3,configured,n);
 else if(c->step==2) {
  ok=sunstone_cdch_flow((uint8_t)n,c->flow,configured,n);
 } else { c->busy=false; c->result=1; return; }
 if(!ok) { c->busy=false; c->result=-1; }
}
int os_usb_serial_native_config(unsigned n,uint32_t baud,unsigned bits,unsigned parity,unsigned stop,bool flow) {
 if(stopping || n>=SS_USB_SERIAL_PORTS || !ports[n].attached || configs[n].busy || !baud || bits<5 || bits>8 || parity>4 || stop>2) return -1;
 struct config *c=&configs[n];
 *c=(struct config){.busy=true,.flow=flow,.line={baud,(uint8_t)stop,(uint8_t)parity,(uint8_t)bits}};
 if(!tuh_cdc_set_line_coding((uint8_t)n,&c->line,configured,n)) { c->busy=false; c->result=-1; return -1; }
 return 1;
}
int os_usb_serial_native_config_result(unsigned n) {
 if(n>=SS_USB_SERIAL_PORTS) return -1;
 return configs[n].busy?0:configs[n].result;
}

static unsigned config_port=SS_USB_SERIAL_PORTS;
int os_usb_serial_native_configure(const struct ss_usb_serial_line *line) {
 if(!line) return -1;
 for(unsigned n=0;n<SS_USB_SERIAL_PORTS;n++) if(ports[n].attached) {
  config_port=n;
  return os_usb_serial_native_config(n,line->baud,line->bits,line->parity,line->stop,line->rtscts!=0);
 }
 return -1;
}
int os_usb_serial_native_configured(void) { return os_usb_serial_native_config_result(config_port); }

extern void sunstone_cdch_control_quiesced(uint8_t);
void os_usb_serial_native_quiesced(unsigned n) {
 if(n>=SS_USB_SERIAL_PORTS || ports[n].attached) return;
 if(tx.busy && tx.port==n) write_done(NULL,2,0);
 configs[n].busy=false; configs[n].result=-1;
 sunstone_cdch_control_quiesced((uint8_t)n); ss_usb_serial_quiesced(n);
}
void os_usb_serial_native_quiesce(uint8_t address) {
 for(unsigned n=0;n<SS_USB_SERIAL_PORTS;n++) if(ports[n].address==address) os_usb_serial_native_quiesced(n);
}
/* Original host task, only after the controller halt proof. Detaching the
 * software class and completing canceled OUT work permits ordinary token
 * release, while stopping prevents that release from refilling RX. */
void os_usb_serial_native_stopped(void) {
 if(stopping || !transport) return;
 stopping=true;
 for(unsigned n=0;n<SS_USB_SERIAL_PORTS;n++) {
  if(ports[n].attached) ss_usb_serial_detach(n);
  os_usb_serial_native_quiesced(n);
 }
}
