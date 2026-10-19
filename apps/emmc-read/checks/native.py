#!/usr/bin/env python3
"""Exercise the actual eMMC image and copied clients with modeled device effects."""
import argparse,hashlib,importlib.util,json,struct,time
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn.arm64_const import UC_ARM64_REG_X0
ROOT=Path(__file__).resolve().parents[3]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
service=module('emmc_read_service_model',ROOT/'checks/storage-read-service.py')
prep=module('emmc_native_prepare',ROOT/'apps/emmc-read/prepare.py')
decoder=module('emmc_native_decoder',ROOT/'apps/emmc-read/observe.py')
class Machine(service.Machine):
    def __init__(self,image,session):
        self.host=struct.unpack_from('<I',session,892)[0];self.controller=0x04022000+self.host*0x1000
        super().__init__(image,session)
        self.regs={self.controller+(a-0x04020000) if 0x04020000<=a<0x04021000 else a:v for a,v in self.regs.items()}
        self.regs[0x02000040]=3<<4
        self.regs[0x02000044]=(3+self.host)<<8
        self.regs[0x02000048]=0;self.regs[0x02000070]=0
        self.regs[0x07090160]=(1<<28)|2
        csd=(1<<126)|(4<<122)|(9<<80)|(5<<84)|(0x7fff<<48)
        self.csd=[(csd>>(96-32*i))&0xffffffff for i in range(4)]
        self.ext=bytearray(512);self.ext[192]=8;self.ext[194]=2;struct.pack_into('<I',self.ext,212,33554432)
    def code(self,cpu,a,size,unused):
        name=self.hooks.get(a);args=[cpu.reg_read(UC_ARM64_REG_X0+i) for i in range(8)]
        if name=='a733_pins_read_inputs_banked':
            assert args[2]==2 and args[3]==2;self.write(args[4],2 if self.present else 0);self.ret(0)
        elif name=='a733_pins_acquire_banked':
            assert args[3]==12
            # This provider is modeled, but inspect the real request producer.
            requests=bytes(cpu.mem_read(args[2],12*self.image.sizes['a733_pin_request_banked']))
            self.pin_requests=requests.hex();super().code(cpu,a,size,unused)
        else:super().code(cpu,a,size,unused)
    def read_mmio(self,cpu,access,a,size,v,unused):
        assert self.held and size==4
        if 0x04020000<=a<0x04024000:assert self.controller<=a<self.controller+0x1000
        value=self.regs.get(a,0)
        if a==self.controller+0x38 and self.data_command and self.fifo==self.total:value|=8|(1<<14 if self.data_command==18 else 0)
        elif a==self.controller+0x3c:value=4 if self.fifo==self.total else 0
        elif a==self.controller+0x200:
            assert self.fifo<self.total
            data=self.ext if self.data_command==8 else service.payload(self.first_sector,self.total//512)
            value=int.from_bytes(data[self.fifo:self.fifo+4],'little');self.fifo+=4
        self.trace.append(['read',a,value]);self.write(a,value)
    def write_mmio(self,cpu,access,a,size,v,unused):
        assert self.held and size==4 and a!=self.controller+0x200
        if 0x04020000<=a<0x04024000:assert self.controller<=a<self.controller+0x1000
        self.trace.append(['write',a,v]);self.regs[a]=v
        if a==self.controller+0x38:self.regs[a]=self.read(a)&~v
        if a==self.controller:self.regs[a]=v&~7
        if a==0x03400198:self.regs[0x03400118]=self.regs.get(0x03400118,0)&~v
        if a!=self.controller+0x18:return
        assert not v&(1<<10),'Media-write direction reached native MMIO'
        self.regs[a]=v&~(1<<31)
        if v&(1<<21):self.regs[a]=0;return
        n=v&63;arg=self.regs.get(self.controller+0x1c,0);self.commands.append([n,arg])
        assert n in (0,1,2,3,9,7,13,8,17,18)
        self.regs[self.controller+0x38]=4;self.regs[self.controller+0x20]=(self.state<<9)|0x100
        self.data_command=self.total=self.fifo=0
        if n==0:self.state=0
        elif n==1:
            self.ocr+=1;self.regs[self.controller+0x20]=0x40300000|(0x80000000 if self.ocr>=3 else 0)
            if self.ocr>=3:self.state=1
        elif n in (2,9):
            for i,word in enumerate(self.cid if n==2 else self.csd):self.regs[self.controller+0x2c-4*i]=word
            if n==2:self.state=2
        elif n==3:assert arg==1<<16;self.state=3;self.regs[self.controller+0x20]=0x700
        elif n==7:self.state=4
        elif n==13:self.regs[self.controller+0x20]=0x900
        elif n in (8,17,18):
            assert (arg==0 and self.regs[self.controller+0x14]==512) if n==8 else 0<self.regs[self.controller+0x14]<=4096
            self.first_sector=arg;self.total=self.regs[self.controller+0x14];self.data_command=n
            self.regs[self.controller+0x20]=0x900;self.regs[self.controller+0x24]=0x900
    def establish(self):
        raw=self.session;self.cpu.mem_write(self.board+self.image.layouts['board_application']['admitted_session'],raw[:592])
        self.write(self.board+self.image.layouts['board_application']['handles']+8,2,8)
        values=[0x4153373345524331,2,224,*struct.unpack_from('<2Q',raw,24),struct.unpack_from('<Q',raw,32)[0]^((1<<64)-1)]
        armed=23999900;end=armed+1440000000
        contract=struct.pack('<6Q',*values)+raw[48:80]+struct.pack('<14Q',armed,armed+24000000,end,end,struct.unpack_from('<Q',raw,40)[0],0,0,0,0,0,0,0,0x41f00000,912)+hashlib.sha256(raw).digest()
        self.cpu.mem_write(0x50020000,contract)
        assert self.call('a733_smp_contract_valid',0x50020000,24000000)==1
        assert self.call('storage_read_session_prepare',0x50020000)==1
        self.cpu.mem_write(0x50030000,bytes(512))
        assert self.call('board_application_pins',0x50030000)==1
        handoff=self.image.layouts['a733_pin_handoff']
        assert self.read(0x50030000+handoff['authorized']+2*4)==0x1ef63
        assert self.read(0x50030000+handoff['authorized']+5*4)==0
        assert self.call('board_application_start')==1 and self.created==[3,1]
        for _ in range(1200):
            self.owner()
            if self.field('card_valid'):break
            assert self.field('result')!=2,(self.field('stage'),self.field('reason'),self.commands,self.trace[-20:])
        assert self.field('card_valid')==1
        assert [n for n,_ in self.commands]==[0,1,1,1,2,3,9,7,13,8,13],self.commands

def run(image_path,session_path,out):
    out.mkdir(parents=True,exist_ok=False);raw=session_path.read_bytes();prep.embedded_reference(image_path.read_bytes(),raw)
    global image
    image=service.base.Image(image_path);image.sizes={}
    with image_path.open('rb') as f:
        elf=ELFFile(f)
        for cu in elf.get_dwarf_info().iter_CUs():
            for die in cu.iter_DIEs():
                if die.tag=='DW_TAG_structure_type' and 'DW_AT_name' in die.attributes and 'DW_AT_byte_size' in die.attributes:image.sizes[die.attributes['DW_AT_name'].value.decode()]=int(die.attributes['DW_AT_byte_size'].value)
    m=Machine(image,raw);assert m.call('storage_read_session_valid',m.base)==1
    # Native admission rejects controller mismatch, NAND mux and unknown host.
    for offset,value in ((592,0x4153373352444131),(892,2),(828+2*3,2)):
        original=bytes(m.cpu.mem_read(m.base+offset,8 if offset==592 else 4 if offset==892 else 1))
        m.write(m.base+offset,value,len(original));assert m.call('storage_read_session_valid',m.base)==0;m.cpu.mem_write(m.base+offset,original)
    m.establish();initial=m.frame();(out/'idle-frame.bin').write_bytes(initial)
    assert decoder.decode(initial,raw)['sectors']==33554432
    for first,blocks in ((33554432,1),(33554431,2),(0,9)):assert m.submit(2,first,blocks,1)[0]==-1
    assert m.submit(2,19,2,1)[0]==1
    pending=m.frame();(out/'pending-frame.bin').write_bytes(pending)
    m.native_client();m.complete(0,True)
    for _ in range(500):
        m.native_client();m.owner()
        if m.read(image.address('storage_read_consumer')+image.layouts['storage_read_consumer']['result'])==1:break
    assert m.read(image.address('storage_read_consumer')+image.layouts['storage_read_consumer']['completed'])==2
    completed=m.frame();value=decoder.decode(completed,raw)
    assert completed[288:]==service.payload(19,2) and value['ext_revision']==8 and value['rca']==1
    (out/'native-frame.bin').write_bytes(completed)
    assert m.invoke(3,'storage_read_ack',value['ticket'])==0
    assert m.invoke(2,'storage_read_ack',value['ticket'])==1
    assert m.submit(2,33554424,8,2)[0]==1
    (out/'pending-last-frame.bin').write_bytes(m.frame());m.complete(0);last=m.frame()
    assert last[288:]==service.payload(33554424,8);(out/'last-frame.bin').write_bytes(last)
    # Actual cancellation revokes already copied output and retains the provider.
    m.invoke(2,'storage_read_cancel',decoder.decode(last,raw)['ticket']);m.owner()
    assert decoder.decode(m.frame(),raw)['status']==2 and m.field('cancel')==1
    assert m.read(m.platform+m.p['fault'])==1
    (out/'trace.json').write_text(json.dumps(m.trace)+'\n')
    (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,image_sha256=hashlib.sha256(image_path.read_bytes()).hexdigest(),session_sha256=hashlib.sha256(raw).hexdigest(),host=m.host,commands=m.commands,native_client_reads=2,mmio_accesses=len(m.trace),scope='Actual session/contract snapshot, provider construction, selected native aperture, MMC identification including EXT_CSD, copied two-client reads and cancellation. Providers, translations, scheduling, clock/time and MMIO/card effects modeled.'),indent=2)+'\n')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','session','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();run(a.image,a.session,a.output)
