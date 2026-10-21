#!/usr/bin/env python3
"""Execute one retained scale2x job and copy with explicitly injected pixels.

Linked session, allocator, upload, provider, RCQ, success lifecycle, IOMMU and
copy instructions execute. Device pixels are arbitrary tagged data, not an
interpolation implementation. Services, translations and publication are modeled.
"""
from pathlib import Path
import hashlib,json,struct
CHECK_SCOPE=__doc__
ROOT=Path(__file__).resolve().parents[1]
checker=ROOT/'checks/g2d-blend-native.py'
definitions=checker.read_text().split('\ntry:\n    host=load')[0]
assert definitions!=checker.read_text()
definitions=definitions.replace("('image','session','input','reference','output','contract')","('image','session','input','reference','output','contract','commands')")
exec(compile(definitions,str(checker),'exec'),globals())
for dependency in (checker,a.commands,Path(__file__)):
    path=dependency.resolve();data=path.read_bytes();index=len(loaded);loaded[path]=data
    identity[str(path)]={'sha256':hashlib.sha256(data).hexdigest(),'bytes':len(data)}
    (inputs/(str(index)+'-'+path.name)).write_bytes(data)
(a.output/'inputs.json').write_text(json.dumps(identity,indent=2)+'\n');verify_inputs()
BASE=0x05440000
class Scale(Blend):
    def __init__(self):
        super().__init__();self.ack_writes=[];self.reset_writes=[];self.output_reads=0
        self.u.hook_add(UC_HOOK_MEM_READ,self.output_read,begin=0x78000000,end=0x78ffffff)
    def output_read(self,u,access,address,size,value,unused):
        output=self.get(self.base+self.fields['output']);n=self.get(self.base+self.fields['bytes'])
        if output and output<=address<output+n:
            assert self.get(self.base+self.fields['finish_ready'],4)==self.get(self.base+self.fields['complete'],4)==1
            self.output_reads+=1
    def configure(self):
        super().configure();self.regs[PCK]=0xa5000008;self.regs[PCK+8]=8;self.regs[BASE+8]=0xa5a50003
    def command_effect(self):
        commands=bytes(self.u.mem_read(self.mapped(0x10800000,4096,1),4096))
        assert commands==loaded[a.commands.resolve()]
        width,height,stride,fmt,op,dx,dy,w,h,sx,sy,color=struct.unpack_from('<12I',session,856)
        assert (width,height,stride,fmt,op,dx,dy,w,h,sx,sy,color)==(60,64,256,0,4,9,17,32,24,40,42,0)
        # Device reader touches only the programmed source crop. The injected
        # output below is tagged data and makes no interpolation/rounding claim.
        for y in range(h//2):
            data=bytes(self.u.mem_read(self.mapped(0x10000000+(sy+y)*stride+sx*4,w//2*4,1),w//2*4))
            assert data==raw[(sy+y)*stride+sx*4:(sy+y)*stride+(sx+w//2)*4]
        result=bytearray(len(raw))
        for y in range(h):
            row=bytes(v for x in range(w) for v in ((17*x+y)&255,(x+13*y)&255,(9*x+7*y)&255,(255-x-y)&255))
            at=(dy+y)*stride+dx*4;self.u.mem_write(self.mapped(0x10400000+at,len(row),2),row);result[at:at+len(row)]=row
        self.expected=bytes(result);self.events.append(('injected-nonnumeric-scale-output',w,h))
    def read_mmio(self,u,access,address,size,value,unused):
        if address==BASE+8:self.put(address,self.regs[address],size);return
        super().read_mmio(u,access,address,size,value,unused)
    def write_mmio(self,u,access,address,size,value,unused):
        if address==BASE+0x24 and self.started:
            assert value==1;self.ack_writes.append(value)
        if address==BASE+8 and self.started:
            assert self.ack_writes==[1] and not self.rcq_status&1
            assert not self.get(self.base+self.fields['finish_ready'],4) and not self.get(self.base+self.fields['complete'],4)
            self.reset_writes.append(value);self.regs[address]=value
            if not value&1:self.rcq_status=0
        super().write_mmio(u,access,address,size,value,unused)
    def checkpoint(self,path):
        state=super().checkpoint(path)
        for name in ('ack_writes','reset_writes','output_reads'):state[name]=getattr(self,name)
        path.write_bytes(gzip.compress(pickle.dumps(state),compresslevel=1));return state
try:
    assert not a.resume and not a.completed
    m=Scale();m.u.mem_write(SCRATCH,session);assert m.call('a733_g2d_session_valid',SCRATCH)==1
    contract=loaded[a.contract.resolve()];assert len(contract)==288
    cf=im.layouts['a733_smp_contract'];m.now=struct.unpack_from('<Q',contract,cf['armed_at'])[0]//24+100
    m.u.mem_write(0x41f00000,session);record=im.address('g2d_admission');m.u.mem_write(record,contract);m.u.mem_write(0x42004000,contract)
    assert m.call('a733_smp_contract_valid',record,m.now*24)==1 and m.call('g2d_session_prepare',record)==1
    retained=m.call('g2d_admitted_session');assert bytes(m.u.mem_read(retained,904))==session
    m.put(im.address('board')+im.layouts['a733_board_session']['deadline_us'],struct.unpack_from('<Q',contract,cf['diagnostic_deadline'])[0]//24,8)
    m.pools();m.upload(raw,session);m.configure();m.checkpoint(a.output/'prepared.pkl.gz')
    (a.output/'preflight.json').write_text(json.dumps(dict(contract_sha256=digest(a.contract),scope='Actual mode1 session validator and private/live contract path; root publication state supplied, not startup execution. Prepared pools and services modeled.',initial_mmio={hex(k):v for k,v in m.regs.items()},physical_access=False),indent=2)+'\n')
    m.run();assert m.started==1 and m.result()==1 and m.injections==0
    assert m.ack_writes==[1] and m.reset_writes==[0xa5a50002,0xa5a50003] and m.rcq_status==0
    assert m.get(m.base+m.fields['finish_ready'],4)==m.get(m.base+m.fields['complete'],4)==1
    copied=bytes(m.u.mem_read(m.get(m.base+m.fields['payload']),len(raw)));assert copied==m.expected
    assert m.get(m.base+m.fields['copied'])==len(raw) and m.output_reads==len(raw)
    assert m.get(m.base+m.fields['guards_ok'],4)==1 and all(row['state']==2 for row in m.rows()['uncached_pool'])
    assert bytes(m.u.mem_read(record,288))==bytes(m.u.mem_read(0x42004000,288))==contract
    mf=im.layouts['a733_iommu_master'];ms=im.sizes['a733_iommu_master'];masters=im.address('masters')
    descriptors=[(m.get(masters+i*ms+mf['id'],1),m.get(masters+i*ms+mf['micro_offset'],4)) for i in range(10)]
    microsets=tuple(base for identifier,base in descriptors);selected=[base for identifier,base in descriptors if identifier==10];assert len(selected)==1
    micro={e[1] for e in m.events if e[0]=='iommu-read' and any(e[1]-base in (0x38,0x3c,0x40,0x44) for base in microsets)}
    assert micro=={selected[0]+offset for offset in (0x38,0x3c,0x40,0x44)}
    m.checkpoint(a.output/'copied.pkl.gz');(a.output/'modeled-output.argb').write_bytes(copied)
    table=m.get(m.base+m.fields['table']);(a.output/'tables.bin').write_bytes(bytes(m.u.mem_read(table,32768)))
    rows=[]
    for address,data in [(at,bytes(m.u.mem_read(at,n))) for at,n in ((0x41000000,0x3000000),(0x48000000,0x1000000),(0x78000000,0x1000000))]:
        name='checkpoint-'+hex(address)+'.bin';(a.output/name).write_bytes(data);rows.append(dict(address=address,bytes=len(data),file=name,sha256=hashlib.sha256(data).hexdigest()))
    (a.output/'usb-checkpoint.json').write_text(json.dumps(dict(image_sha256=digest(a.image),session_sha256=digest(a.session),native_checkpoint_sha256=digest(a.output/'copied.pkl.gz'),memory=rows,scope='Exact copied native state for one subsequent USB serialization/transport; no G2D replay.'),indent=2)+'\n')
    (a.output/'result.json').write_text(json.dumps(dict(passed=True,case='scale2x',submissions=1,copied=len(copied),retained=True,allocations=m.rows(),image_sha256=digest(a.image),session_sha256=digest(a.session),contract_sha256=digest(a.contract),micro_registers=sorted(micro),scope=CHECK_SCOPE,reference_checked=False,events=m.events,physical_access=False),indent=2)+'\n');verify_inputs()
except BaseException as error:
    if 'm' in globals():m.checkpoint(a.output/'interrupted.pkl.gz')
    (a.output/'failure.json').write_text(json.dumps(dict(error=repr(error)),indent=2)+'\n');raise
