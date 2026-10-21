#!/usr/bin/env python3
"""Decode saved camera responses and NV16 frames offline. Physical capture uses check-a733-camera-supervised.py."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

WIDTH,HEIGHT=640,480
FILL=0xa5  # A733_CAMERA_NATIVE_FILL: plane bytes the DMA has not written.

def decode(frame,session,operation,offset=0,count=0):
    if len(session)<48 or struct.unpack_from('<Q',session,16)[0]!=len(session):
        raise ValueError('Invalid admitted camera session size')
    if operation not in (0,1,2) or not 0<=offset<=614400 or not 0<=count<=min(4096,614400-offset) or (operation!=1 and (offset or count)):
        raise ValueError('Invalid camera request shape')
    if len(frame)<72:raise ValueError('Short camera response')
    magic,version,state,actual,size,total,epoch,generation=struct.unpack_from('<6I2Q',frame)
    if (magic,version,total,epoch,generation)!=(0x524d4143,1,614400,*struct.unpack_from('<2Q',session,24)):
        raise ValueError('camera response identity mismatch')
    if frame[40:72]!=hashlib.sha256(session).digest() or actual!=offset or state not in (0,1,2,3):
        raise ValueError('camera response session, offset or state mismatch')
    expected=count if operation==1 and state==2 else 0
    if size!=expected or len(frame)!=72+size or (operation==2 and state!=3):
        raise ValueError('camera response extent or cancellation mismatch')
    return {'state':state,'offset':actual,'bytes':size,'payload':frame[72:]}

def planes(image):
    """Split NV16: a 640x480 luma plane, then 640x480 interleaved U,V bytes."""
    if len(image)!=2*WIDTH*HEIGHT:raise ValueError('NV16 640x480 frame must be 614400 bytes')
    return image[:WIDTH*HEIGHT],image[WIDTH*HEIGHT:]

def unwritten_rows(image):
    """Rows whose luma and chroma bytes all still hold the native fill."""
    y,uv=planes(image);row=bytes([FILL])*WIDTH
    return [r for r in range(HEIGHT) if y[r*WIDTH:(r+1)*WIDTH]==row and uv[r*WIDTH:(r+1)*WIDTH]==row]

def ppm(image):
    """BT.601 limited-range RGB for human review; not a color-accuracy reference."""
    y,uv=planes(image);out=bytearray(3*WIDTH*HEIGHT)
    for i in range(WIDTH*HEIGHT):
        c,d,e=298*(y[i]-16),uv[i&~1]-128,uv[i|1]-128
        for k,value in enumerate(((c+409*e+128)>>8,(c-100*d-208*e+128)>>8,(c+516*d+128)>>8)):
            out[3*i+k]=min(255,max(0,value))
    return b'P6\n%d %d\n255\n'%(WIDTH,HEIGHT)+bytes(out)

def color_bars(image,threshold=24,tolerance=3):
    """Check vertical-bar structure without assuming the bar count, order or colors.

    Register 0x503d=0x84 selects the OV5640 color bar with vertical change
    (pinned Linux ov5640.c). The pinned sources do not define its bar count,
    order or per-row gradient. This check therefore requires only what any
    vertical bar pattern has: in every row, luma and chroma change only at the
    same few sharp column boundaries, and no row is unwritten fill. It rejects
    swapped planes, sheared rows and short frames; it cannot tell U from V.
    """
    y,uv=planes(image);hits=[0]*WIDTH;rows=[]
    for r in range(HEIGHT):
        line,chroma=y[r*WIDTH:(r+1)*WIDTH],uv[r*WIDTH:(r+1)*WIDTH]
        edges={x for x in range(1,WIDTH) if abs(line[x]-line[x-1])>threshold}
        edges|={x for x in range(2,WIDTH,2) if abs(chroma[x]-chroma[x-2])>threshold or abs(chroma[x+1]-chroma[x-1])>threshold}
        rows.append(edges)
        for x in edges:hits[x]+=1
    columns=[x for x in range(WIDTH) if hits[x]>=HEIGHT//4];groups=[]
    for x in columns:
        if groups and x-groups[-1][-1]<=tolerance:groups[-1].append(x)
        else:groups.append([x])
    boundaries=[(g[0]+g[-1])//2 for g in groups]
    stray=sum(1 for edges in rows for x in edges if all(abs(x-b)>tolerance for b in boundaries))
    widths=[b-a for a,b in zip([0,*boundaries],[*boundaries,WIDTH])]
    report={'bands':len(boundaries)+1,'boundaries':boundaries,'widths':widths,'stray_edges':stray,
            'unwritten_rows':len(unwritten_rows(image))}
    reasons=[]
    if not 1<=len(boundaries)<=15:reasons.append('expected 2 to 16 vertical bands')
    if any(g[-1]-g[0]>2*tolerance for g in groups):reasons.append('band boundary is not sharp')
    if any(w<16 for w in widths):reasons.append('band narrower than 16 pixels')
    if stray>HEIGHT:reasons.append('edges outside the shared column boundaries')
    if report['unwritten_rows']:reasons.append('rows still hold the native fill')
    # Mean Y/U/V per band on the top, middle and bottom rows, for comparison
    # with the reviewed pattern; these values are reported, not judged.
    samples={}
    for r in (0,HEIGHT//2,HEIGHT-1):
        values=[]
        for a,b in zip([0,*boundaries],[*boundaries,WIDTH]):
            a,b=(a+tolerance+1)&~1,(b-tolerance-1)&~1
            if b<=a:values.append(None);continue
            n=b-a;base=r*WIDTH
            values.append([round(sum(y[base+a:base+b])/n),round(sum(uv[base+a:base+b:2])/(n//2)),round(sum(uv[base+a+1:base+b:2])/(n//2))])
        samples[str(r)]=values
    report.update(band_yuv=samples,accepted=not reasons,reasons=reasons)
    return report

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--frame',type=Path,help='one saved USB response')
    parser.add_argument('--session',type=Path);parser.add_argument('--operation',type=int,choices=(0,1,2))
    parser.add_argument('--offset',type=int,default=0);parser.add_argument('--count',type=int,default=0)
    parser.add_argument('--image',type=Path,help='a complete 640x480 NV16 frame, such as frame-640x480-nv16.bin')
    parser.add_argument('--ppm',type=Path,help='write a review image from --image')
    parser.add_argument('--color-bars',action='store_true',help='require the vertical bar structure in --image')
    a=parser.parse_args()
    if a.image:
        if a.frame:parser.error('--image and --frame are separate decoders')
        image=a.image.read_bytes();planes(image)
        result={'bytes':len(image),'sha256':hashlib.sha256(image).hexdigest(),'unwritten_rows':len(unwritten_rows(image))}
        if a.color_bars:result['color_bars']=color_bars(image)
        if a.ppm:a.ppm.write_bytes(ppm(image));result['ppm']=str(a.ppm)
        print(json.dumps(result,indent=2))
        if a.color_bars and not result['color_bars']['accepted']:raise SystemExit(1)
        return
    if not (a.frame and a.session and a.operation is not None):parser.error('--frame, --session and --operation are required')
    result=decode(a.frame.read_bytes(),a.session.read_bytes(),a.operation,a.offset,a.count)
    payload=result.pop('payload');result['payload_sha256']=hashlib.sha256(payload).hexdigest()
    print(json.dumps(result,indent=2))
if __name__=='__main__':main()
