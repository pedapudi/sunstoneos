#!/usr/bin/env python3
"""Prepare a private pinned TinyUSB host tree with bounded SuperSpeed BOT enumeration."""
import argparse,hashlib,shutil
from pathlib import Path
PINS={'common/tusb_types.h': '60debb8a21b42e32f8664ac53913e9711470293ab79f1c312000df9112aacf8c', 'tusb.c': '02c521aa1a3b97ffdb7a6b29d77e18353c368f16187ade6b311b1706f519db15', 'host/usbh.c': '8b87d1b3a90962ec02d8b643c43a3385452e5976094f453ad4a19e0216fd711d', 'class/msc/msc_host.c': 'd4a31430507766f022047572b6162a57d9a4286782a262010b8f014361463e1a'}
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
        s=replace(s,'(uint16_t)(sizeof(tusb_desc_interface_t) + desc_itf->bNumEndpoints * sizeof(tusb_desc_endpoint_t));','(uint16_t)(sizeof(tusb_desc_interface_t) + desc_itf->bNumEndpoints * (sizeof(tusb_desc_endpoint_t) + (tuh_speed_get(dev_addr)==TUSB_SPEED_SUPER?6:0)));')
        return replace(s,'ep_desc = (tusb_desc_endpoint_t const*) tu_desc_next(ep_desc);','''ep_desc = (tusb_desc_endpoint_t const*) tu_desc_next(ep_desc);
    if(tuh_speed_get(dev_addr)==TUSB_SPEED_SUPER){
      const uint8_t *comp=(const uint8_t *)ep_desc;
      TU_ASSERT(comp[0]==6 && comp[1]==0x30,0);
      ep_desc=(const tusb_desc_endpoint_t *)(comp+6);
    }''')
    change('class/msc/msc_host.c',msc)
    def host(s):
        s=replace(s,'uint8_t max_packet_size)', 'uint16_t max_packet_size)')
        s=replace(s,'usbh_edpt_control_open(0, 8)', 'usbh_edpt_control_open(0, dev0_bus->speed==TUSB_SPEED_SUPER?512:8)')
        s=replace(s,'usbh_edpt_control_open(new_addr, new_dev->desc_device.bMaxPacketSize0)', 'usbh_edpt_control_open(new_addr, new_dev->bus_info.speed==TUSB_SPEED_SUPER?512:new_dev->desc_device.bMaxPacketSize0)')
        s=replace(s,'desc_device->bMaxPacketSize0 >= 8)', '(dev0_bus->speed==TUSB_SPEED_SUPER?desc_device->bMaxPacketSize0==9:desc_device->bMaxPacketSize0>=8))')
        s=replace(s,'  ENUM_SET_CONFIG,','  ENUM_SS_BOS_HEADER, ENUM_SS_BOS_FULL, ENUM_SS_ISOCH, ENUM_SS_SEL,\n  ENUM_SET_CONFIG,')
        s=replace(s,'static void    process_enumeration(tuh_xfer_t *xfer);','static void    process_enumeration(tuh_xfer_t *xfer);\nextern bool a733_tinyusb_ss_enumerate(uint8_t,unsigned,uint8_t *,tuh_xfer_cb_t,uintptr_t);')
        a='''is_enum_failed = !tuh_descriptor_get_string_langid(daddr, _usbh_epbuf.ctrl, 2,
                                                         process_enumeration, ENUM_GET_STRING_LANGUAGE_ID);'''
        s=replace(s,a,'''if(dev->bus_info.speed==TUSB_SPEED_SUPER)
        is_enum_failed=!a733_tinyusb_ss_enumerate(daddr,0,_usbh_epbuf.ctrl,process_enumeration,ENUM_SS_BOS_HEADER);
      else '''+a)
        return replace(s,'    case ENUM_GET_STRING_LANGUAGE_ID: {','''    case ENUM_SS_BOS_HEADER:
      is_enum_failed=!a733_tinyusb_ss_enumerate(daddr,1,_usbh_epbuf.ctrl,process_enumeration,ENUM_SS_BOS_FULL);break;
    case ENUM_SS_BOS_FULL:
      is_enum_failed=!a733_tinyusb_ss_enumerate(daddr,2,_usbh_epbuf.ctrl,process_enumeration,ENUM_SS_ISOCH);break;
    case ENUM_SS_ISOCH:
      is_enum_failed=!a733_tinyusb_ss_enumerate(daddr,3,_usbh_epbuf.ctrl,process_enumeration,ENUM_SS_SEL);break;
    case ENUM_SS_SEL:
      is_enum_failed=!tuh_descriptor_get_string_langid(daddr,_usbh_epbuf.ctrl,2,process_enumeration,ENUM_GET_STRING_LANGUAGE_ID);break;
    case ENUM_GET_STRING_LANGUAGE_ID: {''')
    change('host/usbh.c',host)
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();prepare(a.source,a.output)
