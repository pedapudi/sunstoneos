#!/usr/bin/env python3
"""Execute the linked maintenance DMA adapter and USB framing; no hardware access."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_LR, UC_ARM64_REG_PC

ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
base=module('maintenance_base',ROOT/'checks/a733-storage-maintenance.py')

class Native(base.Native):
    def __init__(self,image):
        super().__init__(image)
        self.locals={};self.layouts={};self.types={}
        with image.open('rb') as f:
            elf=ELFFile(f);unit=''
            for s in elf.get_section_by_name('.symtab').iter_symbols():
                if s['st_info']['type']=='STT_FILE':unit=s.name
                elif s['st_info']['bind']=='STB_LOCAL':self.locals.setdefault((unit,s.name),[]).append(s['st_value'])
            for cu in elf.get_dwarf_info().iter_CUs():
                for d in cu.iter_DIEs():
                    a=d.attributes
                    if d.tag=='DW_TAG_structure_type' and 'DW_AT_name' in a and 'DW_AT_byte_size' in a:
                        name=a['DW_AT_name'].value.decode()
                        self.types[name]=a['DW_AT_byte_size'].value
                        self.layouts[name]={x.attributes['DW_AT_name'].value.decode():x.attributes['DW_AT_data_member_location'].value
                            for x in d.iter_children() if x.tag=='DW_TAG_member' and 'DW_AT_name' in x.attributes}
        for name in ('dma_sync','dma_span','mmc_admit'):
            self.symbols[name]=self.local('bootstrap.c',name)
        self.n=self.local('bootstrap.c','native');self.nl=self.layouts['a733_storage_native']
        self.service=self.n+self.nl['service'];self.readback=self.service+self.layouts['a733_storage_update']['readback']
        self.mmc=self.n+self.nl['mmc'];self.mc=self.mmc+self.layouts['a733_mmc_platform']['config']
        self.bc=self.n+self.nl['boot']+self.layouts['a733_storage_native_boot']['mmc']
        self.dmaoff=self.layouts['a733_mmc_platform_config']['dma']
        self.put(self.n+self.nl['active'],1,4)
        self.put(self.n+self.nl['boot']+self.layouts['a733_storage_native_boot']['grant']+self.layouts['a733_storage_grant']['deadline_us'],1000000000)
        self.live=1;self.scheduler=2;self.current=3;self.cache=[];self.pages=[];self.bad_page=None;self.bad_attr=False
        self.cache_ok=1;self.failed=False;self.construct=False;self.bound_dma=None;self.sent_bytes=bytearray();self.incoming=bytearray();self.copies=[];self.tasks=0
        self.fakes={self.symbols[n]:n for n in ('a733_storage_native_live','a733_maintenance_alive','a733_counter_clock_now','a733_clock_token_status',
            'aarch64_device_cache','aarch64_device_translate','xTaskGetSchedulerState','xTaskGetCurrentTaskHandle',
            'a733_storage_native_usb_step','a733_storage_native_usb_health','tusb_rhport_init','tud_task_ext','tud_mounted',
            'tud_vendor_n_available','tud_vendor_n_read','tud_vendor_n_write','tud_vendor_n_write_flush',
            'a733_maintenance_poll','vTaskDelay','a733_storage_native_fail')}
        self.constructors={self.symbols[n]:n for n in ('a733_native_usb_init','a733_native_usb_bind','xTaskCreateStatic','a733_mmc_platform_init','a733_maintenance_bind')}
    def local(self,unit,name):
        values=self.locals.get((unit,name),[]);assert len(values)==1,(unit,name,values);return values[0]
    def put(self,address,value,size=8):self.u.mem_write(address,int(value).to_bytes(size,'little'))
    def get(self,address,size=8):return int.from_bytes(self.u.mem_read(address,size),'little')
    def ret(self,value=0):self.setreg('x0',value);self.setreg('pc',self.reg('lr'))
    def step(self,u,pc,size,unused):
        name=getattr(self,'fakes',{}).get(pc)
        if getattr(self,'construct',False):name=self.constructors.get(pc,name)
        if not name:return super().step(u,pc,size,unused)
        a=[u.reg_read(UC_ARM64_REG_X0+i) for i in range(8)]
        if name=='a733_native_usb_init':
            self.put(a[0]+self.layouts['a733_native_usb']['locks']+self.layouts['a733_platform_locks']['initialized'],1,4);self.ret(1)
        elif name in ('a733_native_usb_bind','a733_maintenance_bind'):self.ret(1)
        elif name=='xTaskCreateStatic':self.tasks+=1;self.ret(self.tasks)
        elif name=='a733_mmc_platform_init':
            self.bound_dma=self.get(a[1]+self.dmaoff)
            sync=self.get(a[2]+self.layouts['a733_mmc_platform_ops']['dma_sync'])
            assert sync==(self.symbols['dma_sync'] if self.bound_dma else 0)
            self.u.mem_write(a[0]+self.layouts['a733_mmc_platform']['config'],bytes(self.u.mem_read(a[1],self.types['a733_mmc_platform_config'])))
            self.ret(1)
        elif name in ('a733_storage_native_live','a733_maintenance_alive'):self.ret(self.live)
        elif name=='a733_counter_clock_now':self.put(a[1],1000000);self.ret(1)
        elif name=='a733_clock_token_status':self.ret(0)
        elif name=='xTaskGetSchedulerState':self.ret(self.scheduler)
        elif name=='xTaskGetCurrentTaskHandle':self.ret(self.current)
        elif name=='aarch64_device_translate':
            self.pages.append(a[0]);assert a[1]==1
            attr=0x44 if 0x41c00000<=a[0]<0x41e00000 else 0xff
            if a[0]==self.bad_page:attr^=0xff
            self.u.mem_write(a[2],struct.pack('<QII',a[0],attr,3));self.ret(1)
        elif name=='aarch64_device_cache':self.cache.append(tuple(a[:3]));self.ret(self.cache_ok)
        elif name in ('a733_storage_native_usb_step','a733_storage_native_usb_health','tusb_rhport_init','tud_mounted','a733_maintenance_poll'):self.ret(1)
        elif name in ('tud_task_ext','tud_vendor_n_write_flush'):self.ret()
        elif name=='tud_vendor_n_available':self.ret(len(self.incoming))
        elif name=='tud_vendor_n_read':
            count=min(a[2],len(self.incoming));self.u.mem_write(a[1],bytes(self.incoming[:count]));del self.incoming[:count]
            self.copies.append(count);self.ret(count)
        elif name=='tud_vendor_n_write':
            count=min(2048,a[2]);self.sent_bytes.extend(self.u.mem_read(a[1],count));self.ret(count)
        elif name=='a733_storage_native_fail':self.failed=True;self.ret()
        elif name=='vTaskDelay':self.ret();self.stop='failed' if self.failed else 'tick';u.emu_stop()
        else:raise AssertionError(name)
    def resume(self):
        self.stop=None;self.u.emu_start(self.reg('pc'),0x43000004,count=200000);assert self.stop
    def adapter(self):
        self.put(self.mc+self.dmaoff,0x41c00000);self.put(self.bc+self.dmaoff,0x41c00000)
        owner=self.layouts['a733_mmc_platform_config']['owner'];self.put(self.mc+owner,17);self.put(self.bc+owner,17)
        self.put(0x41e80000+8,17) # a733_clock_token.owner
    def usb(self):
        self.put(self.n+self.nl['active'],1,4);self.put(self.n+self.nl['service_ready'],1,4)
        self.put(self.n+self.nl['task_handle']+16,self.current)
    def usb_address(self,name):return self.local('usb.c',name)

def run(image,observed,out):
    out.mkdir(parents=True);cases=[]
    m=Native(image);m.adapter();assert not m.readback%64
    assert m.types['a733_storage_update']%64==0
    before=m.layouts['a733_storage_update']['input']+512;after=m.layouts['a733_storage_update']['response']
    assert before<=m.layouts['a733_storage_update']['readback'] and after>=m.layouts['a733_storage_update']['readback']+32768
    assert m.call('mmc_admit',(m.n,m.mc,0x41e80000))==1
    pages=list(m.pages);expected=[0x41c00000]+list(range(m.readback&~4095,(m.readback+32767&~4095)+4096,4096))
    assert pages==expected,(pages,expected)
    for page in pages:
        m.bad_page=page;assert m.call('mmc_admit',(m.n,m.mc,0x41e80000))&0xffffffff==0xffffffff
    m.bad_page=None;m.put(m.mc+m.dmaoff,0x41c00040)
    assert m.call('mmc_admit',(m.n,m.mc,0x41e80000))&0xffffffff==0xffffffff
    m.put(m.bc+m.dmaoff,0x41c00040)
    assert m.call('mmc_admit',(m.n,m.mc,0x41e80000))&0xffffffff==0xffffffff
    cases.append('actual admission checks every descriptor/readback page and rejects config or private-pointer substitution')
    m=Native(image);m.adapter();cookie=m.n+m.nl['mmc_native']
    for phase,operation in ((0,0),(1,2)):
        assert m.call('dma_sync',(cookie,m.readback,32768,phase))==1
        assert m.cache[-1]==(operation,m.readback,32768)
    for args in ((cookie+8,m.readback,512,0),(cookie,m.readback+64,512,0),(cookie,m.readback,0,0),
                 (cookie,m.readback,32768+512,0),(cookie,m.readback,513,0),(cookie,m.readback,512,2)):
        n=len(m.cache);assert m.call('dma_sync',args)==0 and len(m.cache)==n
    m.live=0;assert m.call('dma_sync',(cookie,m.readback,512,0))==0
    m.live=1;m.cache_ok=0;assert m.call('dma_sync',(cookie,m.readback,512,0))==0
    cases.append('actual cache callback admits only its private aligned range, supported phases and live lifetime; cache failure propagates')
    # Reuse the accepted modeled launcher's real session publication. Only the
    # selected profile changes; no physical qualification is manufactured.
    raw=bytearray(observed.read_bytes());struct.pack_into('<I',raw,456,2)
    m=Native(image);m.scheduler=1;m.put(m.n+m.nl['active'],0,4);m.u.mem_write(0x41f00000,bytes(raw))
    ticks=struct.unpack_from('<Q',raw,648)[0]+100
    assert m.call('a733_storage_session_decode',(0x41e00000,0x41e04000,0x41f00000,ticks))==1
    bmmc=0x41e00000+m.layouts['a733_storage_native_boot']['mmc']
    m.put(bmmc+m.dmaoff,0x41c00000)
    assert m.call('a733_storage_native_start',(0x41e00000,0x41e08000,0x41e04000))==0
    assert m.get(m.n+m.nl['used'],4)==1 and not m.get(m.n+m.nl['active'],4) and not m.get(m.n+m.nl['task_handle'])
    cases.append('actual bootstrap rejects even the private descriptor address when supplied by its caller before task creation')
    for profile in (0,2):
        m=Native(image);m.construct=True;m.scheduler=1;m.put(m.n+m.nl['active'],0,4)
        struct.pack_into('<I',raw,456,profile);m.u.mem_write(0x41f00000,bytes(raw))
        assert m.call('a733_storage_session_decode',(0x41e00000,0x41e04000,0x41f00000,ticks))==1
        started=m.call('a733_storage_native_start',(0x41e00000,0x41e08000,0x41e04000))
        assert started==1,(profile,started,m.tasks,m.bound_dma,m.get(m.n+m.nl['failed'],4))
        assert m.bound_dma==(0x41c00000 if profile==2 else 0) and m.tasks==3
    cases.append('actual bootstrap selects private DMA only for profile 2 and binds its callback; unchanged provider constructors are modeled separately')
    # Native mailbox bound rejects before copying into the now smaller request.
    m=Native(image);channel=m.n+m.nl['channel'];size=m.types['a733_storage_channel']
    m.u.mem_write(channel,b'\xa5'*size)
    cl=m.layouts['a733_storage_channel'];m.put(channel+cl['state'],0,4);m.put(channel+cl['lost'],0,4)
    before=bytes(m.u.mem_read(channel,size))
    assert m.call('a733_storage_channel_submit',(channel,0x41e80000,633))==0
    assert bytes(m.u.mem_read(channel,size))==before
    cases.append('633-byte mailbox request refuses before copy or publication')
    # Execute the actual USB task. Only its transport/provider scheduling edges
    # are modeled; mailbox extraction, frame copy and completion are native.
    m=Native(image);m.usb();cl=m.layouts['a733_storage_channel'];channel=m.n+m.nl['channel']
    data=bytes((i*19+7)&255 for i in range(32768))
    body=struct.pack('<IIQII',1,0,0,len(data),0)+hashlib.sha256(data).digest()+data
    m.u.mem_write(channel+cl['response'],body);m.put(channel+cl['response_size'],len(body),4);m.put(channel+cl['state'],3,4)
    m.put(m.usb_address('sequence'),1,4)
    transport=m.n+m.nl['transport'];tl=m.layouts['a733_storage_native_transport']
    m.put(transport+tl['accepted_command'],2)
    m.call('a733_storage_native_usb_task',(m.n,));iterations=0;delivered=0
    while delivered<16+len(body):
        assert m.stop=='tick' and not m.failed;iterations+=1
        count=len(m.sent_bytes)-delivered;assert 0<count<=2048
        state=m.u.context_save();m.call('tud_vendor_tx_cb',(0,count));m.u.context_restore(state);delivered+=count
        assert m.get(transport+tl['completed_sequence'])==(1 if delivered==16+len(body) else 0)
        if delivered<16+len(body):m.resume()
    assert iterations==17 and bytes(m.sent_bytes)==struct.pack('<4I',0x53534f53,1,0x53555044|0x80000000,len(body))+body
    (out/'response.bin').write_bytes(m.sent_bytes)
    host=module('storage_dma_host',ROOT/'tools/storage-update.py')
    frame=bytes(m.sent_bytes)
    class Access:
        def identity(self,node):return 'modeled-instance'
        def quarantine_instance(self,*args,**kwargs):self.quarantined=True
    class Device:
        def write(self,endpoint,packet,timeout):assert endpoint==2 and timeout==1500;return len(packet)
        def read(self,endpoint,size,timeout):assert endpoint==0x81 and size==32840 and timeout==1500;return self.frame
    def exchange(raw):
        transport=host.USBExchange.__new__(host.USBExchange);transport.failed=False
        transport.access=Access();transport.access.quarantined=False;transport.node='model';transport.identity='modeled-instance'
        transport.device=Device();transport.device.frame=raw;return transport
    request=bytearray(88);struct.pack_into('<I',request,32,1)
    assert exchange(frame)(request)==body
    for bad in (frame[:-1],frame+bytes(512),bytes(16)+frame[16:]):
        transport=exchange(bad)
        try:transport(request);raise AssertionError('malformed native capture accepted')
        except ValueError:pass
        assert transport.failed and transport.access.quarantined
    cases.append('host accepts the native 32 KiB frame and rejects truncated, oversized and mismatched captures with quarantine')
    cases.append('32 KiB native response streams in 17 existing FIFO pieces; only the final completed IN publishes progress')
    m=Native(image);m.usb();m.incoming=bytearray(struct.pack('<4I',0x53534f53,1,0x53555044,633))
    request=m.usb_address('request');m.u.mem_write(request+16,b'\xa5'*632)
    m.call('a733_storage_native_usb_task',(m.n,))
    assert m.stop=='failed' and m.copies==[16] and bytes(m.u.mem_read(request+16,632))==b'\xa5'*632
    cases.append('oversized USB body refuses immediately after its header without copying payload')
    result=dict(result='pass',physical=False,image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),
                observed_session_sha256=hashlib.sha256(observed.read_bytes()).hexdigest(),checks=cases,
                memory=dict(service=Native(image).types['a733_storage_update'],channel=Native(image).types['a733_storage_channel']))
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('\n'.join('PASS: '+x for x in cases))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True)
    p.add_argument('--observed-session',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();run(a.image,a.observed_session,a.output)
