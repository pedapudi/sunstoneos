#!/usr/bin/env python3
"""Compare new P syntax/register fields with FFmpeg and pinned A733 instructions.

The CPU decoder supplies an independent pixel oracle. Original A733 instruction
helpers run with inert MMIO; neither test establishes native device decoding.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('evidence','parser','libraries'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();out=a.evidence
    def parse(path,valid=True):
        r=subprocess.run([a.parser,path,'valid' if valid else 'invalid'],capture_output=True)
        assert not r.returncode and not r.stderr,(path,r.stderr.decode())
        return [list(map(int,x.split())) for x in r.stdout.decode().splitlines()]
    rows=parse(out/'input.h264');assert rows[0]==[2];plans=rows[1:]
    sets=[];fields={}
    for line in (out/'headers.log').read_text().splitlines():
        if 'Slice Header' in line:
            if 'slice_type' in fields:sets.append(fields)
            fields={}
        match=re.search(r'\]\s+(\d+)\s+(\w+)\s+([01]+)\s+=\s+(-?\d+)\s*$',line)
        if match:fields[match[2]]=(int(match[1]),len(match[3]),int(match[4]))
    sets.append(fields);assert len(sets)==2
    for i,(plan,field) in enumerate(zip(plans,sets)):
        assert plan[10:]==[field['frame_num'][2],2*i,field['slice_type'][2]%5]
        assert plan[4]==max(start+bits for key,(start,bits,value) in field.items() if key in ('slice_qp_delta','disable_deblocking_filter_idc','slice_alpha_c0_offset_div2','slice_beta_offset_div2'))
    registers=struct.unpack('<4096I',(out/'registers.bin').read_bytes())
    for i,picture in enumerate(plans):
        regs=registers[i*2048:(i+1)*2048];sram=regs[1024:]
        for off,value in {0x200:picture[5],0x204:picture[6],0x208:picture[7],0x20c:picture[8],0x21c:picture[9],0x238:picture[3]*8,0x24c:i,0x234:0}.items():assert regs[off//4]==value,(i,hex(off))
        assert sram[256+8*i:256+8*i+3]==(2*i,2*i,0)
        assert sram[400:408]==(0,)*8
    raw=(out/'input.h264').read_bytes();offset=plans[1][2]
    for name,bit in [('frame-num',sets[1]['frame_num'][0]),('list-change',sets[1]['ref_pic_list_modification_flag_l0'][0]),('adaptive-marking',sets[1]['adaptive_ref_pic_marking_mode_flag'][0])]:
        changed=bytearray(raw);changed[offset+bit//8]^=1<<(7-bit%8)
        path=out/(name+'-refused.h264');path.write_bytes(changed);parse(path,False)
    path=out/'third-picture-refused.h264';path.write_bytes(raw+raw[offset-4:]);parse(path,False)
    # The old single-IDR mode remains usable, without rerunning its old cases.
    path=out/'single-idr.h264';path.write_bytes(raw[:offset-4]);assert parse(path)[0]==[1]
    # Execute the pinned A733 POC producer and SRAM field helper on metadata.
    spec=importlib.util.spec_from_file_location('ref',ROOT/'checks/a733-video-h264-reference.py');ref=importlib.util.module_from_spec(spec);spec.loader.exec_module(ref)
    r=ref.Reference(a.libraries/'libawh264.so')
    r.put(ref.CTX+0x100,ref.SPS,'Q');r.put(ref.CTX+0xe390,ref.PIC,'Q');r.put(ref.SPS+1,2,'B');r.put(ref.SPS+0x40,4)
    r.put(ref.CTX+0x18,1);r.put(ref.CTX+0x1e,3,'B');r.put(ref.DEC+0x18,ref.MMIO,'Q');r.put(ref.DEC+0x1088,ref.SHADOW,'Q')
    orders=[]
    for i in range(2):
        r.put(ref.CTX+0xe4c8,1-i,'B');r.put(ref.CTX+0x4c,i)
        r.call('H264InitPoc',ref.CTX)
        actual=struct.unpack('<3I',r.u.mem_read(ref.PIC,12));assert actual==(2*i,)*3;orders.append(actual)
        frame=0x134000
        for field,value in [(4,i),(8,2*i),(12,2*i),(16,0),(20,0),(24,0)]:r.put(frame+field,value)
        r.call('H264ConfigureRefFrameRegister',ref.DEC,frame)
    (out/'a733-reference-trace.json').write_text(json.dumps(dict(poc=orders,sram=r.trace,pins=ref.PINS,scope='Original A733 instructions, inert MMIO, no DMA'),indent=2)+'\n')
    (out/'oracle-result.json').write_text(json.dumps(dict(passed=True,physical_access=False,plans=plans,ffmpeg_fields=sets,input_sha256=hashlib.sha256(raw).hexdigest(),pixel_sha256=hashlib.sha256((out/'reference.yuv').read_bytes()).hexdigest()),indent=2)+'\n')
    print('PASS independent P syntax, refusal, controller fields, A733 POC producer and SRAM semantics')

if __name__=='__main__':main()
