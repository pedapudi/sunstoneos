#!/usr/bin/env python3
"""Verify primary hub protocol sources and the pinned TinyUSB adaptation."""
import argparse,hashlib,json,subprocess,tempfile,zipfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    sources=Path.home()/'.cache/sunstoneos/sources';lock=json.loads((ROOT/'sources.lock.json').read_text());record=lock['usb2-base-specification-bundle'];bundle=sources/record['archive']
    assert hashlib.sha256(bundle.read_bytes()).hexdigest()==record['sha256']
    with zipfile.ZipFile(bundle) as z:pdf=z.read(record['base_pdf_member'])
    assert hashlib.sha256(pdf).hexdigest()==record['base_pdf_sha256']
    assert pdf==(sources/'usb-host-hub/usb_20.pdf').read_bytes()
    spec=(sources/'usb-host-hub/usb_20.txt').read_text();xhci=(sources/'usb-host/xhci-1.2.txt').read_text()
    for text in ('11.24.2.5 Get Hub Descriptor','descriptor index zero','11.24.2.7.1 Port Status Bits','bPwrOn2PwrGood'):
        assert text in spec
    for text in ('Parent Hub Slot ID','Parent Port Number','TT Think Time (TTT)','4.5.2   Slot Context Initialization'):
        assert text in xhci
    tiny=sources/'tinyusb-d668b0abb7219b9ce324fa88d46604bb9e869973/src';hub=tiny/'host/hub.c';patch=ROOT/'drivers/usb/host/hub.patch'
    with tempfile.TemporaryDirectory() as tmp:
        output=Path(tmp)/'hub.c';subprocess.run(['patch','--silent','--output',str(output),str(hub),str(patch)],check=True)
        adapted=output.read_text()
        # USB2 hubs keep descriptor type 0x29 and the nine-byte request; a SuperSpeed hub selects type 0x2a and 12 bytes.
        assert ('.bRequest = HUB_REQUEST_GET_DESCRIPTOR,\n    .wValue   = tuh_speed_get(xfer->daddr)==TUSB_SPEED_SUPER?0x2a00:0x2900,\n'
                '    .wIndex   = 0,\n    .wLength  = tuh_speed_get(xfer->daddr)==TUSB_SPEED_SUPER?12:sizeof(hub_desc_cs_t)\n') in adapted
        assert adapted.index('xfer->complete_cb = config_device_power')<adapted.index('xfer->complete_cb = config_hub_power')<adapted.index('else hub_port_set_feature(daddr,port,HUB_FEATURE_PORT_POWER,')
        assert hub.read_text().split('#include')[0]==adapted.split('#include')[0]
        digest=hashlib.sha256(output.read_bytes()).hexdigest()
    inputs={str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in (bundle,sources/'usb-host-hub/usb_20.pdf',sources/'usb-host/xhci-1.2.pdf',hub,patch,tiny/'host/usbh.c',tiny/'common/tusb_types.h')}
    a.output.write_text(json.dumps(dict(passed=True,physical_access=False,inputs=inputs,adapted_hub_sha256=digest,scope='Official USB2 descriptor/power/port-status source and xHCI hub/TT context requirements; actual pinned TinyUSB hub source patch, complete speed-selected descriptor request, USB2 power-observation order and preserved license. Source comparison only.'),indent=2)+'\n')
if __name__=='__main__':main()
