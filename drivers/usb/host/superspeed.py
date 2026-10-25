#!/usr/bin/env python3
"""Prepare a private pinned TinyUSB host tree with bounded SuperSpeed BOT and keyboard enumeration."""
import argparse,hashlib,shutil
from pathlib import Path
PINS={'class/hid/hid_host.c': '5c0add1606bfd97ef9cf739c713a1875dc8dbeb434d8a3d7c27b074ccaf34c3e', 'common/tusb_types.h': '60debb8a21b42e32f8664ac53913e9711470293ab79f1c312000df9112aacf8c', 'tusb.c': '02c521aa1a3b97ffdb7a6b29d77e18353c368f16187ade6b311b1706f519db15', 'host/usbh.c': '8b87d1b3a90962ec02d8b643c43a3385452e5976094f453ad4a19e0216fd711d', 'class/msc/msc_host.c': 'd4a31430507766f022047572b6162a57d9a4286782a262010b8f014361463e1a'}
def replace(s,a,b):
    assert a in s,a[:80]
    return s.replace(a,b)
def prepare(source,out):
    for name,digest in PINS.items():assert hashlib.sha256((source/name).read_bytes()).hexdigest()==digest,name+" differs from pinned TinyUSB"
    shutil.copytree(source,out,dirs_exist_ok=True)
    def change(name,fn):
        p=out/name;original=(source/name).read_text();data=fn(original)
        if p.read_text()!=data:p.write_text(data)
    change('common/tusb_types.h',lambda s:replace(s,'TUSB_SPEED_HIGH = 2,','TUSB_SPEED_HIGH = 2,\n  TUSB_SPEED_SUPER = 3,'))
    change('tusb.c',lambda s:replace(replace(s,'{"Full", "Low", "High"}','{"Full", "Low", "High", "Super"}'),'if (speed == TUSB_SPEED_HIGH) {\n        // Bulk highspeed', 'if (speed == TUSB_SPEED_SUPER) {\n        TU_ASSERT(max_packet_size == 1024);\n      } else if (speed == TUSB_SPEED_HIGH) {\n        // Bulk highspeed'))
    def msc(s):
        s=replace(s,'bool msch_xfer_cb(uint8_t dev_addr,','''extern bool a733_tinyusb_bot_recover(uint8_t daddr);
/* Called only after the board adapter completes its bounded reset sequence. */
bool a733_tinyusb_msc_retry(uint8_t daddr) {
  msch_interface_t *p_msc=get_itf(daddr);
  msch_epbuf_t *epbuf=get_epbuf(daddr);
  p_msc->stage=MSC_STAGE_CMD;
  p_msc->data_xferred=0;
  return usbh_edpt_xfer(daddr,p_msc->ep_out,(uint8_t *)&epbuf->cbw,sizeof(msc_cbw_t));
}
bool msch_xfer_cb(uint8_t dev_addr,''')
        s=replace(s,'  switch (p_msc->stage) {','''  if(event==XFER_RESULT_STALLED)return a733_tinyusb_bot_recover(dev_addr);
  switch (p_msc->stage) {''')
        s=replace(s,'(uint16_t)(sizeof(tusb_desc_interface_t) + desc_itf->bNumEndpoints * sizeof(tusb_desc_endpoint_t));','(uint16_t)(sizeof(tusb_desc_interface_t) + desc_itf->bNumEndpoints * (sizeof(tusb_desc_endpoint_t) + (tuh_speed_get(dev_addr)==TUSB_SPEED_SUPER?6:0)));')
        return replace(s,'ep_desc = (tusb_desc_endpoint_t const*) tu_desc_next(ep_desc);','''ep_desc = (tusb_desc_endpoint_t const*) tu_desc_next(ep_desc);
    if(tuh_speed_get(dev_addr)==TUSB_SPEED_SUPER){
      const uint8_t *comp=(const uint8_t *)ep_desc;
      TU_ASSERT(comp[0]==6 && comp[1]==0x30,0);
      ep_desc=(const tusb_desc_endpoint_t *)(comp+6);
    }''')
    change('class/msc/msc_host.c',msc)
    def hid(s):
        s=replace(s,'desc_itf->bNumEndpoints * sizeof(tusb_desc_endpoint_t));',
          'desc_itf->bNumEndpoints * (sizeof(tusb_desc_endpoint_t) + (tuh_speed_get(daddr)==TUSB_SPEED_SUPER?6:0)));')
        s=replace(s,'    p_desc = tu_desc_next(p_desc);\n  }\n\n  // Assume bNumDescriptors','''    p_desc = tu_desc_next(p_desc);
    if(tuh_speed_get(daddr)==TUSB_SPEED_SUPER){
      TU_ASSERT(desc_itf->bNumEndpoints==1 && p_desc[0]==6 && p_desc[1]==0x30 &&
        p_desc[2]==0 && p_desc[3]==0 && tu_unaligned_read16(p_desc+4)==p_hid->epin_size,0);
      p_desc+=6;
    }
  }

  // Assume bNumDescriptors''')
        return s
    change('class/hid/hid_host.c',hid)
    def host(s):
        s=replace(s,'uint8_t max_packet_size)', 'uint16_t max_packet_size)')
        s=replace(s,'usbh_edpt_control_open(0, 8)', 'usbh_edpt_control_open(0, dev0_bus->speed==TUSB_SPEED_SUPER?512:8)')
        s=replace(s,'usbh_edpt_control_open(new_addr, new_dev->desc_device.bMaxPacketSize0)', 'usbh_edpt_control_open(new_addr, new_dev->bus_info.speed==TUSB_SPEED_SUPER?512:new_dev->desc_device.bMaxPacketSize0)')
        s=replace(s,'desc_device->bMaxPacketSize0 >= 8)', '(dev0_bus->speed==TUSB_SPEED_SUPER?desc_device->bMaxPacketSize0==9:desc_device->bMaxPacketSize0>=8))')
        s=replace(s,'  ENUM_SET_CONFIG,','  ENUM_SS_BOS_HEADER, ENUM_SS_BOS_FULL, ENUM_SS_ISOCH, ENUM_SS_SEL,\n  ENUM_SET_CONFIG,')
        s=replace(s,'static void    process_enumeration(tuh_xfer_t *xfer);','static void    process_enumeration(tuh_xfer_t *xfer);\nextern bool a733_tinyusb_ss_enumerate(uint8_t,unsigned,uint8_t *,tuh_xfer_cb_t,uintptr_t);\nextern bool a733_tinyusb_ss_direct(uint8_t);')
        a='''is_enum_failed = !tuh_descriptor_get_string_langid(daddr, _usbh_epbuf.ctrl, 2,
                                                         process_enumeration, ENUM_GET_STRING_LANGUAGE_ID);'''
        s=replace(s,a,'''if(dev->bus_info.speed==TUSB_SPEED_SUPER)
        is_enum_failed=!a733_tinyusb_ss_enumerate(daddr,0,_usbh_epbuf.ctrl,process_enumeration,ENUM_SS_BOS_HEADER);
      else '''+a)
        s=replace(s,'dev0_bus->speed = (port_status.status.high_speed)  ? TUSB_SPEED_HIGH',
          'dev0_bus->speed = tuh_speed_get(dev0_bus->hub_addr)==TUSB_SPEED_SUPER?TUSB_SPEED_SUPER:(port_status.status.high_speed)  ? TUSB_SPEED_HIGH')
        return replace(s,'    case ENUM_GET_STRING_LANGUAGE_ID: {','''    case ENUM_SS_BOS_HEADER:
      is_enum_failed=!a733_tinyusb_ss_enumerate(daddr,1,_usbh_epbuf.ctrl,process_enumeration,ENUM_SS_BOS_FULL);break;
    case ENUM_SS_BOS_FULL:
      is_enum_failed=!a733_tinyusb_ss_enumerate(daddr,2,_usbh_epbuf.ctrl,process_enumeration,ENUM_SS_ISOCH);break;
    case ENUM_SS_ISOCH:
      if(a733_tinyusb_ss_direct(daddr))
        is_enum_failed=!a733_tinyusb_ss_enumerate(daddr,3,_usbh_epbuf.ctrl,process_enumeration,ENUM_SS_SEL);
      else is_enum_failed=!tuh_descriptor_get_string_langid(daddr,_usbh_epbuf.ctrl,2,process_enumeration,ENUM_GET_STRING_LANGUAGE_ID);
      break;
    case ENUM_SS_SEL:
      is_enum_failed=!tuh_descriptor_get_string_langid(daddr,_usbh_epbuf.ctrl,2,process_enumeration,ENUM_GET_STRING_LANGUAGE_ID);break;
    case ENUM_GET_STRING_LANGUAGE_ID: {''')
    change('host/usbh.c',host)
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();prepare(a.source,a.output)
