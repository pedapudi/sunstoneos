#!/usr/bin/env python3
"""Check the offline NV16 decoder, color-bar structure test and review image.

The synthetic bars are a model: eight 75% BT.601 bars that darken toward the
bottom. The pinned sources do not define the exact OV5640 0x503d=0x84 output,
so these cases establish only that the structural test accepts a vertical bar
pattern and rejects swapped planes, sheared rows, short frames and flat images.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('camera_observe',ROOT/'apps/camera/observe.py')
observe=importlib.util.module_from_spec(spec);spec.loader.exec_module(observe)
W,H=observe.WIDTH,observe.HEIGHT
BARS=[(180,128,128),(162,44,142),(131,156,44),(112,72,58),(84,184,198),(65,100,212),(35,212,114),(16,128,128)]

def bars():
    y=bytearray(W*H);uv=bytearray(W*H)
    for r in range(H):
        scale=1-r/(2*H)
        for x in range(W):
            Y,U,V=BARS[x*len(BARS)//W]
            y[r*W+x]=round(16+(Y-16)*scale)
            uv[r*W+x]=round(128+((U if x%2==0 else V)-128)*scale)
    return bytes(y)+bytes(uv)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    good=bars();y,uv=good[:W*H],good[W*H:];results={}
    def case(name,image,accepted):
        report=observe.color_bars(image);assert report['accepted']==accepted,(name,report['reasons'])
        results[name]={'accepted':report['accepted'],'reasons':report['reasons'],'bands':report['bands']}
        return report
    report=case('model-bars',good,True)
    assert report['boundaries']==[80*i for i in range(1,8)] and report['stray_edges']==0
    assert report['band_yuv']['0'][0]==[180,128,128] and report['band_yuv']['0'][7]==[16,128,128]
    case('swapped-planes',uv+y,False)
    case('sheared-rows',b''.join(good[o+r*W:o+(r+1)*W][r%W:]+good[o+r*W:o+(r+1)*W][:r%W] for o in (0,W*H) for r in range(H)),False)
    short=bytearray(good)
    for o in (0,W*H):short[o+400*W:o+H*W]=bytes([observe.FILL])*(80*W)
    assert observe.unwritten_rows(bytes(short))==list(range(400,H))
    case('short-frame',bytes(short),False)
    case('flat',bytes([128])*(2*W*H),False)
    try:observe.planes(good[:-1])
    except ValueError:pass
    else:raise AssertionError('Short NV16 frame accepted')
    image=observe.ppm(good);header=b'P6\n640 480\n255\n'
    assert image.startswith(header) and len(image)==len(header)+3*W*H
    rgb=image[len(header):]
    # BT.601 limited range: 75% white is gray 191 and black is 0.
    assert rgb[0:3]==bytes([191,191,191]) and rgb[3*(W-1):3*W]==bytes([0,0,0])
    (a.output/'model-bars.nv16').write_bytes(good);(a.output/'model-bars.ppm').write_bytes(image)
    (a.output/'result.json').write_text(json.dumps({'status':'pass','cases':results,'physical_access':False,
        'observe_sha256':hashlib.sha256((ROOT/'apps/camera/observe.py').read_bytes()).hexdigest(),'scope':__doc__},indent=2)+'\n')
    print('PASS offline NV16 color-bar structure, fill detection and review image')

if __name__=='__main__':main()
