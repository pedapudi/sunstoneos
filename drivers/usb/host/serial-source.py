#!/usr/bin/env python3
"""Adapt the pinned TinyUSB CDC receive path without changing bridge protocols."""
import argparse
import hashlib
from pathlib import Path
PIN = '07a8843a16083fcadc4197eccad6a59d8fea7c6c2e88ec9d75947a1268516dd2'
def prepare(source, output):
    data = source.read_bytes()
    if hashlib.sha256(data).hexdigest() != PIN:
        raise ValueError('CDC source differs from the pinned revision')
    s = data.decode().replace('TUH_EPBUF_DEF(ctrl, 8);','TUH_EPBUF_DEF(ctrl, 16);')
    a = s.index('  } else if (ep_addr == p_cdc->stream.rx.ep_addr) {')
    b = s.index('  } else if (ep_addr == p_cdc->ep_notif)', a)
    s = s[:a] + '''  } else if (ep_addr == p_cdc->stream.rx.ep_addr) {
    sunstone_cdch_rx_done(idx, xferred_bytes, event == XFER_RESULT_SUCCESS);
''' + s[b:]
    # Error completions must release the retained RX slot too.
    s = s.replace('  TU_VERIFY(event == XFER_RESULT_SUCCESS);', '')
    s = s.replace('  if (ep_addr == p_cdc->stream.tx.ep_addr) {',
                  '  if (ep_addr == p_cdc->stream.tx.ep_addr) {\n    TU_VERIFY(event == XFER_RESULT_SUCCESS);')
    s = s.replace('tu_edpt_stream_read_xfer(&p_cdc->stream.rx)', 'sunstone_cdch_rx_start(get_idx_by_ptr(p_cdc))')
    marker = '//--------------------------------------------------------------------+\n// Weak stubs:'
    injected = '''/* SunstoneOS retained RX slots; the original class owns enumeration/control. */
#include "sunstone_usb_serial.h"
extern bool sunstone_serial_prepare(unsigned,uint8_t,uint8_t,uint8_t);
static uint64_t sunstone_rx_cookie[CFG_TUH_CDC];
static uint64_t sunstone_generation[CFG_TUH_CDC];
bool sunstone_cdch_rx_start(uint8_t idx) {
  cdch_interface_t *p=get_itf(idx);
  size_t count; uint64_t cookie;
  if (!p || !p->mounted || !usbh_edpt_claim(p->daddr,p->stream.rx.ep_addr)) return false;
  uint8_t *buf=ss_usb_serial_rx_buffer(idx,&cookie,&count);
  if (!buf) { usbh_edpt_release(p->daddr,p->stream.rx.ep_addr); return false; }
  sunstone_rx_cookie[idx]=cookie;
  if (!usbh_edpt_xfer(p->daddr,p->stream.rx.ep_addr,buf,(uint16_t)count)) {
    ss_usb_serial_rx_complete(idx,cookie,0,false);
    usbh_edpt_release(p->daddr,p->stream.rx.ep_addr); return false;
  }
  return true;
}
static void sunstone_cdch_rx_done(uint8_t idx,uint32_t count,bool ok) {
  ss_usb_serial_rx_complete(idx,sunstone_rx_cookie[idx],count,ok);
  if(ok) sunstone_cdch_rx_start(idx);
}
'''
    assert marker in s
    s = s.replace(marker, injected + '\n' + marker)
    old = '    tuh_cdc_mount_cb(idx);'
    s = s.replace(old, '''    if (!sunstone_serial_prepare(idx,p_cdc->daddr,p_cdc->stream.rx.ep_addr,p_cdc->stream.tx.ep_addr) ||
        !ss_usb_serial_attach(idx,++sunstone_generation[idx],p_cdc->stream.rx.mps,
#if CFG_TUH_CDC_FTDI
       p_cdc->serial_drid == SERIAL_DRIVER_FTDI
#else
       false
#endif
       )) { p_cdc->mounted=false; return; }
''' + old)
    s = s.replace('      tuh_cdc_umount_cb(idx);',
                  '      ss_usb_serial_detach(idx);\n      tuh_cdc_umount_cb(idx);')
    s += '\n' + Path(__file__).with_name('serial-control.inc').read_text()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(s)
if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args(); prepare(a.source,a.output)
