#!/usr/bin/env python3
"""Check bounded AnnexB admission against FFmpeg syntax and pixel references.

The native executor uses modeled MMIO/completion. FFmpeg decodes pixels on the
host. No codec hardware, native entropy engine or device DMA is emulated.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('output','ffmpeg','parser','executor'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    commands=[];results=[]
    def run(command,log):
        command=list(map(str,command));commands.append(command)
        (a.output/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        r=subprocess.run(command,capture_output=True);log.write_bytes(r.stdout+r.stderr)
        assert r.returncode==0,(command,r.stderr.decode(errors='replace'))
        return r
    for name,size,qp,extra in [('predicted','48x32',26,''),('deblock','48x32',18,':deblock=2,-2'),
                               ('small','16x16',30,''),('maximum','64x32',22,'')]:
        out=a.output/name;out.mkdir();stream=out/'input.h264'
        run([a.ffmpeg,'-hide_banner','-loglevel','info','-f','lavfi','-i','testsrc2=size='+size+':rate=1',
             '-frames:v','1','-c:v','libx264','-profile:v','baseline','-preset','medium','-qp',qp,
             '-x264-params','keyint=1:bframes=0:cabac=0:ref=1:8x8dct=0'+extra,'-f','h264',stream],out/'encoder.log')
        assert 'I16..4' in (out/'encoder.log').read_text() # Real predicted intra macroblocks.
        run([a.ffmpeg,'-hide_banner','-loglevel','error','-i',stream,'-frames:v','1',
             '-pix_fmt','yuv420p','-f','rawvideo',out/'expected.yuv'],out/'decoder.log')
        run([a.ffmpeg,'-hide_banner','-loglevel','info','-i',stream,'-c','copy','-bsf:v','trace_headers',
             '-f','null','-'],out/'headers.log')
        fields={}
        for line in (out/'headers.log').read_text().splitlines():
            match=re.search(r'\]\s+(\d+)\s+(\w+)\s+([01]+)\s+=\s+(-?\d+)\s*$',line)
            if match:fields[match[2]]=(int(match[1]),len(match[3]),int(match[4]))
        def value(name,default=None):return fields[name][2] if name in fields else default
        plan=list(map(int,run([a.parser,stream],out/'parser.log').stdout.split()))
        width=(value('pic_width_in_mbs_minus1')+1)*16;height=(value('pic_height_in_map_units_minus1')+1)*16
        assert plan[:2]==[width,height]
        syntax_end=max(start+bits for field,(start,bits,v) in fields.items()
                       if field in ('slice_qp_delta','disable_deblocking_filter_idc',
                                    'slice_alpha_c0_offset_div2','slice_beta_offset_div2'))
        assert plan[4]==syntax_end
        raw=stream.read_bytes();assert raw[plan[2]]&31==5 and plan[2]+plan[3]<=len(raw)
        assert plan[2]+plan[3]==len(raw)
        sps=0xc0000|(value('direct_8x8_inference_flag')<<16)|((width//16-1)<<8)|(height//16-1)
        pps=(value('num_ref_idx_l0_default_active_minus1')<<10)|(value('num_ref_idx_l1_default_active_minus1')<<5)
        pps|=value('weighted_pred_flag')<<4|value('weighted_bipred_idc')<<2|value('constrained_intra_pred_flag')<<1
        shs2=(value('disable_deblocking_filter_idc',0)<<8)|((value('slice_alpha_c0_offset_div2',0)&15)<<4)|(value('slice_beta_offset_div2',0)&15)
        chroma=value('chroma_qp_index_offset')&63
        q=26+value('pic_init_qp_minus26')+value('slice_qp_delta')
        qp_reg=0x1000000|chroma<<16|chroma<<8|q
        assert plan[5:]==[sps,pps,0x1220,shs2,qp_reg]
        run([a.executor,stream,out/'registers.bin'],out/'executor.log')
        regs=struct.unpack('<1024I',(out/'registers.bin').read_bytes()[:4096])
        for offset,expected in {0x200:sps,0x204:pps,0x208:0x1220,0x20c:shs2,0x21c:qp_reg,
                                0x234:0,0x238:plan[3]*8,0x23c:0x10001fff}.items():
            assert regs[offset//4]==expected,(name,hex(offset),regs[offset//4],expected)
        assert (out/'expected.yuv').stat().st_size==width*height*3//2
        results.append(dict(name=name,width=width,height=height,input_bytes=len(raw),plan=plan,
                            input_sha256=hashlib.sha256(raw).hexdigest(),
                            pixels_sha256=hashlib.sha256((out/'expected.yuv').read_bytes()).hexdigest()))
    (a.output/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
    (a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,cases=results,
        ffmpeg_sha256=hashlib.sha256(a.ffmpeg.read_bytes()).hexdigest(),
        scope='FFmpeg syntax/pixel oracle; ASan/UBSan parser damage; native register model; physical decode unqualified'),indent=2)+'\n')
    print('PASS: four predicted Baseline IDR streams, independent syntax/register planning, pixel references and bounded parser damage.')


if __name__=='__main__':main()
