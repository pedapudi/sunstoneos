#!/usr/bin/env python3
"""Compare retained VGA controller records with pinned A733 instruction helpers."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('evidence','libraries','native'):p.add_argument('--'+n,type=Path,required=True)
    a=p.parse_args();spec=importlib.util.spec_from_file_location('reference',ROOT/'checks/a733-video-h264-reference.py');ref=importlib.util.module_from_spec(spec);spec.loader.exec_module(ref)
    r=ref.Reference(a.libraries/'libawh264.so')
    for address,value in [(ref.OUTER+0x190,ref.DEC),(ref.DEC+0x18,ref.MMIO),(ref.DEC+0x20,ref.CTX),
            (ref.DEC+0x68,ref.OUTER+0x68),(ref.OUTER+0x100,ref.OPS),(ref.OPS+0x40,ref.GROUP),
            (ref.DEC+0x1088,ref.SHADOW),(ref.CTX+0x100,ref.SPS),(ref.CTX+0xe390,ref.PIC),(ref.PIC+0xf0,ref.PICTURE)]:r.put(address,value,'Q')
    r.put(ref.SPS+1,2,'B');r.put(ref.SPS+0x40,4);r.put(ref.CTX+0x18,1);r.put(ref.CTX+0x1e,3,'B')
    plans=json.loads((a.evidence/'parser-oracle.json').read_text())['plans'];orders=[]
    for i,picture in enumerate(plans):
        number=picture[10];r.put(ref.CTX+0xe4c8,int(number==0),'B');r.put(ref.CTX+0x4c,number)
        r.call('H264InitPoc',ref.CTX);actual=list(struct.unpack('<3I',r.u.mem_read(ref.PIC,12)))
        assert actual==[picture[11]]*3;orders.append(actual)
        frame=0x134000
        for field,value in [(4,i&1),(8,picture[11]),(12,picture[11]),(16,0),(20,0),(24,0)]:r.put(frame+field,value)
        r.call('H264ConfigureRefFrameRegister',ref.DEC,frame)
    r.put(ref.CTX+0xe6e8,1,'B');r.put(ref.CTX+0xe6e4,2)
    for field,value in [(0xe518,76800),(0xe51c,640),(0xe520,320)]:r.put(ref.CTX+field,value)
    r.call('H264ConfigureFrameInfoRegister',ref.DEC,ref.PIC)
    top={offset:value for group,offset,value in r.trace if group=='top'}
    assert top[0xc4]==76800 and top[0xc8]==0x01400280 and top[0xec]&0x70==0x20
    native=json.loads((a.native/'result.json').read_text());triggers=[x for x in native['cases'][0]['triggers'] if x['picture']<5]+[native['cases'][0]['triggers'][-1]]
    # The saved native record also contains the intentionally failed final-frame branch.
    assert [x['picture'] for x in triggers]==list(range(6))
    for i,(picture,trigger) in enumerate(zip(plans,triggers)):
        regs=trigger['registers'];sram=trigger['sram'];slot=i&1
        for off,value in {0xc4:top[0xc4],0xc8:top[0xc8],0x200:picture[5],0x204:picture[6],0x208:picture[7],0x20c:picture[8],0x21c:picture[9],0x238:picture[3]*8,0x24c:slot}.items():assert regs[str(off)]==value
        assert sram[256+slot*8:256+slot*8+3]==[orders[i][0],orders[i][1],0]
        assert sram[256+slot*8+6]-sram[256+slot*8+5]==19456
    (a.evidence/'a733-vga-reference.json').write_text(json.dumps(dict(passed=True,physical_access=False,pins=ref.PINS,POCs=orders,trace=r.trace,scope='Original A733 POC/primary-format/frame-SRAM instructions with inert MMIO; comparison consumes preserved parser/native records; no device decoding or DMA.'),indent=2)+'\n')
    print('PASS VGA primary format/stride, repeated-IDR POC and frame-SRAM fields vs pinned A733 instructions')
if __name__=='__main__':main()
