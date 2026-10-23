#!/usr/bin/env python3
"""Explicit dispose/reopen consumers for owned triangle and texture sessions."""
import importlib.util
from pathlib import Path
import struct
import time

ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
common=module('gpu_lifecycle_common',ROOT/'apps/gpu-program/lifecycle.py')
draw=module('gpu_lifecycle_draw',Path(__file__).with_name('draw.py'))
texture=module('gpu_lifecycle_texture',Path(__file__).with_name('texture.py'))


class Triangle(common.Compute):
    version=7;page=64;initial_sequence=0;maximum_sequence=draw.MAX_FRAMES;maximum_words=4096
    def __init__(self,exchange,epoch,*,deadline,retain):
        super().__init__(exchange,epoch,4096,deadline=deadline,retain=retain);self.profile='triangle'

    def _input(self,data):
        if len(data)%32 or not 96<=len(data)<=4032 or (len(data)//32)%3:
            raise ValueError('GPU lifecycle triangle input bounds')
        # Reuse the legacy bounded finite position/color validation.
        rows=list(struct.iter_unpack('<8f',data))
        if draw.vertices(rows)!=data:raise ValueError('GPU lifecycle triangle input differs')

    @classmethod
    def for_device(cls,device,epoch,*,deadline,retain):
        def exchange(request,remaining):
            if device.write(0x02,request,timeout=max(1,int(remaining*1000)))!=len(request):
                raise ValueError('Short GPU triangle lifecycle request')
            remaining=deadline-time.monotonic()
            if remaining<=0:raise TimeoutError('GPU triangle lifecycle deadline after write')
            return device.read(0x81,304,timeout=max(1,int(remaining*1000)))
        return cls(exchange,epoch,deadline=deadline,retain=retain)


class Texture(common.Session):
    close_op=5;dispose_op=6;open_op=7;initial_sequence=0
    def __init__(self,write,read,epoch,width,height,*,deadline,retain,profile=2):
        if not callable(write) or not callable(read) or profile not in (2,3,4) or not 0<width<=1920 or not 0<height<=1080:
            raise ValueError('GPU lifecycle texture transport or geometry')
        super().__init__(epoch,deadline,retain,{2:'texture',3:'depth',4:'batch'}[profile])
        self.write=write;self.receive=read;self.graphics_profile=profile;self.version=texture.VERSIONS[profile]+4
        self.width=width;self.height=height;self.stride=(width+1)&~1;self.total=self.stride*height*4
        self.generation=0

    def identity(self):
        result=super().identity()
        result.update(width=self.width,height=self.height,stride=self.stride,generation=self.generation)
        return result

    def _call(self,op,seq,first=0,payload=b'',queue_epoch=None,*,generation=0,shape=(0,0,0,0,0),state=0):
        q=self.queue_epoch if queue_epoch is None else queue_epoch
        raw=struct.pack('<4I2Q8IQ',0x49585047,self.version,op,seq,self.session_epoch,generation,
                        first,len(payload),*shape,state,q)+payload
        self.last_request=raw;self.last_receipt=None
        if self.write(raw,self.remaining())!=len(raw):raise ValueError('Short GPU texture lifecycle request')
        # The separate 72-byte header is short at both supported USB speeds.
        # A prior full body publication may leave its one terminating ZLP.
        header=bytes(self.receive(512,self.remaining()))
        if not header:header=bytes(self.receive(512,self.remaining()))
        self.last_response=header
        if len(header)!=72:raise ValueError('GPU texture lifecycle header extent')
        h=struct.unpack('<12I3Q',header)
        if h[:2]!=(0x4f585047,self.version) or h[3]!=op or h[12]!=self.session_epoch or h[9]:
            raise ValueError('GPU texture lifecycle identity')
        if h[2] not in range(6) or h[4]>draw.MAX_FRAMES or h[5:9]!=(self.width,self.height,self.stride,self.total) or h[11] not in (0,1):
            raise ValueError('GPU texture lifecycle geometry/state')
        if h[10]!=(self.total if op==4 and h[2]==1 else 0):raise ValueError('GPU texture lifecycle acquired extent')
        if h[2] in (3,5) and h[11]!=1:raise ValueError('GPU texture lifecycle lacks CLOSED proof')
        if op==7 and h[11]!=(0 if h[2]==1 else 1):raise ValueError('GPU texture OPEN closed flag differs')
        expected_generation=0 if op in (0,7) and h[14]==self.queue_epoch+1 and h[2]==1 else self.generation
        if self.queue_epoch and op in (0,4,5,6,7) and h[13]!=expected_generation:
            raise ValueError('GPU texture lifecycle generation changed')
        if op==6 and h[11]!=1:raise ValueError('GPU DISPOSE lost CLOSED proof')
        if op in (0,7) and self.queue_epoch and h[14]==self.queue_epoch+1 and h[2]==1 and (h[13] or h[11]):
            raise ValueError('GPU OPEN retained a stale draft or CLOSED flag')
        reply={'status':h[2],'sequence':h[4],'queue_epoch':h[14],'generation':h[13],
               'closed':h[11],'bytes':h[10]}
        self.last_receipt=reply;return reply

    def _accept(self,reply,*,epochs=None):
        reply=super()._accept(reply,epochs=epochs)
        return reply

    def _observed(self,reply):
        super()._observed(reply);self.generation=reply['generation']

    @common.operation
    def submit(self,item):
        shape,data=item[:2];state=item[2] if len(item)==3 else 0;data=bytes(data)
        if len(shape)!=5 or not texture.state_valid(self.graphics_profile,state):raise ValueError('GPU texture lifecycle input state')
        width,height,stride,vertices,indices=shape
        if not (0<width<=1920 and 0<height<=1080 and stride==(width+1)&~1 and 1<=vertices<=65536 and 3<=indices<=65535 and indices%3==0):
            raise ValueError('GPU texture lifecycle input geometry')
        batches=(state&31)*16 if self.graphics_profile==4 else 0
        if len(data)!=vertices*24+indices*2+batches+stride*height*4:raise ValueError('GPU texture lifecycle input extent')
        self._ready()
        if self.state!=1 or self.sequence>=draw.MAX_FRAMES:raise ValueError('GPU texture submission state')
        self._protect_output();previous=self.sequence;old_generation=self.generation
        reply=self._accept(self._call(1,previous,shape=shape,state=state))
        generation=reply['generation']
        if reply['status']!=1 or reply['sequence']!=previous or generation<=old_generation:
            raise ValueError('GPU texture BEGIN did not publish fresh draft')
        self.generation=generation
        for offset in range(0,len(data),texture.CHUNK):
            reply=self._accept(self._call(2,previous,offset,data[offset:offset+texture.CHUNK],generation=generation))
            if reply['status']!=1 or reply['sequence']!=previous or reply['generation']!=generation:
                raise ValueError('GPU texture WRITE generation differs')
        reply=self._accept(self._call(3,previous,generation=generation))
        current=previous+1
        if reply['status']!=1 or reply['sequence']!=current or reply['generation']!=generation:
            raise ValueError('GPU texture COMMIT incomplete')
        self.sequence=current;self.generation=generation;self.state=1
        reply=self._accept(self._call(4,current))
        if reply['status']!=1 or reply['sequence']!=current or reply['generation']!=generation:
            raise ValueError('GPU texture READ identity differs')
        self.partial=bytearray();boundary=False
        while len(self.partial)<self.total:
            part=bytes(self.receive(min(texture.CHUNK,self.total-len(self.partial)),self.remaining()))
            if not part:
                if boundary:raise ValueError('Repeated empty GPU texture transfer')
                boundary=True;continue
            if len(part)>self.total-len(self.partial):raise ValueError('GPU texture copied stream extent')
            boundary=False;self.partial.extend(part)
        output=bytes(self.partial);self.outputs.append({'identity':self.identity(),'bytes':output})
        self._persist('output',output);self.copied=(self.queue_epoch,current);self.partial.clear()
        return {'width':self.width,'height':self.height,'stride':self.stride,'pixels':output}

    @classmethod
    def for_device(cls,device,epoch,width,height,*,deadline,retain,profile=2):
        def write(data,remaining):return device.write(0x02,data,timeout=max(1,int(remaining*1000)))
        def read(n,remaining):return device.read(0x81,n,timeout=max(1,int(remaining*1000)))
        return cls(write,read,epoch,width,height,deadline=deadline,retain=retain,profile=profile)
