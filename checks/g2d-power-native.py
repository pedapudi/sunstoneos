#!/usr/bin/env python3
"""Changed G2D power branches on one linked image; all hardware is modeled.

The G2D task requests PCK domain 1 through its row on the power-domain owner
(docs/power-domains.md) and reads the row's answer once per tick. The model
registers the row as board startup does, with a notification callback that
answers the row in place of the owner's service task: ON, REFUSED, FAILED or
never. The owner's own sequences are covered by checks/power-domains-pck.c.
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
# The row's notification callback: one RET instruction that the model hooks.
NOTIFY=0x90f00010
# enum a733_power_row_state and the generation mask (a733_power_domain_row.h).
ROW_PENDING,ROW_ON,ROW_REFUSED,ROW_FAILED=1,2,4,5
GENERATION_MASK=0xffffffff>>3
# PERI0 gate enable and status as both boards read them (bb9-read): every
# output's software and auto gate on. The pinned task model sets only the
# 400 MHz output's gates; G2D's PERI0_300M parent also needs 2X, 600M and 300M.
PERI0_GATES={0x02003908:0x8fff0fff,0x02003988:0x0fff0000}
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
        super().__init__();self.mode='on';self.answers=[];self.lock_bounds=[];self.notified=False
        self.phase='fresh-call-boundary'
        self.u.reg_write(UC_ARM64_REG_PSTATE,4) # Actual privileged task EL1t/SP_EL0.
        self.task_handle=im.address('g2d')+im.layouts['g2d_application']['task']
        self.put(self.base+self.fields['handle'],self.task_handle,8)
        self.hooks[im.address('xTaskGetCurrentTaskHandle')]='xTaskGetCurrentTaskHandle'
        # The device-pool ledger serializes in a kernel critical section; the
        # model runs one task on one core, so entry and exit only return.
        for name in ('vTaskEnterCritical','vTaskExitCritical'):self.hooks[im.address(name)]=name
        self.row=self.base+self.fields['row'];self.rf=im.layouts['a733_power_domain_row'];self.saved=False
        self.u.mem_write(NOTIFY,bytes.fromhex('c0035fd6'))
    def hook(self,u,pc,size,unused):
        name=self.hooks.get(pc)
        if name=='xTaskGetCurrentTaskHandle':self.ret(self.task_handle);return
        if name in ('vTaskEnterCritical','vTaskExitCritical'):self.ret(0);return
        if name=='a733_platform_lock_enter':self.lock_bounds.append(self.args()[2])
        if name=='vTaskDelay':
            # Serialized task model: one-tick delay wakes at the next tick.
            self.now=(self.now//1000+1)*1000
            if self.notified:
                # The notified service answers while the task blocks. The
                # first request's pending state is the fork point.
                if not self.saved:self.saved=True;self.checkpoint(a.output/'before-domain.pkl.gz')
                self.notified=False;self.answer()
        if pc==NOTIFY:
            # The row request stored its word with release order before this call.
            assert self.args()[0]==0 and not self.notified
            self.notified=True;self.ret(0);return
        # Named callback hooks take precedence over instruction inspection.
        # Replaced counter/translation/scheduler services own their MRS fields.
        if name is None and pc not in self.callback_map and pc!=STOP:
            word=int.from_bytes(u.mem_read(pc,4),'little')
            assert word&0xfff00000!=0xd5300000,('unexpected executed MRS',hex(pc),hex(word))
        super().hook(u,pc,size,unused)
    def request(self):
        q=self.get(self.row+self.rf['request'],4);return (q>>1)&GENERATION_MASK,q&1
    def answer(self):
        """The service's answer to the latest request, by mode."""
        generation,on=self.request();self.answers.append((self.mode,generation,on))
        if self.mode=='pending':return
        state={'on':ROW_ON,'refused':ROW_REFUSED,'failed':ROW_FAILED}[self.mode]
        if state==ROW_ON:assert on==1;self.regs[PCK]=(self.regs.get(PCK,0)&~15)|8;self.regs[PCK+8]=8
        self.put(self.row+self.rf['state'],generation<<3|state,4)
    def state(self):
        generation,_=self.request();s=self.get(self.row+self.rf['state'],4)
        return s&7 if s>>3==generation else ROW_PENDING
    def pools(self,nonidentity=False):
        # The pinned model calls device_memory_pool_init, which the image no
        # longer links; initialize each pool as its platform initializer does:
        # the boot pool DMA32 write-back, the uncached pool Normal
        # non-cacheable (platform/a733/memory_reservations.c, memory_uncached.c).
        assert not nonidentity
        for pool,lo,hi,kind,cache in [('boot_pool','__device_pool_start','__device_pool_end',1,0),
                                      ('uncached_pool','__uncached_pool_start','__uncached_pool_end',0,1)]:
            start=im.address(lo);end=im.address(hi)
            assert self.call('device_memory_pool_init_typed',im.address(pool),start,start,end-start,kind,cache)==1
        assert self.call('g2d_upload_allocate')==1
    def call(self,name,*args):
        self.phase='executing:'+name
        result=super().call(name,*args);self.phase='fresh-call-boundary';return result
    def configure(self):
        super().configure();self.regs[PCK]=0xa5000000;self.regs[PCK+8]=0
        # The row as a733_power_domain_row_init() leaves it at board startup:
        # domain 1, the session owner, G2D's DCDC2 token, this callback.
        owner=self.get(self.base+self.fields['session']+im.layouts['a733_g2d_session']['owner'])
        domains=im.address('providers')+im.layouts['board_provider']['domains']
        for field,value,n in (('domains',domains,8),('rail',self.base+self.fields['rail'],8),('next',0,8),
                ('notify',NOTIFY,8),('notify_context',0,8),('owner',owner,8),('domain',1,4),
                ('counted',0,4),('request',0,4),('state',0,4)):
            self.put(self.row+self.rf[field],value,n)
    def read_mmio(self,u,access,at,size,value,unused):
        if 0x07060000<=at<0x07062000:
            # PCK domain 1 belongs to the owner; G2D reads it only through its row.
            self.events.append(('domain-read',at,self.regs.get(at,0)));raise AssertionError(('G2D read a PCK word',hex(at)))
        super().read_mmio(u,access,at,size,value,unused)
    def write_mmio(self,u,access,at,size,value,unused):
        if 0x07060000<=at<0x07062000:
            self.events.append(('domain-write',at,value));raise AssertionError(('G2D wrote a PCK word',hex(at)))
        super().write_mmio(u,access,at,size,value,unused)
    def checkpoint(self,path):
        state={k:getattr(self,k) for k in ('now','regs','events','started','rcq_status','variant',
            'held','callback_map','mode','answers','lock_bounds','task_handle','saved','phase','notified')}
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
        self.phase='fresh-call-fork-from-row-request'

try:
    m=Power();m.pools();m.upload(raw,session);m.configure();m.regs.update(PERI0_GATES)
    assert m.call('a733_g2d_session_valid',m.base)==1
    preflight={'modeled':True,'physical_authority':False,'image':digest(a.image),'session':digest(a.session),
        'input':digest(a.input),'model_sources':PINS,'task_handle':m.task_handle,
        'initial_mmio':{hex(k):v for k,v in m.regs.items()},
        'pmic_read_service':{'chip':3,'revision':0,'page_before':0,'page_after':0,'rail':1,'enable':2,'selector':30,'programmed_uv':800000},
        'row':{'address':m.row,'domain':1,'notify':NOTIFY},'scope':__doc__}
    (a.output/'preflight.json').write_text(json.dumps(preflight,indent=2)+'\n')
    # ON: one request, answered ON at its notification; the copy completes.
    m.run();assert m.result()==1 and m.started==1 and m.expected==bytes(m.u.mem_read(m.get(m.base+m.fields['payload']),len(raw)))
    assert m.answers==[('on',1,1)] and m.state()==ROW_ON and m.get(m.base+m.fields['domain_requested'],4)==1
    assert not [e for e in m.events if e[0] in ('domain-read','domain-write')]
    m.checkpoint(a.output/'off-on-result.pkl.gz')
    reports=[dict(case='on-copy',result=m.result(),submissions=m.started,requests=len(m.answers),retained=True)]
    state=pickle.loads(gzip.decompress((a.output/'before-domain.pkl.gz').read_bytes()))
    assert state['image']==digest(a.image) and state['session']==digest(a.session)
    for case in ('refused','failed','pending'):
        m=Power();m.restore(state)
        assert m.regs==state['regs'] and m.events==state['events'] and not m.held and m.started==0
        assert m.result()==0 and m.state()==ROW_PENDING and m.request()==(1,1) and m.notified
        assert m.get(m.base+m.fields['domain_requested'],4)==1
        m.mode=case;m.saved=True;m.lock_bounds=[];m.notified=False;m.answer()
        before=len(m.events)
        for tick in range(40):
            m.tick()
            if m.result():break
            # A row never answered: the task keeps polling until its deadline.
            if case=='pending' and tick==20:m.now=1000001
        else:raise AssertionError(('power fork did not terminate',case))
        changed=m.events[before:]
        assert m.started==0 and m.result()==2 and m.request()==(1,1),(case,m.started,m.result(),m.request(),m.state(),tick)
        assert not [e for e in changed if e[0] in ('domain-read','domain-write','iommu-read','write','submit')],(case,changed[:8])
        if case=='pending':assert m.state()==ROW_PENDING and tick>20
        else:assert m.state()=={'refused':ROW_REFUSED,'failed':ROW_FAILED}[case] and tick==0
        count=len(m.events);m.tick();assert len(m.events)==count and m.result()==2
        m.checkpoint(a.output/(case+'.pkl.gz'))
        reports.append(dict(case=case,result=m.result(),submissions=m.started,requests=len(m.answers),ticks=tick+1,retained=True))
    verify_inputs()
    (a.output/'result.json').write_text(json.dumps({'passed':True,'cases':reports,'modeled':True,'physical_authority':False},indent=2)+'\n')
    print(json.dumps({'passed':True,'cases':len(reports),'physical_authority':False}))
except BaseException as error:
    if "m" in globals():m.checkpoint(a.output/"interrupted.pkl.gz")
    (a.output/"failure.json").write_text(json.dumps({"error":repr(error),"phase":getattr(globals().get("m"),"phase",None),"case":getattr(globals().get("m"),"mode",None)},indent=2)+"\n")
    raise
