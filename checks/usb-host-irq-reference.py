#!/usr/bin/env python3
"""Verify the pinned interrupt route and reference acknowledgement ordering."""
import argparse,hashlib,json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    sources=Path.home()/'.cache/sunstoneos/sources';lock=json.loads((ROOT/'sources.lock.json').read_text());inputs={}
    for key in ('board-usb-host-irq-xhci-ring-c','board-usb-host-irq-xhci-h','board-gpu-power-sun60iw2p1-dtsi','usb-xhci-specification'):
        record=lock[key];path=sources/record['archive'];digest=hashlib.sha256(path.read_bytes()).hexdigest();assert digest==record['sha256'];inputs[str(path)]=digest
    linux=(sources/'usb-host-irq/xhci-ring.c').read_text();handler=linux[linux.index('irqreturn_t xhci_irq('):linux.index('irqreturn_t xhci_msi_irq(')]
    assert handler.index('status |= STS_EINT')<handler.index('irq_pending |= IMAN_IP')<handler.index('while (xhci_handle_event')<handler.index('xhci_update_erst_dequeue(xhci, ir, event_ring_deq, true)')
    dtsi=(sources/lock['board-gpu-power-sun60iw2p1-dtsi']['archive']).read_text();node=dtsi[dtsi.index('xhci2: xhci2-controller@6a00000'):];node=node[:node.index('};')]
    assert 'reg = <0x0 0x06a00000 0x0 0x100000>' in node and 'interrupts = <GIC_SPI 155 IRQ_TYPE_LEVEL_HIGH>' in node
    spec=(sources/'usb-host/xhci-1.2.txt').read_text()
    for text in ('Interrupt Pending (IP) - RW1C','Event Handler Busy (EHB) - RW1C','Software that uses EINT shall clear it prior to clearing any IP flags','Software shall not write ERDP consecutively with the same value'):
        assert text in spec
    a.output.write_text(json.dumps(dict(passed=True,physical_access=False,inputs=inputs,scope='Pinned A733 DT route, Intel xHCI1.2 W1C/EINT-before-IP/ERDP advancement requirements and pinned Linux acknowledgement/drain ordering. Source comparison only; Linux xhci_irq is not linked or executed.'),indent=2)+'\n')
if __name__=='__main__':main()
