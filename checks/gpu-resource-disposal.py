#!/usr/bin/env python3
"""Execute native GPU child disposal/reconstruction; inherited stop proof is modeled.

The exact linked reserved owners, pools, translation removal, release accounting,
backend binding/proof and reconstruction execute. CPU admission/cache/translation,
FreeRTOS scheduling and the already accepted healthy shutdown are model inputs.
No GPU work, firmware execution, interconnect behavior or physical reuse is proved.
"""
import argparse, hashlib, importlib.util, json, struct, gzip, pickle
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import *
from elftools.elf.elffile import ELFFile
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('gpu_disposal_image',ROOT/'checks/npu-large-native.py')
base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)

class Machine:
    def __init__(self,path):
        self.path=Path(path);self.image=base.Image(path);self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
        self.u.mem_map(0x40000000,0x04000000);self.u.mem_map(0x48000000,0x31000000)
        self.u.mem_map(0x90000000,0x100000)
        self.u.reg_write(UC_ARM64_REG_PSTATE,0x3c5)
        for at,data in self.image.parts:
            if data:self.u.mem_write(at,data)
        self.sizes={};self.enums={}
        with path.open('rb') as f:
            for cu in ELFFile(f).get_dwarf_info().iter_CUs():
                for d in cu.iter_DIEs():
                    if d.tag=='DW_TAG_structure_type' and 'DW_AT_name' in d.attributes and 'DW_AT_byte_size' in d.attributes:
                        self.sizes[d.attributes['DW_AT_name'].value.decode()]=int(d.attributes['DW_AT_byte_size'].value)
                    if d.tag=='DW_TAG_enumeration_type':
                        for c in d.iter_children():
                            if c.tag=='DW_TAG_enumerator':self.enums[c.attributes['DW_AT_name'].value.decode()]=int(c.attributes['DW_AT_const_value'].value)
        self.app=self.image.address('app');self.tick=24000000;self.scheduler=1;self.task=0
        self.cache=[];self.fail_cache=None;self.traces=[];self.publishing=False;self.incoming=None;self.worker_stop=None;self.reply=None
        names=['a733_smp_counter','aarch64_device_translate','aarch64_device_cache_stride','aarch64_device_cache',
            'a733_device_memory_ready','a733_device_memory_commit_allowed','a733_cpu_coherence_publish',
            'xTaskGetSchedulerState','xTaskGetCurrentTaskHandle','xTaskCreateStatic','xQueueGenericCreateStatic',
            'vPortEnterCritical','vPortExitCritical','platform_panic','xQueueReceive','xQueueGenericSend',
            'uxQueueMessagesWaiting']
        self.hooks={self.image.address(n):n for n in names}
        self.hooks[self.image.address('power_begin','/platform/a733/gpu_backend.c')]='modeled_power_boundary'
        sites=set(self.hooks)|{0x900ff000}
        for at,data in self.image.parts:
            for offset in range(0,len(data)-3,4):
                w=int.from_bytes(data[offset:offset+4],'little')
                if w&0xffffffe0 in (0xd5384240,0xd5384200,0xd53be000,0xd53be020) or w==0xd5034fdf:sites.add(at+offset)
        for at in sites:self.u.hook_add(UC_HOOK_CODE,self.hook,begin=at,end=at)
    def address(self,path):
        at=self.app;typ='gpu_application'
        for name,next_type in path:
            at+=self.image.layouts[typ][name];typ=next_type
        return at
    def at(self,name):return self.app+self.image.layouts['gpu_application'][name]
    def field(self,at,typ,name):return at+self.image.layouts[typ][name]
    def put(self,at,value,n=8):self.u.mem_write(at,int(value).to_bytes(n,'little'))
    def get(self,at,n=8):return int.from_bytes(self.u.mem_read(at,n),'little')
    def set(self,at,typ,name,value,n=4):self.put(self.field(at,typ,name),value,n)
    def enum(self,name):return self.enums[name]
    def ret(self,value=0):self.u.reg_write(UC_ARM64_REG_X0,value);self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_LR))
    def hook(self,cpu,at,size,unused):
        if at==0x900ff000:self.u.emu_stop();return
        w=self.get(at,4)
        if at not in self.hooks:
            if w==0xd5034fdf:self.u.reg_write(UC_ARM64_REG_PC,at+4);return
            value=4 if w&0xffffffe0==0xd5384240 else 1 if w&0xffffffe0==0xd5384200 else 24000000 if w&0xffffffe0==0xd53be000 else self.tick
            self.u.reg_write(UC_ARM64_REG_X0+(w&31),value);self.u.reg_write(UC_ARM64_REG_PC,at+4);return
        name=self.hooks[at];a=[self.u.reg_read(UC_ARM64_REG_X0+i) for i in range(8)]
        if name=='a733_smp_counter':self.ret(self.tick)
        elif name=='aarch64_device_translate':
            attr=0x44 if 0x78000000<=a[0]<0x79000000 else 0xff if a[0]>=0x40000000 else 0
            self.u.mem_write(a[2],struct.pack('<QII',a[0],attr,3 if attr else 2));self.ret(1)
        elif name=='aarch64_device_cache_stride':self.ret(64)
        elif name=='aarch64_device_cache':
            self.cache.append(tuple(a[:3]));self.ret(0 if self.fail_cache and self.fail_cache(a) else 1)
        elif name in ('a733_device_memory_ready','a733_cpu_coherence_publish'):self.ret(1)
        elif name=='a733_device_memory_commit_allowed':self.ret((a[0],a[1],a[2]) in ((0,0x48000000,0x78000000),(1,0x78000000,0x79000000)))
        elif name=='xTaskGetSchedulerState':self.ret(self.scheduler)
        elif name=='xTaskGetCurrentTaskHandle':self.ret(self.task)
        elif name=='xTaskCreateStatic':self.ret(a[6])
        elif name=='xQueueGenericCreateStatic':self.ret(a[3])
        elif name in ('vPortEnterCritical','vPortExitCritical'):self.ret()
        elif name=='modeled_power_boundary':self.ret(1)
        elif name=='xQueueReceive':
            assert self.publishing or self.incoming is not None
            if self.incoming is not None:
                self.u.mem_write(a[1],self.incoming);self.incoming=None;self.ret(1);return
            self.ret(0)
            if self.get(self.s+self.image.layouts['pvr_gpu_service']['phase'],4)==self.enum('PVR_GPU_SERVICE_ENABLING'):
                self.publishing=False;self.worker_stop='published';self.u.emu_stop()
        elif name=='uxQueueMessagesWaiting':
            # Between burst steps the worker checks for queued requests; publication
            # stops at the same modeled power admission boundary as a receive.
            self.ret(0)
            if self.publishing and self.get(self.s+self.image.layouts['pvr_gpu_service']['phase'],4)==self.enum('PVR_GPU_SERVICE_ENABLING'):
                self.publishing=False;self.worker_stop='published';self.u.emu_stop()
        elif name=='xQueueGenericSend':
            self.reply=bytes(self.u.mem_read(a[1],self.sizes['pvr_gpu_service_reply']))
            self.ret(1);self.worker_stop='reply';self.u.emu_stop()
        else:raise AssertionError((name,a))
    def checkpoint(self,label):
        if not hasattr(self,'out'):return
        attrs={k:v for k,v in self.__dict__.items() if k in ('tick','scheduler','task','cache','publishing','f','c','g','s','b','p','d','cc','worker_stop','incoming','reply')}
        regs={str(r):self.u.reg_read(r) for r in [UC_ARM64_REG_X0+i for i in range(31)]+[UC_ARM64_REG_SP,UC_ARM64_REG_PC,UC_ARM64_REG_PSTATE]}
        ram=[(at,bytes(self.u.mem_read(at,n))) for at,n in ((0x41000000,0x1000000),(0x48000000,0x8000000),(0x78000000,0x1000000),(0x90000000,0x100000))]
        with gzip.open(self.out/(label+'.pkl.gz'),'wb') as f:pickle.dump(dict(image_sha256=hashlib.sha256(self.path.read_bytes()).hexdigest(),attrs=attrs,regs=regs,ram=ram),f,protocol=5)
    def call(self,name,*args,unit=None):
        self.u.reg_write(UC_ARM64_REG_SP,(self.at('service')+self.image.layouts['pvr_gpu_service']['worker_stack']+32768-32) if self.scheduler==2 else 0x900f0000);self.u.reg_write(UC_ARM64_REG_LR,0x900ff000)
        for i,arg in enumerate(args):self.u.reg_write(UC_ARM64_REG_X0+i,arg)
        try:self.u.emu_start(self.image.address(name,unit),0,count=300000000)
        except Exception:
            self.checkpoint('failure-'+name);raise
        assert self.u.reg_read(UC_ARM64_REG_PC)==0x900ff000 or name=='worker' and self.worker_stop,(name,hex(self.u.reg_read(UC_ARM64_REG_PC)))
        return self.u.reg_read(UC_ARM64_REG_X0)
    def make(self):
        f,c,g,s,b,p,d=[self.at(n) for n in ('firmware','compute','graphics','service','backend','power','disposal')]
        self.f,self.c,self.g,self.s,self.b,self.p,self.d=f,c,g,s,b,p,d
        self.set(self.at('session'),'a733_board_session','deadline_us',61000000,8)
        fw=self.image.address('gpu_firmware');end=self.image.address('gpu_firmware_end')
        assert self.call('a733_memory_reservations_commit_verified')==1
        assert self.call('a733_memory_uncached_commit_verified')==1
        assert self.call('a733_gpu_reserved_prepare',f,9,fw,end-fw,400000000)
        assert self.call('a733_gpu_compute_reserved_prepare',c,f,s,10)
        profile=d+self.image.layouts['a733_gpu_disposal']['profile']
        self.u.mem_write(profile,struct.pack('<6I Q',64,64,16384,1024,1024,25,5000000))
        assert self.call('a733_gpu_graphics_memory_prepare_depth_blend',g,f,c,s,11,profile)==1
        # Provider construction/OFF is inherited accepted evidence, not replayed.
        self.set(p,'a733_gpu_platform','initialized',1)
        self.set(p,'a733_gpu_platform','manager',self.at('shared')+self.image.layouts['a733_native_usb']['gpu'],8)
        cfg=f+self.image.layouts['a733_gpu_reserved']['config'];memory=f+self.image.layouts['a733_gpu_reserved']['memory']
        self.put(0x90001000,0,8);self.u.mem_write(0x90001000,bytes(self.sizes['a733_gpu_backend_external']))
        assert self.call('a733_gpu_backend_bind',b,s,cfg,memory,p,0x90001000)==1
        self.set(cfg,'pvr_gpu_service_config','disposal',d,8) # As the application's binding does.
        assert self.call('pvr_gpu_service_init',s,cfg,3,1)==1
        client=0x90004000;self.put(self.at('handles')+16,client)
        assert self.call('pvr_gpu_service_bind',s,0,client)==1
        for name,value in [('firmware',f),('compute',c),('graphics',g),('backend',b),('limit',61000000)]:self.set(d,'a733_gpu_disposal',name,value,8)
        self.scheduler=2;self.task=self.get(s+self.image.layouts['pvr_gpu_service']['worker'])
        self.set(s,'pvr_gpu_service','owner',0)
        self.set(s,'pvr_gpu_service','epoch',1,8)
        self.set(s,'pvr_gpu_service','deadline',2000000,8)
        # Execute the normal service publication and native free-list callbacks;
        # stop at the unchanged power-provider admission boundary.
        assert self.call('prepare',s,unit='/drivers/gpu/pvr_gpu_service.c')==1
        self.checkpoint('constructed')
        self.publishing=True;self.call('worker',s,unit='/drivers/gpu/pvr_gpu_service.c')
        self.checkpoint('published')
        cc=c+self.image.layouts['a733_gpu_compute_reserved']['compute'];self.cc=cc
        # Materialize the existing complete parent healthy-stop proof and exact
        # successful cleanup receipts. This does not execute firmware or shutdown.
        self.set(s,'pvr_gpu_service','phase',self.enum('PVR_GPU_SERVICE_STOPPED'))
        self.set(s,'pvr_gpu_service','owner',8);self.set(s,'pvr_gpu_service','closed_owner',0)
        for name in ('epoch','closed_epoch','published_epoch'):self.set(s,'pvr_gpu_service',name,1,8)
        self.set(s,'pvr_gpu_service','deadline',2000000,8)
        self.set(b,'a733_gpu_backend','phase',self.enum('A733_GPU_BACKEND_ISOLATED'))
        self.set(p,'a733_gpu_platform','phase',self.enum('A733_GPU_PLATFORM_OFF'))
        self.set(memory,'a733_gpu_memory','phase',self.enum('A733_GPU_MEMORY_OFFLINE'))
        stop=s+self.image.layouts['pvr_gpu_service']['stop']
        self.set(stop,'pvr_gpu_stop','phase',self.enum('PVR_GPU_STOP_STOPPED'))
        self.set(stop,'pvr_gpu_stop','isolation_observed',1)
        ack=stop+self.image.layouts['pvr_gpu_stop']['acknowledgement'];self.put(ack,1,4);self.put(ack+4,1,4)
        self.set(s+self.image.layouts['pvr_gpu_service']['queue'],'pvr_ccb','phase',self.enum('PVR_CCB_RETIRED'))
        self.set(s+self.image.layouts['pvr_gpu_service']['control'],'pvr_gpu_control','phase',self.enum('PVR_GPU_CONTROL_RETIRED'))
        self.set(cc,'pvr_compute_service','phase',self.enum('PVR_COMPUTE_SERVICE_CLEANED'))
        self.set(cc,'pvr_compute_service','cleanup_status',1)
        token=cc+self.image.layouts['pvr_compute_service']['cleanup_token'];self.set(token,'pvr_ccb_token','epoch',1,8);self.set(token,'pvr_ccb_token','serial',3,8)
        rs=g+self.image.layouts['a733_gpu_graphics_memory']['submission']
        self.set(rs,'pvr_render_service','phase',self.enum('PVR_RENDER_SERVICE_CLEANED'))
        self.set(s,'pvr_gpu_service','graphics_cleanup_count',6);self.set(s,'pvr_gpu_service','graphics_cleanup_index',6);self.set(s,'pvr_gpu_service','graphics_events_retired',1)
        addresses=s+self.image.layouts['pvr_gpu_service']['graphics_cleanup_address'];types=s+self.image.layouts['pvr_gpu_service']['graphics_cleanup_type']
        assert self.call('a733_gpu_graphics_memory_cleanup_records',g,addresses,types,0x90002000)==1
        assert self.get(0x90002000,4)==6
        for i in range(6):
            self.put(s+self.image.layouts['pvr_gpu_service']['graphics_cleanup_status']+4*i,1,4)
            at=s+self.image.layouts['pvr_gpu_service']['graphics_cleanup_tokens']+i*self.sizes['pvr_ccb_token']
            self.set(at,'pvr_ccb_token','epoch',1,8);self.set(at,'pvr_ccb_token','serial',i+1,8)
        assert self.call('a733_gpu_backend_stopped',b,s)==1
        self.checkpoint('stopped')
    def restore(self,path):
        with gzip.open(path,'rb') as f:state=pickle.load(f)
        bound=state.get('image_sha256')
        if bound is None:bound=json.loads((Path(path).parent/'preflight.json').read_text())['image_sha256']
        assert bound==hashlib.sha256(self.path.read_bytes()).hexdigest(),'checkpoint image mismatch'
        for at,data in state['ram']:self.u.mem_write(at,data)
        for reg,value in state['regs'].items():self.u.reg_write(int(reg),value)
        self.__dict__.update(state['attrs'])
    def control(self,operation,epoch,timeout=1000000):
        message=bytearray(self.sizes['pvr_gpu_service_message']);ml=self.image.layouts['pvr_gpu_service_message'];ql=self.image.layouts['pvr_gpu_service_request']
        struct.pack_into('<I',message,ml['request']+ql['operation'],operation)
        struct.pack_into('<Q',message,ml['request']+ql['timeout_us'],timeout)
        struct.pack_into('<Q',message,ml['ticket'],1);struct.pack_into('<Q',message,ml['epoch'],epoch)
        struct.pack_into('<I',message,ml['client'],0)
        self.incoming=bytes(message);self.worker_stop=None;self.reply=None
        self.call('worker',self.s,unit='/drivers/gpu/pvr_gpu_service.c')
        assert self.reply is not None
        rl=self.image.layouts['pvr_gpu_service_reply']
        return {key:int.from_bytes(self.reply[at:at+(8 if key in ('epoch','ticket') else 4)],'little') for key,at in rl.items() if key in ('epoch','ticket','phase','result','error')}
    def dispose(self):
        assert self.call('a733_gpu_disposal_begin',self.s,1)==1
        self.set(self.s,'pvr_gpu_service','phase',self.enum('PVR_GPU_SERVICE_DISPOSING'))
        for i in range(20000):
            r=self.call('a733_gpu_disposal_poll',self.s)
            if r:return r,i+1
        raise AssertionError('disposal poll budget')

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--resume',type=Path);a=p.parse_args()
    a.output.mkdir(parents=True,exist_ok=False)
    m=Machine(a.image);m.out=a.output
    # Resolve every called function, consumed type/field and modeled boundary
    # before constructors or worker code execute; inlined advance is not called.
    calls=['a733_memory_reservations_commit_verified','a733_memory_uncached_commit_verified',
        'a733_gpu_reserved_prepare','a733_gpu_compute_reserved_prepare','a733_gpu_graphics_memory_prepare_depth_blend',
        'a733_gpu_backend_bind','pvr_gpu_service_init','pvr_gpu_service_bind','a733_gpu_graphics_memory_cleanup_records',
        'a733_gpu_backend_stopped','a733_gpu_disposal_begin','a733_gpu_disposal_poll','a733_gpu_disposal_rebuild','device_memory_lookup']
    resolved={name:m.image.address(name) for name in calls}
    for name in ('worker','prepare'):resolved['service.'+name]=m.image.address(name,'/drivers/gpu/pvr_gpu_service.c')
    required=['gpu_application','a733_gpu_disposal','pvr_gpu_service','a733_gpu_reserved','a733_gpu_compute_reserved',
        'a733_gpu_graphics_memory','a733_gpu_platform','pvr_compute_service','pvr_render_service','pvr_gpu_stop',
        'pvr_ccb','pvr_gpu_control','pvr_ccb_token','device_memory_pool','device_memory_allocation','device_memory_lease']
    assert all(n in m.image.layouts and n in m.sizes for n in required)
    (a.output/'preflight.json').write_text(json.dumps(dict(image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),
        functions=resolved,hooks=m.hooks,types=required,stack='actual service.worker_stack; scratch outside complete pools',
        initialization='actual extended pool commit_verified under modeled admission; native child constructors, backend binding, static service/client construction; actual worker publication up to modeled power admission; existing complete healthy stop and receipts modeled'),indent=2)+'\n')
    if a.resume:m.restore(a.resume)
    else:m.make()
    # Save all exact parent leases; old generations must remain invalid forever.
    old={n:bytes(m.u.mem_read(m.g+m.image.layouts['a733_gpu_graphics_memory'][n],m.sizes['device_memory_lease'])) for n in ('backing_lease','payload_lease','shared_lease','table_lease')}
    r,steps=m.dispose();assert r==1
    assert m.get(m.d+m.image.layouts['a733_gpu_disposal']['released'],4)==127
    m.set(m.s,'pvr_gpu_service','phase',m.enum('PVR_GPU_SERVICE_DISPOSED'))
    m.checkpoint('disposed')
    for name,lease in old.items():
        m.u.mem_write(0x90003000,lease);assert m.call('device_memory_lookup',int.from_bytes(lease[:8],'little'),0x90003000)==0
    before_global=bytes(m.u.mem_read(m.f+m.image.layouts['a733_gpu_reserved']['wb_lease'],m.sizes['device_memory_lease']))
    refused=m.control(m.enum('PVR_GPU_SERVICE_OPEN'),0)
    assert refused['result']==m.enum('PVR_GPU_SERVICE_INVALID')
    opened=m.control(m.enum('PVR_GPU_SERVICE_OPEN'),1,15000000)
    assert opened['result']==m.enum('PVR_GPU_SERVICE_OK') and opened['epoch']==2
    assert m.get(m.d+m.image.layouts['a733_gpu_disposal']['phase'],4)==m.enum('A733_GPU_DISPOSAL_LIVE')
    assert bytes(m.u.mem_read(m.f+m.image.layouts['a733_gpu_reserved']['wb_lease'],len(before_global)))==before_global
    for name,lease in old.items():
        fresh=bytes(m.u.mem_read(m.g+m.image.layouts['a733_gpu_graphics_memory'][name],len(lease)))
        assert fresh!=lease and int.from_bytes(fresh[16:24],'little')>int.from_bytes(lease[16:24],'little')
    m.checkpoint('reconstructed')
    m.worker_stop=None;m.publishing=True;m.call('worker',m.s,unit='/drivers/gpu/pvr_gpu_service.c')
    m.checkpoint('reopened-publication')
    record={'passed':True,'old_epoch_open_refusal':refused,'new_open':opened,'image_sha256':hashlib.sha256(a.image.read_bytes()).hexdigest(),'steps':steps,'scope':__doc__}
    (a.output/'result.json').write_text(json.dumps(record,indent=2)+'\n')
    print(json.dumps(record))
if __name__=='__main__':main()
