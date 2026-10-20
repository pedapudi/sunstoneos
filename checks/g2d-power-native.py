#!/usr/bin/env python3
"""Changed G2D power branches on one linked image; all hardware is modeled.

Reuse the pinned allocation/task models, executing only their class definitions.
No old image, checkpoint, symbol address, session identity or result is reused.
PMIC reads, counter, scheduler/task identity, Device translations, mutexes and
RCQ pixel effects remain explicit model boundaries, not physical evidence.
"""
import argparse,gzip,hashlib,importlib.util,json,pickle,struct,types
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_LR,UC_ARM64_REG_SP,UC_ARM64_REG_PC,UC_ARM64_REG_PSTATE

ROOT=Path(__file__).resolve().parents[1]
STOP=0x90fff000;STACK=0x90ffeff0;SCRATCH=0x90e00000
PCK=0x07061000
MMIO=[(0x02002000,0x2000),(0x07010000,0x1000),(0x07060000,0x2000),
      (0x07090000,0x1000),(0x03900000,0x20000),(0x05440000,0x40000)]
PINS={'allocation-check.py':'f0f62a2c4d68a9f1446f34066f335d143c34cb55410fa65e46d2e446bb9ae2d9',
      'task-check.py':'84da1eb3d75a6b470778585cc715510e76f94d71becded6443cf5187fb1c556d'}
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

p=argparse.ArgumentParser(description=__doc__)
for name in ('image','session','input','reference','output'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
paths=[a.image,a.session,a.input,Path(__file__),*(a.reference/n for n in PINS),
    *(ROOT/n for n in ('checks/jpeg-encode-native.py','checks/jpeg-runtime-native.py',
        'apps/g2d/prepare.py','apps/board/prepare.py','tools/check-a733-smp.py',
        'tools/check-a733-smp-supervised.py','tools/check-a733-smp-observe.py'))]
loaded={p.resolve():p.read_bytes() for p in paths}
identity={str(p):{'sha256':hashlib.sha256(b).hexdigest(),'bytes':len(b)} for p,b in loaded.items()}
inputs=a.output/'inputs';inputs.mkdir()
for i,(path,data) in enumerate(loaded.items()):(inputs/(str(i)+'-'+path.name)).write_bytes(data)
(a.output/'inputs.json').write_text(json.dumps(identity,indent=2)+'\n')
def verify_inputs():
    for path,data in loaded.items():assert path.read_bytes()==data,('input changed',str(path))
helpers=load('g2d_power_image',ROOT/'checks/jpeg-encode-native.py')
im=helpers.Image(a.image);session=loaded[a.session.resolve()];raw=loaded[a.input.resolve()]
assert len(session)==904 and hashlib.sha256(raw).digest()==session[768:800]
prep=load('g2d_power_prepare',ROOT/'apps/g2d/prepare.py');prep.embedded_reference(a.image.read_bytes(),session)
for name,expected in PINS.items():assert digest(a.reference/name)==expected
allocation=(a.reference/'allocation-check.py').read_text()
allocation=allocation[allocation.index('class Machine:'):allocation.index('p=argparse.ArgumentParser();')]
exec(compile(allocation,str(a.reference/'allocation-check.py'),'exec'),globals())
api=types.SimpleNamespace(Machine=Machine,SCRATCH=SCRATCH)
task=(a.reference/'task-check.py').read_text()
task=task[task.index('class Task('):task.index("case=sys.argv[1]")]
COMMANDS=True;EV=a.image.parent.parent;OUT=a.output
exec(compile(task,str(a.reference/'task-check.py'),'exec'),globals())
verify_inputs()

class Power(Task):
    def __init__(self):
        super().__init__();self.mode='off';self.domain_reads=0;self.lock_bounds=[]
        self.injections=0
        self.phase='fresh-call-boundary'
        self.u.reg_write(UC_ARM64_REG_PSTATE,4) # Actual privileged task EL1t/SP_EL0.
        self.task_handle=im.address('g2d')+im.layouts['g2d_application']['task']
        self.put(self.base+self.fields['handle'],self.task_handle,8)
        self.hooks[im.address('xTaskGetCurrentTaskHandle')]='xTaskGetCurrentTaskHandle'
        self.poll=im.address('a733_de_sys_power_poll');self.saved=False
    def hook(self,u,pc,size,unused):
        name=self.hooks.get(pc)
        if name=='xTaskGetCurrentTaskHandle':self.ret(self.task_handle);return
        if name=='a733_platform_lock_enter':self.lock_bounds.append(self.args()[2])
        if name=='vTaskDelay':
            # Serialized task model: one-tick delay wakes at the next tick.
            self.now=(self.now//1000+1)*1000
        if pc==self.poll and not self.saved:
            self.saved=True;self.checkpoint(a.output/'before-domain.pkl.gz')
        # Named callback hooks take precedence over instruction inspection.
        # Replaced counter/translation/scheduler services own their MRS fields.
        if name is None and pc not in self.callback_map and pc!=STOP:
            word=int.from_bytes(u.mem_read(pc,4),'little')
            assert word&0xfff00000!=0xd5300000,('unexpected executed MRS',hex(pc),hex(word))
        super().hook(u,pc,size,unused)
    def call(self,name,*args):
        self.phase='executing:'+name
        result=super().call(name,*args);self.phase='fresh-call-boundary';return result
    def configure(self):
        super().configure();self.regs[PCK]=0xa5000000;self.regs[PCK+8]=0
    def read_mmio(self,u,access,at,size,value,unused):
        if at==PCK+8 and self.regs[PCK]&15==8 and self.mode!='timeout':
            self.domain_reads+=1
            if self.domain_reads==2:self.regs[PCK+8]=8
        if PCK<=at<PCK+0x1000:
            ready=self.get(self.base+self.fields['domain']+im.layouts['a733_de_sys_power']['state'],4)==2
            assert self.held or (ready and at in (PCK,PCK+8))
            self.events.append(('domain-read',at,self.regs.get(at,0)))
        super().read_mmio(u,access,at,size,value,unused)
    def write_mmio(self,u,access,at,size,value,unused):
        if PCK<=at<PCK+0x1000:
            assert self.held;self.regs[at]=value;self.events.append(('domain-write',at,value))
            if self.mode=='uncertain':self.injections+=1;self.now=1000001
        super().write_mmio(u,access,at,size,value,unused)
    def checkpoint(self,path):
        state={k:getattr(self,k) for k in ('now','regs','events','started','rcq_status','variant',
            'held','callback_map','mode','domain_reads','lock_bounds','task_handle','saved','phase','injections')}
        state.update(image=identity[str(a.image.resolve())]['sha256'],
            session=identity[str(a.session.resolve())]['sha256'],
            cpu=self.u.context_save(),
            memory=[(at,bytes(self.u.mem_read(at,n))) for at,n in
                [(0x41000000,0x3000000),(0x48000000,0x1000000),(0x78000000,0x1000000),(0x90000000,0x1000000)]+MMIO])
        if hasattr(self,'expected'):state['expected']=self.expected
        path.write_bytes(gzip.compress(pickle.dumps(state),compresslevel=1));return state
    def restore(self,state):
        state=pickle.loads(pickle.dumps(state)) # Every fork owns nested maps/lists.
        assert state['image']==identity[str(a.image.resolve())]['sha256']
        assert state['session']==identity[str(a.session.resolve())]['sha256']
        for at,data in state['memory']:self.u.mem_write(at,data)
        for k,v in state.items():
            if k not in ('memory','image','session','cpu'):setattr(self,k,v)
        # Forks intentionally begin fresh task-function calls with saved data;
        # interrupted CPU context is preserved for diagnosis, not relabeled.
        self.phase='fresh-call-fork-from-provider-entry'

try:
    m=Power();m.pools();m.upload(raw,session);m.configure()
    assert m.call('a733_g2d_session_valid',m.base)==1
    preflight={'modeled':True,'physical_authority':False,'image':digest(a.image),'session':digest(a.session),
        'input':digest(a.input),'model_sources':PINS,'task_handle':m.task_handle,
        'initial_mmio':{hex(k):v for k,v in m.regs.items()},
        'pmic_read_service':{'chip':3,'revision':0,'page_before':0,'page_after':0,'rail':1,'enable':2,'selector':30,'programmed_uv':800000},
        'scope':__doc__}
    (a.output/'preflight.json').write_text(json.dumps(preflight,indent=2)+'\n')
    m.run();assert m.result()==1 and m.started==1 and m.expected==bytes(m.u.mem_read(m.get(m.base+m.fields['payload']),len(raw)))
    writes=[e for e in m.events if e[0]=='domain-write']
    assert writes==[('domain-write',PCK+off,value) for off,value in
        [(0x170,0x1f1f1f),(0x174,0x1f1f),(0xc00,0x08080808),(0xc04,0x0808),(0xc10,8),(0,0xa5000008)]]
    m.checkpoint(a.output/'off-on-result.pkl.gz')
    reports=[dict(case='off-on-copy',result=m.result(),submissions=m.started,writes=writes,retained=True)]
    state=pickle.loads(gzip.decompress((a.output/'before-domain.pkl.gz').read_bytes()))
    assert state['image']==digest(a.image) and state['session']==digest(a.session)
    df=im.layouts['a733_de_sys_power'];domain=m.base+m.fields['domain']
    for case in ('on-adopt','incoherent','timeout','uncertain'):
        m=Power();m.restore(state)
        assert m.regs==state['regs'] and m.events==state['events'] and not m.held and m.started==0
        assert m.result()==0 and m.get(domain+df['state'],4)==0 and m.get(domain+df['write_attempted'],4)==0
        m.mode=case;m.saved=True;m.lock_bounds=[]
        if case in ('incoherent','timeout'):m.injections=1
        if case=='on-adopt':m.regs[PCK]=0xa5000008;m.regs[PCK+8]=8
        if case=='incoherent':m.regs[PCK]=0xa5000008;m.regs[PCK+8]=0
        before=len(m.events)
        for tick in range(40):
            m.tick()
            if case=='on-adopt' and m.get(domain+df['state'],4)==2:break
            if m.result():break
        else:raise AssertionError(('power fork did not terminate',case))
        changed=m.events[before:];writes=[e for e in changed if e[0]=='domain-write']
        assert m.started==0
        if case=='on-adopt':
            assert not writes and m.get(domain+df['state'],4)==2 and not m.held
            assert m.get(domain+df['inherited_on'],4)==1
        else:
            assert m.result()==2 and m.get(domain+df['state'],4)==3
            assert m.injections==1
            if case=='incoherent':
                assert not writes and m.held and m.get(domain+df['write_attempted'],4)==0
                assert m.regs[PCK]&15==8 and m.regs[PCK+8]&15==0
            if case=='uncertain':assert len(writes)==1 and m.held and m.get(domain+df['write_attempted'],4)==1
            if case=='timeout':
                assert len(writes)==6 and not m.held
                bound=m.get(domain+df['wait_deadline'])
                assert len(m.lock_bounds)>1 and m.lock_bounds[0]>bound and all(d==bound for d in m.lock_bounds[1:])
                assert m.now>=bound and m.regs[PCK]&15==8 and m.regs[PCK+8]&15==0
            count=len(m.events);m.tick();assert len(m.events)==count and m.result()==2
        m.checkpoint(a.output/(case+'.pkl.gz'))
        reports.append(dict(case=case,result=m.result(),submissions=m.started,writes=writes,held=m.held,injections=m.injections,retained=True))
    verify_inputs()
    (a.output/'result.json').write_text(json.dumps({'passed':True,'cases':reports,'modeled':True,'physical_authority':False},indent=2)+'\n')
    print(json.dumps({'passed':True,'cases':len(reports),'physical_authority':False}))
except BaseException as error:
    if "m" in globals():m.checkpoint(a.output/"interrupted.pkl.gz")
    (a.output/"failure.json").write_text(json.dumps({"error":repr(error),"phase":getattr(globals().get("m"),"phase",None),"case":getattr(globals().get("m"),"mode",None)},indent=2)+"\n")
    raise
