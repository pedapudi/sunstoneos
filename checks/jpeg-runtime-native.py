#!/usr/bin/env python3
"""Execute runtime ingestion, resource gating, dynamic copy and USB serialization."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
class Image:
    def __init__(self,path):
        with path.open('rb') as f:
            elf=ELFFile(f);self.parts=[(int(p['p_vaddr']),p.data()) for p in elf.iter_segments() if p['p_type']=='PT_LOAD'];self.symbols={};self.layouts={}
            for s in elf.get_section_by_name('.symtab').iter_symbols():self.symbols.setdefault(s.name,set()).add(int(s['st_value']))
            for cu in elf.get_dwarf_info().iter_CUs():
                for die in cu.iter_DIEs():
                    name=die.attributes.get('DW_AT_name')
                    if die.tag=='DW_TAG_structure_type' and name:
                        fields={c.attributes['DW_AT_name'].value.decode():int(c.attributes['DW_AT_data_member_location'].value) for c in die.iter_children() if c.tag=='DW_TAG_member' and 'DW_AT_name' in c.attributes and 'DW_AT_data_member_location' in c.attributes}
                        if fields:self.layouts[name.value.decode()]=fields
    def address(self,name):
        values=self.symbols.get(name,set())-{0};assert len(values)==1,(name,values);return next(iter(values))
class Machine:
    def __init__(self,image,session,reference):
        self.image=image;self.cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.cpu.mem_map(0x40000000,0x04000000);self.cpu.mem_map(0x50000000,0x01000000)
        for a,data in image.parts:self.cpu.mem_write(a,data)
        self.base=image.address('jpeg');self.layout=image.layouts['jpeg_application'];self.supplied=self.base+self.layout['supplied'];self.il=image.layouts['jpeg_input'];self.reference=reference
        self.cpu.mem_write(self.base,session);self.cpu.mem_write(image.address('board'),session[:592]);self.write(image.address('board')+160,1000000,8)
        self.calls=[];self.describes=0;self.finished=False;self.fault=False;self.error=False;self.guard=False;self.bad_clock=False;self.incoming=bytearray();self.rails=0
        self.hooks={image.address(n):n for n in ('board_live','a733_counter_clock_now','a733_iommu_scan','a733_iommu_poll','device_memory_describe','jpeg_load','vTaskDelay','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','tud_vendor_n_available','tud_vendor_n_read','a733_rail_acquire')}
        # Hook only the stubbed entry points, so large copies run at native speed.
        for address in [*self.hooks,0x50fff000]:self.cpu.hook_add(UC_HOOK_CODE,self.hook,begin=address,end=address)
        self.extents=[]
    def write(self,a,v,n=4):self.cpu.mem_write(a,int(v).to_bytes(n,'little'))
    def read(self,a,n=4):return int.from_bytes(self.cpu.mem_read(a,n),'little')
    def ret(self,v=0):self.cpu.reg_write(UC_ARM64_REG_X0,v&((1<<64)-1));self.cpu.reg_write(UC_ARM64_REG_PC,self.cpu.reg_read(UC_ARM64_REG_LR))
    def hook(self,cpu,address,size,unused):
        if address==0x50fff000:self.finished=True;cpu.emu_stop();return
        name=self.hooks.get(address)
        if name is None:return
        args=[cpu.reg_read(UC_ARM64_REG_X0+i) for i in range(8)];self.calls.append(name)
        if name=='a733_counter_clock_now':self.write(args[1],1000000 if self.bad_clock else 1000,8);self.ret(1)
        elif name=='a733_rail_acquire':self.rails+=1;self.finished=True;cpu.emu_stop()
        elif name=='vTaskDelay':self.finished=True;cpu.emu_stop()
        elif name=='a733_iommu_poll':self.ret(-1 if self.fault else 1)
        elif name=='device_memory_describe':
            extent,size=self.extents[self.describes%3];self.describes+=1
            self.cpu.mem_write(args[2],struct.pack('<3Q',extent,extent,size));self.write(args[3],2);self.ret(1)
        elif name=='jpeg_load':self.write(args[1],0x8 if self.error else 0);self.ret(1)
        elif name=='tud_vendor_n_available':self.ret(len(self.incoming))
        elif name=='tud_vendor_n_read':
            n=min(args[2],len(self.incoming));self.cpu.mem_write(args[1],bytes(self.incoming[:n]));del self.incoming[:n];self.ret(n)
        else:self.ret(1)
    def call(self,name,*args):
        self.finished=False;self.cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);self.cpu.reg_write(UC_ARM64_REG_LR,0x50fff000)
        for i,value in enumerate(args):self.cpu.reg_write(UC_ARM64_REG_X0+i,value)
        self.cpu.emu_start(self.image.address(name),0,count=100000000);assert self.finished,(name,hex(self.cpu.reg_read(UC_ARM64_REG_PC)))
        value=self.cpu.reg_read(UC_ARM64_REG_X0)&0xffffffff;return value if value<1<<31 else value-(1<<32)
    def upload(self,packet):
        for offset in range(0,len(packet),17):
            self.incoming.extend(packet[offset:offset+17])
            while self.incoming:self.call('board_usb0_task',0)
        assert self.call('jpeg_input_ready',self.supplied)==1
    def copy_backing(self,pixels):
        # Guard page, page-rounded mapped bytes and guard page per buffer, as
        # the native allocator sizes them from the committed geometry.
        pixels=bytes(pixels)
        w=self.read(self.supplied+self.il['width']);h=self.read(self.supplied+self.il['height']);stride=self.read(self.supplied+self.il['stride']);rows=self.read(self.supplied+self.il['rows']);cw=(w+1)//2;ch=(h+1)//2
        used=(self.read(self.supplied+self.il['input_bytes'],8),stride*rows,stride*rows//2);address=0x50000000;self.extents=[]
        for name,n in zip(('input','luma','chroma'),used):
            size=((n+4095)&~4095)+8192;self.extents.append((address,size));self.write(self.base+self.layout[name],address+4096,8)
            self.cpu.mem_write(address,bytes([0xa5])*size);address+=size
        self.luma=self.extents[1][0]+4096;chroma=self.extents[2][0]+4096
        self.cpu.mem_write(self.luma,bytes([0xcc])*(stride*rows));self.cpu.mem_write(chroma,bytes([0xdd])*(stride*rows//2))
        for row in range(h):self.cpu.mem_write(self.luma+row*stride,pixels[row*w:(row+1)*w])
        for plane in range(2):
            for row in range(ch):
                offset=w*h+plane*cw*ch+row*cw;self.cpu.mem_write(chroma+plane*(stride*rows//4)+row*(stride//2),pixels[offset:offset+cw])
        fault=self.image.layouts['a733_iommu_fault'];iommu=self.image.layouts['a733_iommu'];self.write(self.base+self.layout['iommu']+iommu['fault']+fault['scanned_masters'],0xc0,2)
    def frame(self):
        self.incoming.extend(b'\0');self.call('board_usb0_task',0)
        length=self.read(self.image.address('jpeg_response_bytes'));assert 144<=length<=144+460800
        return bytes(self.cpu.mem_read(self.image.address('jpeg_response'),length))
def run(path,inputs,out,resume=None):
    out.mkdir(parents=True,exist_ok=False);image=Image(path);decoder=module('jpeg_runtime_native_decode',ROOT/'apps/jpeg/observe.py');results={}
    cases=json.loads((inputs/'cases.json').read_text())['cases']
    for case in cases:
        source=Path(case['path']);session=(source/'session.bin').read_bytes();packet=(source/'upload.bin').read_bytes();reference=(source/'reference.yu12').read_bytes() or None
        if resume is not None:
            old=resume/(case['name']+'-frame.bin');frame=old.read_bytes();decoded=decoder.decode(frame,session,reference)
            assert decoded['width']==case['width'] and decoded['height']==case['height']
            (out/old.name).write_bytes(frame)
            results[case['name']]=dict(passed=True,reused_completed_frame=str(old),sha256=hashlib.sha256(frame).hexdigest(),oracle_verified=decoded['oracle_verified'])
            continue
        m=Machine(image,session,reference)
        shared=image.address('providers')+image.layouts['board_provider']['shared'];m.write(shared+image.layouts['a733_native_usb']['pmic_ready'],1)
        m.call('jpeg_task',0);assert m.rails==0
        m.upload(packet);m.call('jpeg_task',0);assert m.rails==1
        count=decoder.geometry(case['width'],case['height'],(case['width']+31)&~31,(case['height']+31)&~31)
        pixels=reference or bytes([128])*count;m.copy_backing(pixels)
        # One call per 64 KiB copy budget, then the post-copy fault scan.
        calls=[]
        while not calls or (calls[-1]==0 and len(calls)<=-(-count//65536)+1):calls.append(m.call('jpeg_copied',m.base,13,990000))
        assert calls[-1]==1 and set(calls[:-1])=={0} and (count>65536 or len(calls)==2),calls
        assert m.read(m.base+m.layout['copy_row'])==case['height']+2*((case['height']+1)//2)
        assert bytes(m.cpu.mem_read(m.base+m.layout['payload'],count))==pixels
        m.write(m.base+m.layout['result'],1);frame=m.frame();decoded=decoder.decode(frame,session,reference)
        assert decoded['width']==case['width'] and decoded['height']==case['height'] and frame[144:]==pixels
        (out/(case['name']+'-frame.bin')).write_bytes(frame)
        results[case['name']]=dict(passed=True,bytes=count,width=decoded['width'],height=decoded['height'],oracle_verified=decoded['oracle_verified'],calls=len(m.calls),copy_calls=len(calls),provider_requests_before_input=0)
    # Oracle mode rejects changed samples; ordinary mode preserves copied data
    # without treating an unavailable numerical oracle as a decode failure.
    for case in (cases[0],cases[1]):
        source=Path(case['path']);session=(source/'session.bin').read_bytes()
        m=Machine(image,session,None);m.upload((source/'upload.bin').read_bytes())
        count=m.read(m.supplied+m.il['output_bytes']);pixels=bytearray([128])*count;pixels[0]^=1;m.copy_backing(pixels)
        result=m.call('jpeg_copied',m.base,13,990000)
        assert result==(-1 if case['oracle'] else 0)
        results['sample-change-'+case['name']]=dict(passed=True,oracle=case['oracle'])
    # Changed post-copy faults/errors/guard tails must still suppress publication.
    source=Path(cases[0]['path']);session=(source/'session.bin').read_bytes();packet=(source/'upload.bin').read_bytes()
    for variant in ('iommu-fault','codec-error','guard-tail'):
        m=Machine(image,session,None);m.upload(packet);m.copy_backing(bytes([128])*867)
        if variant=='iommu-fault':m.fault=True
        if variant=='codec-error':m.error=True
        if variant=='guard-tail':m.cpu.mem_write(m.luma+2048,b'\0')
        assert m.call('jpeg_copied',m.base,13,990000)==-1;results[variant]=dict(passed=True)
    (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,image_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),cases=results,scope='Actual linked runtime upload/parser/publication, task resource gating, dynamic copy and USB result serializer. Shared provider observations, scheduling/time, IOMMU/codec observations and device-written pixels are modeled. No physical decoding or numerical qualification.'),indent=2)+'\n')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','inputs','output'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--resume-completed-frames',type=Path)
    a=p.parse_args();run(a.image,a.inputs,a.output,a.resume_completed_frames)
