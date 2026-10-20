#!/usr/bin/env python3
"""Upload indexed textured triangles and copy frames from an owned GPU session."""
import math
import struct
import time

STATUS,BEGIN,WRITE,SUBMIT,READ_FRAME,CLOSE=range(6)
CHUNK=65536

def session(base,width=1920,height=1080,profile=2):
    """Append GPU-only geometry before the coordinator authenticates the session."""
    if profile not in (2,3) or len(base)!=592 or not 0<width<=1920 or not 0<height<=1080:
        raise ValueError('GPU texture session geometry')
    return base+struct.pack('<5Q',0x4153373347545831,profile-1,632,width,height)

def input_bytes(vertices,indices,width,height,pixels,*,profile=2,state=0):
    """Build position/UV vertices, uint16 indices and padded linear RGBA8 texels."""
    if not 1<=len(vertices)<=65536 or not 3<=len(indices)<=65535 or len(indices)%3:
        raise ValueError('GPU indexed geometry bounds')
    if not 0<width<=1920 or not 0<height<=1080:
        raise ValueError('GPU texture bounds')
    if not state_valid(profile,state):raise ValueError('GPU depth/blend state')
    stride=(width+1)&~1
    if len(pixels)!=width*height*4:raise ValueError('GPU RGBA8 texture length')
    result=bytearray()
    for row in vertices:
        if len(row)!=6 or not all(math.isfinite(x) for x in row) or not all(-1<=x<=1 for x in row[:2]) or not ((row[2]==0 if profile==2 else 0<=row[2]<=1) and row[3]==1) or not all(0<=x<=1 for x in row[4:]):
            raise ValueError('GPU position/UV vertex')
        result.extend(struct.pack('<6f',*row))
    for index in indices:
        if type(index) is not int or not 0<=index<len(vertices):raise ValueError('GPU vertex index')
        result.extend(struct.pack('<H',index))
    for y in range(height):
        result.extend(pixels[y*width*4:(y+1)*width*4]);result.extend(bytes((stride-width)*4))
    shape=(width,height,stride,len(vertices),len(indices))
    return (shape,bytes(result),state) if profile==3 else (shape,bytes(result))

def state_valid(profile,state):
    return type(state) is int and (state==0 if profile==2 else profile==3 and 0<=state<16 and (state&3 or not state&4))

def request(operation,frame,epoch,generation=0,offset=0,payload=b'',shape=(0,0,0,0,0),state=0,profile=2):
    if len(payload)>CHUNK:raise ValueError('GPU copied chunk bound')
    return struct.pack('<4I2Q8I',0x49585047,5 if profile==3 else 4,operation,frame,epoch,generation,offset,len(payload),*shape,state)+payload

def frames(write,read,epoch,inputs,timeout=50,*,profile=2):
    """Use supplied bounded transport functions; stream each frame before reuse.

    write(bytes, remaining_seconds) sends one request. read(maximum_bytes,
    remaining_seconds) returns a nonempty prefix. Neither function opens a device.
    """
    if profile not in (2,3) or not 0<epoch<2**64 or not 0<timeout<60:raise ValueError('GPU session deadline')
    end=time.monotonic()+timeout
    def remaining():
        n=end-time.monotonic()
        if n<=0:raise TimeoutError('GPU texture operation deadline')
        return n
    def exact(n):
        out=bytearray();boundary=False
        while len(out)<n:
            part=bytes(read(min(CHUNK,n-len(out)),remaining()))
            if not part:
                # TinyUSB's FIFO path emits one automatic ZLP after a
                # packet-aligned burst. It may delimit consecutive chunks or
                # remain pending before the next response header.
                if boundary:raise ValueError('Repeated empty GPU transfer')
                boundary=True;continue
            if len(part)>n-len(out):raise ValueError('GPU output stream length')
            boundary=False
            out.extend(part)
        remaining();return bytes(out)
    def exchange(operation,frame,generation=0,offset=0,payload=b'',shape=(0,0,0,0,0),state=0):
        raw=request(operation,frame,epoch,generation,offset,payload,shape,state,profile)
        if write(raw,remaining())!=len(raw):raise ValueError('Short GPU texture request')
        # Ask for one byte beyond the fixed header: at full speed its 64-byte
        # packet must terminate with TinyUSB's ZLP. At high speed the same
        # header is already a short packet. Native code publishes body chunks
        # only after this header transfer completes.
        header=bytes(read(65,remaining()))
        if not header:header=bytes(read(65,remaining()))
        if len(header)!=64:raise ValueError('GPU response header transfer')
        h=struct.unpack('<12I2Q',header)
        if h[:2]!=(0x4f585047,5 if profile==3 else 4) or h[3]!=operation or h[12]!=epoch or h[9]:
            raise ValueError('GPU texture response identity')
        return h
    h=exchange(STATUS,0)
    if h[2]!=0 or h[4]!=0:raise ValueError('GPU texture session already used')
    outputs=[];current=0
    for item in inputs:
        shape,data=item[:2];state=item[2] if len(item)==3 else 0
        if not state_valid(profile,state):raise ValueError('GPU depth/blend state')
        if current>=(2**32-1)//2:raise ValueError('GPU frame sequence exhausted')
        h=exchange(BEGIN,current,shape=shape,state=state)
        if h[2]!=1 or h[4]!=current:raise ValueError('GPU input draft refused')
        generation=h[13]
        for offset in range(0,len(data),CHUNK):
            h=exchange(WRITE,current,generation,offset,data[offset:offset+CHUNK])
            if h[2]!=1 or h[13]!=generation:raise ValueError('GPU upload refused')
        h=exchange(SUBMIT,current,generation)
        current+=1
        if h[2]!=1 or h[4]!=current:raise ValueError('GPU draw incomplete')
        h=exchange(READ_FRAME,current)
        width,height,stride,total=h[5:9]
        if h[2]!=1 or h[4]!=current or not 0<width<=1920 or not 0<height<=1080 or stride!=(width+1)&~1 or total!=stride*height*4 or h[10]!=total:
            raise ValueError('GPU framebuffer geometry')
        outputs.append({'width':width,'height':height,'stride':stride,'pixels':exact(total)})
    h=exchange(CLOSE,current)
    if h[2]!=1 or h[4]!=current or h[11]!=1:raise ValueError('GPU shutdown incomplete')
    return outputs

def capture_frames(device,epoch,inputs,timeout=50,*,profile=2):
    """Use the coordinator's identity-validated, locked, already running handle."""
    def write(data,remaining):return device.write(0x02,data,timeout=max(1,int(remaining*1000)))
    def read(n,remaining):return device.read(0x81,n,timeout=max(1,int(remaining*1000)))
    return frames(write,read,epoch,inputs,timeout,profile=profile)
