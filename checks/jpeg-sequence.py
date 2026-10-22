#!/usr/bin/env python3
"""Repeated JPEG decoding in one sequence session.

Produces version 3 sessions and uploads with apps/jpeg/prepare.py, then runs
the actual input code under address and undefined-behavior sanitizers for
nominal and refused uploads. The linked image then executes USB ingestion,
the result serializer, delivery tracking, the owner task's next-job
preparation and the executor's next-job entry; device pixels, copies of
earlier images and provider observations are modeled as in
checks/jpeg-runtime-native.py. No device, board or USB is accessed.
"""
import argparse,hashlib,importlib.util,json,struct,subprocess
from pathlib import Path
from unicorn import UC_HOOK_CODE
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
prep=module('jpeg_sequence_prepare',ROOT/'apps/jpeg/prepare.py')
observe=module('jpeg_sequence_observe',ROOT/'apps/jpeg/observe.py')
inputs=module('jpeg_sequence_inputs',ROOT/'checks/jpeg-runtime-input.py')
native=module('jpeg_sequence_native',ROOT/'checks/jpeg-runtime-native.py')
COPIED,PARSE=13,2

def images():
    nonconstant=Path.home()/'.cache/sunstoneos/evidence/jpeg-nonconstant-20261017/oracle'
    count=lambda w,h:w*h+2*((w+1)//2)*((h+1)//2)
    return [(inputs.constant(64,48),b''),((nonconstant/'fixture.jpg').read_bytes(),(nonconstant/'reference.yuv').read_bytes()),
            (inputs.constant(640,480),bytes([128])*count(640,480)),(inputs.constant(33,17),b'')]
def session(image,manifest,version=3,reference=bytes(32)):
    raw=bytearray((Path.home()/'.cache/sunstoneos/evidence/jpeg-sealing-20261018/session.bin').read_bytes());assert len(raw)==856
    _,_,identity=prep.base.image_parts(image);raw[48:80]=identity;struct.pack_into('<Q',raw,600,version)
    raw[768:800]=hashlib.sha256(manifest).digest();raw[800:832]=reference;return bytes(raw)
def mapped(manifest):
    count=struct.unpack_from('<I',manifest,8)[0];page=lambda n:(n+4095)&~4095;out=[0,0,0]
    for i in range(count):
        n,_,w,h=struct.unpack_from('<4I',manifest,16+80*i);luma=((w+31)&~31)*((h+31)&~31)
        out=[max(out[0],n),max(out[1],luma),max(out[2],luma//2)]
    return [page(x) for x in out]
def step(data,expect,deliver=0,width=0,height=0):return struct.pack('<5I',len(data),expect&0xffffffff,deliver,width,height)+data

def input_cases(image,out,executable):
    pictures=images();manifest=prep.sequence_manifest([j for j,_ in pictures],[r for _,r in pictures])
    raw=session(image,manifest);upload=prep.sequence_upload(raw,manifest);sizes=[prep.jpeg_size(j) for j,_ in pictures]
    frames=[prep.image_upload(raw,manifest,i,j,r) for i,(j,r) in enumerate(pictures)]
    nominal=[step(upload,0)]+[step(f,1,int(i>0),*sizes[i]) for i,f in enumerate(frames)]
    wrong=bytearray(manifest);struct.pack_into('<I',wrong,28,sizes[0][1]+16);wrong=bytes(wrong)
    corrupt=bytearray(frames[1]);corrupt[40]^=1
    nine=prep.sequence_manifest([pictures[0][0]]*8,[b'']*8);nine=nine[:8]+struct.pack('<I',9)+nine[12:]+nine[16:96]
    cases={'nominal':(raw,nominal),
        'undelivered-next-image':(raw,[step(upload,0),step(frames[0],1,0,*sizes[0]),step(frames[1],-1)]),
        'out-of-order-image':(raw,[step(upload,0),step(frames[1],-1)]),
        'changed-image-bytes':(raw,[step(upload,0),step(frames[0],1,0,*sizes[0]),step(bytes(corrupt),-1,1)]),
        'manifest-geometry-differs':(session(image,wrong),[step(prep.sequence_upload(session(image,wrong),wrong),0),step(prep.image_upload(session(image,wrong),wrong,0,*pictures[0]),-1)]),
        'image-after-count':(raw,nominal+[step(frames[0][:4],-1,1)]),
        'version2-session':(session(image,manifest,2),[step(upload,-1)]),
        'sequence-with-session-oracle':(session(image,manifest,3,bytes([1])*32),[step(upload,-1)]),
        'single-upload-in-sequence':(raw,[step(struct.pack('<4I2Q',0x5547504a,1,len(pictures[0][0]),0,*struct.unpack_from('<2Q',raw,24))+pictures[0][0],-1)]),
        'manifest-digest-differs':(raw,[step(upload[:-1]+bytes([upload[-1]^1]),-1)]),
        'nine-images':(session(image,nine),[step(struct.pack('<4I2Q',0x5547504a,2,len(nine),0,*struct.unpack_from('<2Q',raw,24))+nine,-1)])}
    report=[]
    for name,(s,steps) in cases.items():
        d=out/name;d.mkdir();(d/'session.bin').write_bytes(s);(d/'steps.bin').write_bytes(b''.join(steps))
        for chunk in ((1,17,512) if name=='nominal' else (512,)):
            command=list(map(str,[executable,d/'session.bin',d/'steps.bin',chunk,*mapped(manifest)]))
            r=subprocess.run(command,capture_output=True,text=True);(d/f'chunk-{chunk}.log').write_text(r.stdout+r.stderr)
            assert r.returncode==0,(name,chunk,r.stdout,r.stderr)
            expected=f'commits={4 if name in ("nominal","image-after-count") else 1 if name in ("undelivered-next-image","changed-image-bytes") else 0} rejected={0 if name=="nominal" else 1}'
            assert r.stdout.strip()==expected,(name,r.stdout)
        report.append(dict(case=name,result=r.stdout.strip()))
    return report,pictures,manifest

class Machine(native.Machine):
    def __init__(self,image,raw):
        super().__init__(image,raw,None);address=image.address('board_fail');self.hooks[address]='board_fail'
        self.cpu.hook_add(UC_HOOK_CODE,self.hook,begin=address,end=address)
        shared=image.address('providers')+image.layouts['board_provider']['shared'];self.write(shared+image.layouts['a733_native_usb']['pmic_ready'],1)
    def send(self,packet):
        for offset in range(0,len(packet),17):
            self.incoming.extend(packet[offset:offset+17])
            while self.incoming and 'board_fail' not in self.calls:self.call('board_usb0_task',0)
    def backing(self,sizes):
        # Guard page, mapped capacity and guard page per buffer, sized from the
        # largest manifest entries as the native allocator sizes them.
        address=0x50000000;self.extents=[]
        for name,n in zip(('input','luma','chroma'),sizes):
            self.extents.append((address,n+8192));self.write(self.base+self.layout[name],address+4096,8)
            self.cpu.mem_write(address,bytes([0xa5])*(n+8192));address+=n+8192
    def decoded(self,pixels):
        """Write device output for the current image, copy it and publish."""
        u=lambda f,n=4:self.read(self.supplied+self.il[f],n);w,h,stride,rows=u('width'),u('height'),u('stride'),u('rows');cw,ch=(w+1)//2,(h+1)//2
        luma=self.extents[1][0]+4096;chroma=self.extents[2][0]+4096
        for row in range(h):self.cpu.mem_write(luma+row*stride,pixels[row*w:(row+1)*w])
        for plane in range(2):
            for row in range(ch):
                offset=w*h+plane*cw*ch+row*cw;self.cpu.mem_write(chroma+plane*(stride*rows//4)+row*(stride//2),pixels[offset:offset+cw])
        fault=self.image.layouts['a733_iommu_fault'];iommu=self.image.layouts['a733_iommu'];self.write(self.base+self.layout['iommu']+iommu['fault']+fault['scanned_masters'],0xc0,2)
        calls=[]
        while not calls or calls[-1]==0:calls.append(self.call('jpeg_copied',self.base,13,990000));assert len(calls)<20
        assert calls[-1]==1 and bytes(self.cpu.mem_read(self.base+self.layout['payload'],len(pixels)))==pixels;self.write(self.base+self.layout['result'],1)
        return len(calls)
    def frame(self):
        self.incoming.extend(b'\0');self.call('board_usb0_task',0)
        length=self.read(self.image.address('jpeg_response_bytes'));assert 160<=length<=160+observe.MAXIMUM
        return bytes(self.cpu.mem_read(self.image.address('jpeg_response'),length))
    def deliver(self):
        n=self.read(self.image.address('jpeg_response_bytes'));self.write(self.image.address('jpeg_response_sent'),n);self.call('tud_vendor_tx_cb',0,n)
        assert self.read(self.image.address('jpeg_response_bytes'))==0
    def retained_run(self,sizes,length):
        """State of an executor whose previous job reached COPIED."""
        run=self.base+self.layout['run'];rl=self.image.layouts['a733_video_jpeg_run'];jl=self.image.layouts['a733_video_jpeg_job'];job=run+rl['job']
        for field,value,n in [('input',self.extents[0][0]+4096,8),('file_bytes',length,4),('input_iova',0x10001000,8),('input_bytes',sizes[0],8),
                              ('luma_iova',0x10100000,8),('luma_bytes',sizes[1],8),('chroma_iova',0x10180000,8),('chroma_bytes',sizes[2],8)]:self.write(job+jl[field],value,n)
        self.write(run+rl['state'],COPIED);self.write(run+rl['retained_readout'],1)

def native_cases(image_path,pictures,manifest):
    image=native.Image(image_path);raw=session(image_path,manifest);sizes=mapped(manifest);report={}
    frames=[prep.image_upload(raw,manifest,i,j,r) for i,(j,r) in enumerate(pictures)]
    def start():
        m=Machine(image,raw);m.send(prep.sequence_upload(raw,manifest));assert m.call('jpeg_input_ready',m.supplied)==0
        m.send(frames[0]);assert m.call('jpeg_input_ready',m.supplied)==1 and 'board_fail' not in m.calls
        assert [m.read(m.supplied+m.il['mapped']+4*i) for i in range(3)]==sizes
        m.backing(sizes);m.write(m.base+m.layout['stage'],1);m.write(m.base+m.layout['decoded'],1);return m
    m=start();rl=image.layouts['a733_video_jpeg_run'];run=m.base+m.layout['run']
    for index,(jpeg,reference) in enumerate(pictures):
        w,h=prep.jpeg_size(jpeg);count=observe.geometry(w,h,(w+31)&~31,(h+31)&~31);pixels=reference or bytes([(index*37+7)&255])*count
        copies=m.decoded(pixels);frame=m.frame();value=observe.decode_sequence(frame,raw,manifest,index,reference or None)
        assert value['status']==1 and frame[160:]==pixels and value['width']==w and value['height']==h
        m.deliver();assert m.read(m.supplied+m.il['delivered'])==index+1
        report[f'image-{index}']=dict(width=w,height=h,bytes=count,copy_calls=copies,oracle_verified=value['oracle_verified'])
        if index+1==len(pictures):break
        m.send(frames[index+1]);assert m.call('jpeg_input_ready',m.supplied)==index+2 and m.read(m.base+m.layout['result'])==0
        assert observe.decode_sequence(m.frame(),raw,manifest,index+1,pictures[index+1][1] or None)['status']==0
        m.deliver();assert m.read(m.supplied+m.il['delivered'])==index+1
        # The owner task rewrites input and poison under the same mappings and
        # starts the next executor job.
        m.retained_run(sizes,len(jpeg));m.call('jpeg_task',0)
        nxt=pictures[index+1][0];base=m.extents[0][0]+4096
        assert m.read(run+rl['state'])==PARSE and m.read(run+rl['job']+image.layouts['a733_video_jpeg_job']['file_bytes'])==len(nxt)
        assert m.read(m.base+m.layout['decoded'])==index+2 and m.read(m.base+m.layout['copy_step'])==0 and m.read(m.base+m.layout['copy_row'])==0
        assert bytes(m.cpu.mem_read(base,sizes[0]))==nxt+bytes(sizes[0]-len(nxt))
        assert bytes(m.cpu.mem_read(base-4096,4096))==bytes([0xa5])*4096 and bytes(m.cpu.mem_read(base+sizes[0],4096))==bytes([0xa5])*4096
        for b in (1,2):assert bytes(m.cpu.mem_read(m.extents[b][0],sizes[b]+8192))==bytes([0xa5])*(sizes[b]+8192)
    assert 'board_fail' not in m.calls
    m.send(frames[0][:1]);assert 'board_fail' in m.calls;report['frame-after-count']=dict(stopped=True)
    # A frame before the previous result is delivered stops the lifetime and
    # leaves the published image and its success result in place.
    m=start();m.decoded(bytes([7])*observe.geometry(64,48,64,64));m.send(frames[1][:32])
    assert 'board_fail' in m.calls and m.call('jpeg_input_ready',m.supplied)==1 and m.read(m.base+m.layout['result'])==1
    report['undelivered-next-image']=dict(stopped=True,published=1)
    # A failure recorded before the next image arrives is never replaced.
    m=start();m.decoded(bytes([7])*observe.geometry(64,48,64,64));m.frame();m.deliver();m.write(m.base+m.layout['result'],2);m.send(frames[1])
    assert 'board_fail' in m.calls and m.read(m.base+m.layout['result'])==2;report['failure-kept']=dict(stopped=True)
    # The owner refuses a next job that does not follow the jobs it started.
    m=start();m.decoded(bytes([7])*observe.geometry(64,48,64,64));m.frame();m.deliver();m.send(frames[1]);m.retained_run(sizes,len(pictures[0][0]))
    m.write(m.base+m.layout['decoded'],0);m.call('jpeg_task',0)
    assert m.read(m.base+m.layout['result'])==2 and m.read(run+rl['state'])==COPIED;report['next-count-mismatch']=dict(result=2)
    return report

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','output','executable'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    report,pictures,manifest=input_cases(a.image,a.output,a.executable)
    composed=native_cases(a.image,pictures,manifest)
    (a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),
        input_cases=report,native_cases=composed,scope='Actual sequence upload producer, sanitized input code and linked USB ingestion, serializer, delivery, owner next-job preparation and executor entry. Device pixels, decoder completion and provider observations are modeled. No physical decoding, drain or numerical qualification.'),indent=2)+'\n')
    print('PASS JPEG sequence sessions: four images in one lifetime, delivery gating, next-job buffer refill and refusals')
if __name__=='__main__':main()
