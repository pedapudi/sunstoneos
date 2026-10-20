#!/usr/bin/env python3
"""Execute one linked source-over-solid job with explicit RCQ pixel effects.

Reuse accepted power/task model definitions, not their old cases or checkpoints.
Read services, RTOS identity, translations, mutexes and hardware remain modeled.
The ideal integer effect below cannot qualify hardware blending or rounding.
"""
from pathlib import Path
import hashlib,struct,json
# The accepted checker pins and verifies the original allocation/task models.
# Stop before its old power case batch; execute only its setup/class definitions.
check=Path(__file__).with_name('g2d-power-native.py')
prefix=check.read_text().split('\ntry:\n    m=Power();')[0]
assert prefix!=check.read_text()
anchor="for name in ('image','session','input','reference','output')"
assert prefix.count(anchor)==1
prefix=prefix.replace(anchor,"for name in ('image','session','input','reference','output','contract')")
assert prefix.count('a=p.parse_args();')==1
prefix=prefix.replace('a=p.parse_args();',"p.add_argument('--resume',type=Path);p.add_argument('--completed',action='store_true');a=p.parse_args();")
exec(compile(prefix,str(check),'exec'),globals())
for dependency in (check,ROOT/'apps/g2d/host.py',a.contract,*([a.resume] if a.resume else [])):
    path=dependency.resolve();data=path.read_bytes();index=len(loaded)
    loaded[path]=data;identity[str(path)]={'sha256':hashlib.sha256(data).hexdigest(),'bytes':len(data)}
    (inputs/(str(index)+'-'+path.name)).write_bytes(data)
(a.output/'inputs.json').write_text(json.dumps(identity,indent=2)+'\n')
verify_inputs()
from unicorn.arm64_const import UC_ARM64_REG_SP_EL0,UC_ARM64_REG_PSTATE
class Blend(Power):
    def __init__(self):
        super().__init__();self.publications=[]
        self.hooks[im.address('a733_cpu_coherence_publish')]='a733_cpu_coherence_publish'
    def hook(self,u,pc,size,unused):
        if self.hooks.get(pc)=='a733_cpu_coherence_publish':
            self.publications.append(self.args()[:3]);self.ret(1);return
        super().hook(u,pc,size,unused)
    def upload(self,source,sealed):
        supplied=self.base+self.fields['supplied'];epoch,generation=struct.unpack_from('<2Q',sealed,24)
        packet=struct.pack('<4I2Q',0x55444753,1,len(source),0,epoch,generation)+source;at=0;r=0
        deadline=self.get(im.address('board')+im.layouts['a733_board_session']['deadline_us'])-10000
        while at<len(packet):
            n=min(197,self.call('g2d_input_remaining',supplied),len(packet)-at);assert n
            self.u.mem_write(SCRATCH,packet[at:at+n]);r=self.call('g2d_input_feed',supplied,self.base,im.address('a733_counter_clock_now'),0,deadline,SCRATCH,n);assert r in (0,1);at+=n
        assert r==1 and self.call('g2d_input_ready',supplied)==1
    def call(self,name,*args):
        self.u.reg_write(UC_ARM64_REG_SP_EL0,STACK);self.u.reg_write(UC_ARM64_REG_PSTATE,4)
        return super().call(name,*args)
    def checkpoint(self,path):
        state=super().checkpoint(path);state['publications']=self.publications
        path.write_bytes(gzip.compress(pickle.dumps(state),compresslevel=1));return state
    def command_effect(self):
        head=bytes(self.u.mem_read(self.mapped(0x10800000,128,1),128));r={}
        for i in range(8):
            lo,length,dirty,offset=struct.unpack_from('<4I',head,i*16)
            if not dirty:
                assert i==7 and not any((lo,length,offset));continue
            assert dirty==1 and lo%32==0 and length%4==0 and length
            data=bytes(self.u.mem_read(self.mapped(lo,length,1),length))
            for at in range(0,length,4):
                assert offset+at not in r
                r[offset+at]=int.from_bytes(data[at:at+4],'little')
        width,height,stride,fmt,op,dx,dy,w,h,sx,sy,color=struct.unpack_from('<12I',session,856)
        assert op==3 and color>>24==255
        size=(w-1)|((h-1)<<16)
        assert r[0x1000]==r[0x1800]==r[0x8000]==0
        assert r[0x800]==0xff000001 and r[0x2000]==0xff000011
        assert r[0x400]==0x301 and r[0x44c]==0x03010301 and r[0x480]==0xf0 and r[0x484]==0x41000
        assert r[0x440]==r[0x460]==0 # Straight input and straight output.
        assert all(r[k]==size for k in (0x804,0x82c,0x2004,0x201c,0x420,0x424,0x448,0x3004))
        assert r[0x808]==r[0x2008]==r[0x430]==r[0x434]==r[0x3000]==0
        assert r[0x80c]==r[0x200c]==r[0x3008]==stride
        assert r[0x818]==0x10000000+sy*stride+sx*4 and r[0x2010]==0 and r[0x2014]==color
        assert r[0x3014]==0x10400000+dy*stride+dx*4 and r[0x3018]==0
        # Interpret the emitted two layers and PD mode. This is a declared
        # ideal arithmetic model; it is not hardware-produced pixel evidence.
        background=r[0x2014].to_bytes(4,'little')
        for y in range(h):
            pixels=bytes(self.u.mem_read(self.mapped(r[0x818]+y*r[0x80c],w*4,1),w*4));row=bytearray()
            for x in range(w):
                b,g,red,alpha=pixels[x*4:x*4+4]
                row.extend((value*alpha+background[c]*(255-alpha)+127)//255 for c,value in enumerate((b,g,red)))
                row.append(255)
            self.u.mem_write(self.mapped(r[0x3014]+y*r[0x3008],w*4,2),bytes(row))
        self.events.append(('modeled-source-over-solid',w,h))
        self.expected=host.expected_output(raw,session)

try:
    host=load('g2d_blend_host',ROOT/'apps/g2d/host.py')
    assert not a.completed or a.resume
    m=Blend()
    if a.resume:
        m.restore(pickle.loads(gzip.decompress(a.resume.read_bytes())))
        if a.completed:assert m.started==1 and m.result()==1
        else:assert m.started==0 and not m.get(m.base+m.fields['wb_pool']) and not m.get(m.base+m.fields['pool'])
    else:
        m.u.mem_write(SCRATCH,session);assert m.call('a733_g2d_session_valid',SCRATCH)==1
        invalid=bytearray(session);invalid[903]=254;m.u.mem_write(SCRATCH,bytes(invalid))
        assert m.call('a733_g2d_session_valid',SCRATCH)==0 and m.started==0
    # Consume the one root-published contract. Publication visibility remains
    # an explicit callback boundary; no EL3/cache behavior is inferred.
    if not a.completed:
        contract=loaded[a.contract.resolve()];assert len(contract)==288
        cf=im.layouts['a733_smp_contract'];armed=struct.unpack_from('<Q',contract,cf['armed_at'])[0]
        m.now=armed//24+100;m.u.mem_write(0x41f00000,session)
        # Model the root's completed publication at both actual retained locations;
        # the real getter enforces its pointer and full live-copy equality checks.
        record=im.address('g2d_admission');m.u.mem_write(record,contract);m.u.mem_write(0x42004000,contract)
        assert m.call('a733_smp_contract_valid',record,m.now*24)==1
        assert m.call('g2d_session_prepare',record)==1 and len(m.publications)==1
        retained=m.call('g2d_admitted_session');assert retained and bytes(m.u.mem_read(retained,904))==session
        board=im.address('board');m.u.mem_write(board,session[:592])
        m.put(board+im.layouts['a733_board_session']['deadline_us'],struct.unpack_from('<Q',contract,cf['diagnostic_deadline'])[0]//24,8)
        m.pools();m.upload(raw,session);m.configure()
        m.regs[PCK]=0xa5000008;m.regs[PCK+8]=8 # Reuse accepted coherent-ON path.
        (a.output/'preflight.json').write_text(json.dumps(dict(
            scope='Root post-publication contract bytes supplied as model state at actual private/live addresses; no startup execution. Prepared memory, owner/time/PMIC and hardware observations modeled as in accepted power fixture.',
            contract_sha256=digest(a.contract),private_record=record,live_record=0x42004000,
            modeled_publications=m.publications,now_us=m.now,
            native_deadline_us=m.get(board+im.layouts['a733_board_session']['deadline_us'])-10000,
            initial_mmio={hex(k):v for k,v in m.regs.items()},model_sources=PINS,
            physical_access=False),indent=2)+'\n')
        m.run()
    assert m.result()==1 and m.started==1
    contract=loaded[a.contract.resolve()];assert len(contract)==288
    assert bytes(m.u.mem_read(im.address('g2d_admission'),288))==contract
    assert bytes(m.u.mem_read(0x42004000,288))==contract
    copied=bytes(m.u.mem_read(m.get(m.base+m.fields['payload']),len(raw)))
    assert copied==m.expected and m.get(m.base+m.fields['copied'])==len(raw)
    assert m.get(m.base+m.fields['guards_ok'],4)==1 and all(row['state']==2 for row in m.rows()['uncached_pool'])
    assert not [e for e in m.events if e[0]=='domain-write']
    # Decode the actual table returned by a733_iommu_master(), rather than
    # treating global bank1 offsets such as 0x10040 as micro registers.
    mf=im.layouts['a733_iommu_master'];ms=im.sizes['a733_iommu_master'];masters=im.address('masters')
    descriptors=[(m.get(masters+i*ms+mf['id'],1),m.get(masters+i*ms+mf['micro_offset'],4)) for i in range(10)]
    microsets=tuple(base for identifier,base in descriptors)
    selected=[base for identifier,base in descriptors if identifier==10];assert len(selected)==1
    micro={e[1] for e in m.events if e[0]=='iommu-read' and any(e[1]-base in (0x38,0x3c,0x40,0x44) for base in microsets)}
    assert micro=={selected[0]+offset for offset in (0x38,0x3c,0x40,0x44)}
    globals_=sorted({e[1] for e in m.events if e[0]=='iommu-read' and (e[1]&0xffff)<0x400})
    assert 0x10040 in globals_
    if a.completed:(a.output/'copied.pkl.gz').write_bytes(a.resume.read_bytes())
    else:m.checkpoint(a.output/'copied.pkl.gz')
    (a.output/'modeled-output.argb').write_bytes(copied)
    table=m.get(m.base+m.fields['table']);(a.output/'tables.bin').write_bytes(bytes(m.u.mem_read(table,32768)))
    report=dict(passed=True,case='source-over-solid',submissions=m.started,malformed_background_refused=True,
        copied=len(copied),retained=True,contract_sha256=digest(a.contract),private_session_checked=True,contract_state='Root post-publication bytes supplied, not startup execution',image_sha256=digest(a.image),session_sha256=digest(a.session),
        reference_scope='Ideal straight-alpha /255 nearest only; physical rounding unqualified.',
        physical_access=False,passive_terminal_verification=a.completed,micro_registers=sorted(micro),global_registers=globals_,events=m.events,allocations=m.rows(),model_sources=PINS)
    (a.output/'result.json').write_text(json.dumps(report,indent=2)+'\n');verify_inputs()
except BaseException as error:
    if 'm' in globals():m.checkpoint(a.output/'interrupted.pkl.gz')
    (a.output/'failure.json').write_text(json.dumps(dict(error=repr(error)),indent=2)+'\n');raise
