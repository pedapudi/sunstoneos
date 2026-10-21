#!/usr/bin/env python3
"""Compute-only native child reclamation and application transport models.

Leaf execution uses actual reserved owners, three child parent leases, backend
binding, stopped proof checks and offline reconstruction. The accepted healthy
stop and CPU architecture effects are modeled. Application execution separately
uses actual mailbox/client/TinyUSB code with modeled GPU/service completions.
Neither model establishes physical GPU execution or memory reclamation.
"""
import argparse,gzip,hashlib,importlib.util,json,pickle,struct,types
from pathlib import Path
from unicorn.arm64_const import *
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
leaf=module('compute_lifecycle_leaf',ROOT/'checks/gpu-resource-disposal.py')
texture=module('compute_lifecycle_transport',ROOT/'checks/gpu-texture-runtime.py')
sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()

def retain_package(machine,package):
    """Model prior authenticated retention; run the real immutable accessor."""
    image=machine.image;cpu=machine.u if hasattr(machine,'u') else machine.cpu
    at=image.address('program_session');fields=image.layouts['gpu_program_session'];raw=bytearray(fields['package']+8288)
    app=image.address('app');session=app+image.layouts['gpu_application']['session']
    raw[:592]=bytes(cpu.mem_read(session,592))
    struct.pack_into('<4Q',raw,fields['magic'],0x4153373347505031,1,len(raw),8288)
    raw[fields['package_digest']:fields['package_digest']+32]=hashlib.sha256(package).digest()
    raw[fields['package']:]=package
    cpu.mem_write(at,bytes(raw));cpu.mem_write(image.address('session_sealed'),struct.pack('<I',1))
    return at+fields['package']

class Owner(leaf.Machine):
    def make_compute(self,package):
        f,c,s,b,p,d=[self.at(n) for n in ('firmware','compute','service','backend','power','disposal')]
        self.f,self.c,self.g,self.s,self.b,self.p,self.d=f,c,0,s,b,p,d
        self.set(self.at('session'),'a733_board_session','deadline_us',61000000,8)
        package_at=retain_package(self,package)
        assert self.call('gpu_application_package')==package_at
        assert self.call('pvr_compute_package_valid',package_at)==1
        assert self.call('a733_memory_reservations_commit_verified')==1
        assert self.call('a733_memory_uncached_commit_verified')==1
        fw=self.image.address('gpu_firmware');end=self.image.address('gpu_firmware_end')
        assert self.call('a733_gpu_reserved_prepare',f,9,fw,end-fw,400000000)
        assert self.call('a733_gpu_compute_reserved_prepare_package',c,f,s,10,package_at)
        # Same accepted provider-construction boundary as retained depth checks.
        self.set(p,'a733_gpu_platform','initialized',1)
        self.set(p,'a733_gpu_platform','manager',self.at('shared')+self.image.layouts['a733_native_usb']['gpu'],8)
        cfg=f+self.image.layouts['a733_gpu_reserved']['config'];memory=f+self.image.layouts['a733_gpu_reserved']['memory']
        self.u.mem_write(0x90001000,bytes(self.sizes['a733_gpu_backend_external']))
        assert self.call('a733_gpu_backend_bind',b,s,cfg,memory,p,0x90001000)==1
        self.set(cfg,'pvr_gpu_service_config','disposal',d,8) # As the application's binding does.
        assert self.call('pvr_gpu_service_init',s,cfg,3,1)==1
        self.put(self.at('handles')+16,0x90004000)
        assert self.call('pvr_gpu_service_bind',s,0,0x90004000)==1
        for name,value in [('firmware',f),('compute',c),('graphics',0),('backend',b),('limit',61000000)]:self.set(d,'a733_gpu_disposal',name,value,8)
        self.scheduler=2;self.task=self.get(s+self.image.layouts['pvr_gpu_service']['worker'])
        self.set(s,'pvr_gpu_service','owner',0);self.set(s,'pvr_gpu_service','epoch',1,8)
        self.set(s,'pvr_gpu_service','deadline',2000000,8)
        assert self.call('prepare',s,unit='/drivers/gpu/pvr_gpu_service.c')==1
        self.checkpoint('constructed')
        self.publishing=True;self.call('worker',s,unit='/drivers/gpu/pvr_gpu_service.c');self.checkpoint('published')
        self.cc=c+self.image.layouts['a733_gpu_compute_reserved']['compute']
        # Inherit the already accepted full healthy stop; no cleanup replay.
        self.set(s,'pvr_gpu_service','phase',self.enum('PVR_GPU_SERVICE_STOPPED'))
        self.set(s,'pvr_gpu_service','owner',8);self.set(s,'pvr_gpu_service','closed_owner',0)
        for name in ('epoch','closed_epoch','published_epoch'):self.set(s,'pvr_gpu_service',name,1,8)
        self.set(s,'pvr_gpu_service','deadline',16000000,8)
        self.set(b,'a733_gpu_backend','phase',self.enum('A733_GPU_BACKEND_ISOLATED'))
        self.set(p,'a733_gpu_platform','phase',self.enum('A733_GPU_PLATFORM_OFF'))
        self.set(memory,'a733_gpu_memory','phase',self.enum('A733_GPU_MEMORY_OFFLINE'))
        stop=s+self.image.layouts['pvr_gpu_service']['stop']
        self.set(stop,'pvr_gpu_stop','phase',self.enum('PVR_GPU_STOP_STOPPED'))
        self.set(stop,'pvr_gpu_stop','isolation_observed',1)
        ack=stop+self.image.layouts['pvr_gpu_stop']['acknowledgement'];self.put(ack,1,4);self.put(ack+4,1,4)
        self.set(s+self.image.layouts['pvr_gpu_service']['queue'],'pvr_ccb','phase',self.enum('PVR_CCB_RETIRED'))
        self.set(s+self.image.layouts['pvr_gpu_service']['control'],'pvr_gpu_control','phase',self.enum('PVR_GPU_CONTROL_RETIRED'))
        self.set(self.cc,'pvr_compute_service','phase',self.enum('PVR_COMPUTE_SERVICE_CLEANED'))
        self.set(self.cc,'pvr_compute_service','cleanup_status',1)
        cfg_at=self.get(self.cc+self.image.layouts['pvr_compute_service']['config'])
        context=self.get(cfg_at+self.image.layouts['pvr_compute_service_config']['firmware_address'],4)
        self.set(self.cc,'pvr_compute_service','cleanup_context',context)
        token=self.cc+self.image.layouts['pvr_compute_service']['cleanup_token']
        self.set(token,'pvr_ccb_token','epoch',1,8);self.set(token,'pvr_ccb_token','serial',3,8)
        assert self.call('a733_gpu_backend_stopped',b,s)==1
        assert not self.get(s+self.image.layouts['pvr_gpu_service']['config']+self.image.layouts['pvr_gpu_service_config']['graphics'])
        self.checkpoint('stopped')

def preflight_owner(m,out):
    functions=['gpu_application_package','pvr_compute_package_valid','a733_memory_reservations_commit_verified',
        'a733_memory_uncached_commit_verified','a733_gpu_reserved_prepare','a733_gpu_compute_reserved_prepare_package',
        'a733_gpu_backend_bind','pvr_gpu_service_init','pvr_gpu_service_bind','a733_gpu_backend_stopped',
        'a733_gpu_disposal_begin','a733_gpu_disposal_poll','a733_gpu_disposal_rebuild','device_memory_lookup']
    resolved={n:m.image.address(n) for n in functions}
    for n in ('worker','prepare'):resolved['service.'+n]=m.image.address(n,'/drivers/gpu/pvr_gpu_service.c')
    required=['gpu_application','gpu_program_session','a733_gpu_disposal','pvr_gpu_service','pvr_gpu_service_config',
        'a733_gpu_reserved','a733_gpu_compute_reserved','a733_gpu_backend_external','a733_gpu_platform',
        'pvr_compute_service','pvr_gpu_stop','pvr_ccb','pvr_gpu_control','pvr_ccb_token','device_memory_lease',
        'pvr_gpu_service_message','pvr_gpu_service_request','pvr_gpu_service_reply']
    assert all(n in m.image.layouts and n in m.sizes for n in required)
    record=dict(image_sha256=sha(m.path),functions=resolved,hooks=m.hooks,types=required,
        return_widths='int/poll results use W0; pointer results use X0; leases32B passed indirectly',
        scheduler='single-core SRE: CurrentEL4, SPSel1, counter24MHz; vPort critical boundaries modeled; actual worker_stack',
        providers='actual extended commit_verified/pool/owner/backend/service initialization; inherited provider construction and complete healthy stop modeled',
        pool_bounds={n:m.image.address(n) for n in ('__device_pool_start','__device_pool_end','__uncached_pool_start','__uncached_pool_end')})
    (out/'preflight.json').write_text(json.dumps(record,indent=2)+'\n')

def owner_case(a):
    m=Owner(a.image);m.out=a.output;preflight_owner(m,a.output)
    try:return owner_steps(a,m)
    except BaseException:
        m.checkpoint('failure-owner');raise

def owner_steps(a,m):
    if a.resume:m.restore(a.resume)
    else:m.make_compute(a.package.read_bytes())
    old={n:bytes(m.u.mem_read(m.c+m.image.layouts['a733_gpu_compute_reserved'][n],32)) for n in ('payload_lease','firmware_lease','table_lease')}
    global_leases=[bytes(m.u.mem_read(m.f+m.image.layouts['a733_gpu_reserved'][n],32)) for n in ('wb_lease','nc_lease')]
    package_at=m.call('gpu_application_package');package=bytes(m.u.mem_read(package_at,8288))
    reply=m.control(m.enum('PVR_GPU_SERVICE_DISPOSE'),1,15000000);assert reply['result']==0
    for steps in range(500):
        m.tick+=24000;r=m.call('a733_gpu_disposal_poll',m.s)&0xffffffff
        if r:break
    assert r==1 and m.get(m.d+m.image.layouts['a733_gpu_disposal']['count'],4)==3
    assert m.get(m.d+m.image.layouts['a733_gpu_disposal']['released'],4)==7
    m.set(m.s,'pvr_gpu_service','phase',m.enum('PVR_GPU_SERVICE_DISPOSED'));m.checkpoint('disposed')
    for value in old.values():
        m.u.mem_write(0x90003000,value);assert m.call('device_memory_lookup',int.from_bytes(value[:8],'little'),0x90003000)==0
    opened=m.control(m.enum('PVR_GPU_SERVICE_OPEN'),1,15000000)
    assert opened['result']==0 and opened['epoch']==2
    for name,value in old.items():
        fresh=bytes(m.u.mem_read(m.c+m.image.layouts['a733_gpu_compute_reserved'][name],32))
        assert int.from_bytes(fresh[16:24],'little')>int.from_bytes(value[16:24],'little')
    assert package_at==m.call('gpu_application_package') and bytes(m.u.mem_read(package_at,8288))==package
    assert m.get(m.d+m.image.layouts['a733_gpu_disposal']['package'])==package_at
    for name,value in zip(('wb_lease','nc_lease'),global_leases):assert bytes(m.u.mem_read(m.f+m.image.layouts['a733_gpu_reserved'][name],32))==value
    m.checkpoint('reconstructed');m.publishing=True;m.worker_stop=None
    m.call('worker',m.s,unit='/drivers/gpu/pvr_gpu_service.c');m.checkpoint('reopened-publication')
    record=dict(passed=True,image_sha256=sha(a.image),package_sha256=sha(a.package),child_parents=3,steps=steps+1,
        modeled_elapsed_us=(steps+1)*1000,new_open=opened,package_retained_outside_children=True,scope=__doc__)
    (a.output/'result.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record))

class Application(texture.Machine):
    def __init__(self,image,package,reference):
        texture.transport.Machine.__init__(self,image)
        self.speed=2;self.states=[];self.contexts={};self.incoming=bytearray()
        self.pending=None;self.claimed=False;self.closed=False;self.disposed=False
        self.frame=0;self.ticket=0;self.generation=0;self.zlps=0;self.packets=0;self.reads=0
        self.queue_epoch=1;self.sequence=0;self.operation=0;self.first=0;self.phase=3
        self.reference=reference;self.total=len(reference)//4;self.hold_open=False;self.held_status=False
        self.control_budgets=[];self.critical=0
        from elftools.elf.elffile import ELFFile
        with image.path.open('rb') as f:
            sizes=[int(d.attributes['DW_AT_byte_size'].value) for cu in ELFFile(f).get_dwarf_info().iter_CUs() for d in cu.iter_DIEs()
                if d.tag=='DW_TAG_structure_type' and d.attributes.get('DW_AT_name') and d.attributes['DW_AT_name'].value==b'pvr_gpu_service_reply' and 'DW_AT_byte_size' in d.attributes]
        assert sizes and len(set(sizes))==1;self.reply_size=sizes[0]
        for n in ('tud_vendor_n_write','tud_vendor_n_write_flush'):del self.hooks[image.address(n)]
        for n in ('pvr_gpu_service_collect','pvr_gpu_service_send_epoch','pvr_gpu_service_compute_read',
            'pvr_gpu_service_compute_read_epoch','pvr_gpu_service_compute_read_sequence',
            'vPortEnterCritical','vPortExitCritical','tud_speed_get','usbd_edpt_open','usbd_edpt_claim',
            'usbd_edpt_release','usbd_edpt_xfer'):self.hooks[image.address(n)]=n
        self.package_at=retain_package(self,package)
        self.call('vendord_init');configuration=self.call('tud_descriptor_configuration_cb',0)
        assert self.call('vendord_open',0,configuration+9,23)==23
    def hook(self,cpu,address,size,unused):
        name=self.hooks.get(address);a=[cpu.reg_read(UC_ARM64_REG_X0+i) for i in range(8)]
        if name=='vPortEnterCritical':self.critical+=1;self.ret();return
        if name=='vPortExitCritical':assert self.critical;self.critical-=1;self.ret();return
        if name=='vTaskDelay':assert not self.critical
        if name in ('pvr_gpu_service_send','pvr_gpu_service_send_epoch'):
            explicit=name.endswith('_epoch');q=a[2] if explicit else a[1];slot=a[4] if explicit else a[3]
            self.operation=int.from_bytes(cpu.mem_read(q,4),'little')
            timeout=int.from_bytes(cpu.mem_read(q+8,8),'little')
            if explicit:
                assert a[1]==self.queue_epoch
                assert timeout==(15000000 if self.operation in (1,11) else 1000000)
                self.control_budgets.append((self.operation,timeout))
            if self.operation==2:self.closed=True;self.phase=5
            elif self.operation==11:assert self.closed;self.disposed=True;self.phase=15
            elif self.operation==1 and explicit:
                assert self.disposed;self.queue_epoch+=1;self.sequence=0;self.phase=3;self.closed=self.disposed=False
            elif self.operation==3:
                self.sequence+=1
                if self.queue_epoch==2:self.hold_open=True
            else:assert self.operation in (0,1)
            self.ticket+=1;self.write(slot,self.ticket,8);self.ret(1);return
        if name in ('pvr_gpu_service_compute_read','pvr_gpu_service_compute_read_sequence','pvr_gpu_service_compute_read_epoch'):
            if name.endswith('_epoch'):
                assert a[1]==self.queue_epoch and a[2]==self.sequence
                self.first=a[3];slot=a[5]
            elif name.endswith('_sequence'):
                assert a[1]==self.sequence;self.first=a[2];slot=a[4]
            else:self.first=a[1];slot=a[3]
            self.operation=6;self.ticket+=1;self.write(slot,self.ticket,8);self.ret(1);return
        if name=='pvr_gpu_service_collect':
            if self.hold_open:
                assert not self.critical
                self.ret(0);self.stop='yield';cpu.emu_stop();return
            f=self.image.layouts['pvr_gpu_service_reply'];p=a[2];cpu.mem_write(p,bytes(self.reply_size))
            values=dict(ticket=a[1],epoch=self.queue_epoch,phase=self.phase,compute_sequence=self.sequence,
                compute_phase=10 if self.closed else 7 if self.sequence else 4)
            if self.operation==6:
                count=min(32,self.total-self.first)
                values.update(compute_first_word=self.first,compute_total_words=self.total,compute_word_count=count)
                cpu.mem_write(p+f['compute_words'],self.reference[self.first*4:(self.first+count)*4])
            for k,v in values.items():self.write(p+f[k],v,8 if k in ('epoch','ticket') else 4)
            self.ret(1);return
        super().hook(cpu,address,size,unused)

def protocol_case(a):
    assert a.client and a.reference and a.leaf_proof
    proof=json.loads(a.leaf_proof.read_text());assert proof['passed'] and proof['child_parents']==3 and proof['new_open']['epoch']==2
    image=leaf.base.Image(a.image);image.path=a.image
    resolved={n:image.address(n,'/apps/gpu-render/main.c' if n=='client' else None) for n in ('client','gpu_application_usb',
        'gpu_application_lifecycle','gpu_application_package','program_session','session_sealed','pvr_gpu_service_send',
        'pvr_gpu_service_send_epoch','pvr_gpu_service_collect','pvr_gpu_service_compute_read','pvr_gpu_service_compute_read_epoch',
        'pvr_gpu_service_compute_read_sequence','vPortEnterCritical','vPortExitCritical','vendord_init','vendord_open','vendord_xfer_cb')}
    package=a.package.read_bytes();reference=a.reference.read_bytes();assert len(reference)==260
    m=Application(image,package,reference);client=module('compute_lifecycle_host',a.client)
    client.time=types.SimpleNamespace(monotonic=lambda:m.now/1000000,sleep=lambda seconds:m.pump())
    records=[];held=[]
    registers=[UC_ARM64_REG_X0+i for i in range(31)]+[UC_ARM64_REG_SP,UC_ARM64_REG_PC,UC_ARM64_REG_PSTATE]+[UC_ARM64_REG_Q0+i for i in range(32)]+[UC_ARM64_REG_FPCR,UC_ARM64_REG_FPSR]
    def checkpoint(label):
        original=m.cpu.context_save();contexts={}
        for name,context in m.contexts.items():
            m.cpu.context_restore(context);contexts[name]={str(r):m.cpu.reg_read(r) for r in registers}
        m.cpu.context_restore(original)
        state=dict(image_sha256=sha(a.image),memory=[(at,bytes(m.cpu.mem_read(at,n))) for at,n in ((0x41000000,0x1200000),(0x43000000,0x1000000))],
            regs={str(r):m.cpu.reg_read(r) for r in registers},contexts=contexts,
            attrs={k:v for k,v in vars(m).items() if k not in ('image','cpu','contexts','hooks')},records=records,
            host={k:v for k,v in vars(c).items() if not callable(v)})
        with gzip.open(a.output/(label+'.pkl.gz'),'wb') as f:pickle.dump(state,f,protocol=5)
    def save(name,data):
        p=a.output/name;p.write_bytes(data);return dict(path=str(p.resolve()),sha256=sha(p))
    def exchange(request,remaining):
        assert not m.incoming;row=dict(request=save(f'{len(records):03d}-request.bin',request));records.append(row)
        op=struct.unpack_from('<I',request,8)[0]
        m.incoming+=request;raw=m.read(512,remaining)
        row['response']=save(f'{len(records)-1:03d}-response.bin',raw)
        assert len(raw)>=48 and (len(raw)%512)!=0
        h=struct.unpack_from('<10IQ',raw)
        if op==0 and m.hold_open:
            assert h[2]==0 and h[6]==1 and h[10]==1
            held.append(row);m.held_status=True;m.hold_open=False
        return raw
    c=client.Compute(exchange,123,65,deadline=60,retain=lambda kind,identity,data:None)
    if a.resume:
        with gzip.open(a.resume,'rb') as f:state=pickle.load(f)
        assert state['image_sha256']==sha(a.image)
        for at,data in state['memory']:m.cpu.mem_write(at,data)
        m.__dict__.update(state['attrs']);m.contexts={}
        for name,values in state['contexts'].items():
            for reg,value in values.items():m.cpu.reg_write(int(reg),value)
            m.contexts[name]=m.cpu.context_save()
        for reg,value in state['regs'].items():m.cpu.reg_write(int(reg),value)
        records=state['records'];c.__dict__.update(state['host'])
    preflight=dict(image_sha256=sha(a.image),package_sha256=sha(a.package),reference_sha256=sha(a.reference),functions=resolved,
        reply_extents=[176,176,52],control_extent=48,speed='high',terminal_zlp_expected=False,
        scope='Actual native app/parser/mailbox and pinned TinyUSB vendor FIFO/endpoint callbacks; DCD effects, service completion and GPU output modeled. Original absolute host/native deadline retained. Held OPEN compute waits until an actual old-epoch pending STATUS is copied.')
    (a.output/'preflight.json').write_text(json.dumps(preflight,indent=2)+'\n')
    try:
        if not a.resume:
            assert c.read()==reference;checkpoint('first-output')
        c.close();c.dispose();c.open();checkpoint('reopened')
        assert c.read()==reference;checkpoint('second-output')
        c.close();checkpoint('terminal')
        assert m.held_status and not m.hold_open and not m.critical and not m.zlps
    except BaseException:
        checkpoint('failure');raise
    input_bytes=struct.unpack_from('<I',package,28)[0];input_row=save('input.bin',package[4192:4192+input_bytes])
    session_at=image.address('program_session');session_bytes=image.layouts['gpu_program_session']['package']+8288
    session=save('session.bin',bytes(m.cpu.mem_read(session_at,session_bytes)))
    producer=dict(schema='pvr-gpu-lifecycle-native-producer-v1',modeled=True,physical_authority=False,
        image_sha256=sha(a.image),session_epoch=123,profile='compute',exchanges=records,session=session,
        leaf_proof=dict(path=str(a.leaf_proof.resolve()),sha256=sha(a.leaf_proof)),package=dict(path=str(a.package.resolve()),sha256=sha(a.package)),
        held_pending_open_status=len(held),control_budgets=m.control_budgets,endpoint_completions=m.packets,zlps=m.zlps,scope=preflight['scope'])
    pp=a.output/'producer.json';pp.write_text(json.dumps(producer,indent=2)+'\n')
    fixture=dict(schema='pvr-gpu-lifecycle-host-fixture-v1',modeled=True,physical_authority=False,
        image=dict(path=str(a.image.resolve()),sha256=sha(a.image)),session_epoch=123,profile='compute',words=65,
        inputs=[input_row],exchanges=records,producer=dict(path=str(pp.resolve()),sha256=sha(pp)))
    (a.output/'fixture.json').write_text(json.dumps(fixture,indent=2)+'\n')
    (a.output/'result.json').write_text(json.dumps(dict(passed=True,image_sha256=sha(a.image),exchanges=len(records),held_pending_open_status=True,
        copied_bytes_per_output=260,scope=preflight['scope']),indent=2)+'\n')
    print('PASS compute lifecycle application/TinyUSB transcript',len(records))


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('case',choices=('owner','protocol'))
    for n in ('image','package','output'):p.add_argument('--'+n,type=Path,required=True)
    p.add_argument('--resume',type=Path);p.add_argument('--client',type=Path);p.add_argument('--leaf-proof',type=Path);p.add_argument('--reference',type=Path)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    if a.case=='owner':owner_case(a)
    else:protocol_case(a)
if __name__=='__main__':main()
