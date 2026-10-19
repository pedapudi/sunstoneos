#!/usr/bin/env python3
"""Use independent FFmpeg field positions for VGA syntax and admission checks."""
import argparse
import json
from pathlib import Path
import re
import subprocess

def fields(text):
    slices=[];current={};parameter={}
    for line in text.splitlines():
        if 'Slice Header' in line:
            if 'slice_type' in current:slices.append(current)
            current={}
        m=re.search(r'\]\s+(\d+)\s+(\w+)\s+([01]+)\s+=\s+(-?\d+)\s*$',line)
        if m:
            row=[int(m[1]),len(m[3]),int(m[4])];current[m[2]]=row
            if m[2] in ('direct_8x8_inference_flag','weighted_pred_flag','bottom_field_pic_order_in_frame_present_flag'):parameter.setdefault(m[2],row)
    slices.append(current);return slices,parameter

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--evidence',type=Path,required=True);p.add_argument('--parser',type=Path,required=True)
    p.add_argument('--reject-only',action='store_true',help='Preserve accepted valid-frame checks; execute only new refusal branches.')
    a=p.parse_args();out=a.evidence;slices,parameter=fields((out/'headers.log').read_text());assert len(slices)==6
    if not a.reject_only:
        plans=[]
        for i,f in enumerate(slices):
            r=subprocess.run([a.parser,out/f'qualification/frame-{i:03}.h264',str(f['frame_num'][2])],capture_output=True,text=True);assert r.returncode==0,(i,r.stderr)
            plan=list(map(int,r.stdout.split()));assert plan[:2]==[640,480] and plan[10:13]==[f['frame_num'][2],2*f['frame_num'][2],f['slice_type'][2]%5]
            assert plan[4]==max(start+bits for key,(start,bits,value) in f.items() if key in ('slice_qp_delta','disable_deblocking_filter_idc','slice_alpha_c0_offset_div2','slice_beta_offset_div2'))
            plans.append(plan)
        (out/'parser-oracle.json').write_text(json.dumps(dict(passed=True,physical_access=False,plans=plans,ffmpeg_fields=slices),indent=2)+'\n')
    original=(out/'qualification/frame-000.h264').read_bytes();starts=list(re.finditer(b'\x00\x00\x00\x01',original));assert len(starts)==3
    refusals=[]
    for name,kind in [('direct_8x8_inference_flag',7),('weighted_pred_flag',8),('bottom_field_pic_order_in_frame_present_flag',8)]:
        index=next(i for i,m in enumerate(starts) if original[m.end()]&31==kind);m=starts[index]
        end=starts[index+1].start();nal=original[m.end():end];rbsp=bytearray();zeros=0
        for v in nal:
            if zeros==2 and v==3:zeros=0;continue
            rbsp.append(v);zeros=zeros+1 if v==0 else 0
        bit=parameter[name][0];rbsp[bit//8]^=1<<(7-bit%8)
        encoded=bytearray();zeros=0
        for v in rbsp:
            if zeros==2 and v<=3:encoded.append(3);zeros=0
            encoded.append(v);zeros=zeros+1 if v==0 else 0
        changed=original[:m.end()]+encoded+original[end:];path=out/(name+'-refused.h264');path.write_bytes(changed)
        r=subprocess.run([a.parser,path,'0','invalid'],capture_output=True,text=True);assert r.returncode==0,(name,r.stderr);refusals.append(name)
    path=out/'oversize-refused.h264';path.write_bytes(original+bytes(524289-len(original)))
    r=subprocess.run([a.parser,path,'0','invalid'],capture_output=True,text=True);assert r.returncode==0,r.stderr;refusals.append('AU size524289')
    (out/'parser-refusal.json').write_text(json.dumps(dict(passed=True,physical_access=False,refusals=refusals,field_positions=parameter),indent=2)+'\n')
    print('PASS VGA bounds and independent-header refusal cases')
if __name__=='__main__':main()
