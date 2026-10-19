#!/usr/bin/env python3
"""Prepare only the pinned Cadence USB-only parameter functions for the native adapter."""
import argparse,hashlib,re
from pathlib import Path
PIN='84b5471d3b7bbb2ae247b75459135dff0a1d40eef9a2bb994df14eb799b5c082'
def block(text,name):
    start=re.search(r'static void '+name+r'\(',text).start();at=text.index('{',start);depth=1;end=at+1
    while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
def prepare(source):
    raw=source.read_bytes();assert hashlib.sha256(raw).hexdigest()==PIN,'Cadence source differs'
    text=raw.decode()
    end=text.index('struct',text.index('#define SUBSYS_REG_USB3BGR'))
    defines=text[text.index('/* Sub-System USB3.1 Bus'):end]
    functions='\n'.join(block(text,n) for n in ('combo0_usb_clk_set','combo0_usb_param_config'))
    functions=functions.replace('readl(', 'ss_read32(sunxi_cphy,').replace('writel(', 'ss_write32(sunxi_cphy,').replace('phy_writew(', 'ss_write16(sunxi_cphy,')
    return text[:text.index('#include')]+defines+'\n'+functions+'\n'
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();data=prepare(a.source)
    if not a.output.exists() or a.output.read_text()!=data:a.output.write_text(data)
    indices=sorted(set(re.findall(r'combo0->phy_reg \+ \(0x([0-9a-fA-F]+) << 1\)',data)))
    addresses=''.join('case 0x'+i+':\n' for i in indices)+'return 1;\n'
    target=a.output.with_name('ss-cadence-addresses.inc')
    if not target.exists() or target.read_text()!=addresses:target.write_text(addresses)
