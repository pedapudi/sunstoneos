/* SPDX-License-Identifier: Apache-2.0 */
/* Included in the pinned CDC class to retain its family dispatch and buffers. */
static struct { bool busy,enabled; tuh_xfer_cb_t cb; uintptr_t user; uint8_t data[16]; } native_flow[CFG_TUH_CDC];
static void native_flow_done(tuh_xfer_t *x) {
 unsigned n=(unsigned)x->user_data;
 if(n>=CFG_TUH_CDC || !native_flow[n].busy) return;
 tuh_xfer_cb_t cb=native_flow[n].cb; x->user_data=native_flow[n].user;
 native_flow[n].busy=false; if(cb) cb(x);
}
static void native_flow_read(tuh_xfer_t *x) {
 unsigned n=(unsigned)x->user_data;
 if(n>=CFG_TUH_CDC || !native_flow[n].busy) return;
 cdch_interface_t *p=get_itf((uint8_t)n);
 if(!p || x->result!=XFER_RESULT_SUCCESS) { native_flow_done(x); return; }
 if(x->actual_len!=(p->serial_drid==SERIAL_DRIVER_CP210X?16u:1u)) {
  x->result=XFER_RESULT_FAILED; native_flow_done(x); return;
 }
 bool enabled=native_flow[n].enabled;
 uint8_t *b=native_flow[n].data; bool ok=false;
 if(p->serial_drid==SERIAL_DRIVER_PL2303) b[0]=x->buffer[0];
 if(p->serial_drid==SERIAL_DRIVER_CP210X) {
  /* Preserve threshold words and unrelated feature bits from GET_FLOW. */
  uint32_t hs=tu_le32toh(tu_unaligned_read32(b)), repl=tu_le32toh(tu_unaligned_read32(b+4));
  hs=(hs & ~0x7bu) | 1u | (enabled?8u:0u);
  repl=(repl & ~0xc3u) | (enabled?0x80u:0x40u);
  tu_unaligned_write32(b,tu_htole32(hs)); tu_unaligned_write32(b+4,tu_htole32(repl));
  ok=cp210x_set_request(p,CP210X_SET_FLOW,0,b,16,native_flow_done,n);
 } else if(p->serial_drid==SERIAL_DRIVER_PL2303) {
  uint8_t reg=0,mask=0xf0,value=enabled?((p->pl2303.quirks&PL2303_QUIRK_LEGACY)?0x40:0x60):0;
  if(p->pl2303.type==PL2303_TYPE_HXN) { reg=0x0a; mask=0x1c; value=enabled?0x18:0x1c; }
  ok=pl2303_vendor_write(p,reg,(uint8_t)((b[0]&~mask)|value),native_flow_done,n);
 }
 if(!ok) { x->result=XFER_RESULT_FAILED; native_flow_done(x); }
}
bool sunstone_cdch_flow(uint8_t idx,bool enabled,tuh_xfer_cb_t cb,uintptr_t user) {
 cdch_interface_t *p=get_itf(idx);
 if(!p || idx>=CFG_TUH_CDC || native_flow[idx].busy) return false;
 if(p->serial_drid==SERIAL_DRIVER_ACM) {
  if(enabled) return false; /* ACM has no standard automatic RTS/CTS request. */
  tuh_xfer_t x={.daddr=p->daddr,.result=XFER_RESULT_SUCCESS,.user_data=user};
  if(cb) cb(&x); return true;
 }
 native_flow[idx].busy=true; native_flow[idx].enabled=enabled; native_flow[idx].cb=cb; native_flow[idx].user=user;
 bool ok=false;
 switch(p->serial_drid) {
 case SERIAL_DRIVER_FTDI:
  ok=ftdi_set_request(p,2,0x40,0,p->ftdi.channel|(enabled?0x100:0),native_flow_done,idx); break;
 case SERIAL_DRIVER_CH34X:
  ok=ch34x_control_out(p,0x9a,0x2727,enabled?0x0101:0,native_flow_done,idx); break;
 case SERIAL_DRIVER_CP210X: {
  tusb_control_request_t setup={.bmRequestType=0xc1,.bRequest=0x14,.wIndex=tu_htole16(p->bInterfaceNumber),.wLength=tu_htole16(16)};
  tuh_xfer_t x={.daddr=p->daddr,.ep_addr=0,.setup=&setup,.buffer=native_flow[idx].data,.complete_cb=native_flow_read,.user_data=idx};
  ok=tuh_control_xfer(&x); break;
 }
 case SERIAL_DRIVER_PL2303:
  ok=pl2303_vendor_read(p,p->pl2303.type==PL2303_TYPE_HXN?0x0a:0,native_flow_read,idx); break;
 default: break;
 }
 if(!ok) native_flow[idx].busy=false;
 return ok;
}

void sunstone_cdch_control_quiesced(uint8_t idx) {
 if(idx<CFG_TUH_CDC) native_flow[idx].busy=false;
}
