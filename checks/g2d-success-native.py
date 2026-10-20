#!/usr/bin/env python3
"""Check normal G2D acknowledgment/reset/copy ordering on one submitted job.

Actual linked completion, IOMMU scan and copy instructions run. Root-published
contract state, prepared pools, services, RTOS identity and device effects remain
modeled. Existing blend pixels are reused as data, not requalified numerically.
"""
from pathlib import Path
import hashlib,json,gzip,pickle,struct
CHECK_SCOPE=__doc__
ROOT=Path(__file__).resolve().parents[1]
blend_checker=ROOT/'checks/g2d-blend-native.py'
definitions=blend_checker.read_text().split('\ntry:\n    host=load')[0]
assert definitions!=blend_checker.read_text()
exec(compile(definitions,str(blend_checker),'exec'),globals())
BASE=0x05440000
baseline=Path('/home/sunil/.cache/sunstoneos/evidence/g2d-blend-20261020')
baseline_paths=[blend_checker,baseline/'build/sunstone-a733-g2d.elf',baseline/'entry/session.bin',baseline/'entry/input.argb',baseline/'native-completed/copied.pkl.gz',baseline/'native-completed/modeled-output.argb']
for dependency in baseline_paths:
    path=dependency.resolve();data=path.read_bytes();index=len(loaded);loaded[path]=data
    identity[str(path)]={'sha256':hashlib.sha256(data).hexdigest(),'bytes':len(data)}
    (inputs/(str(index)+'-'+path.name)).write_bytes(data)
(a.output/'inputs.json').write_text(json.dumps(identity,indent=2)+'\n');verify_inputs()
assert digest(baseline_paths[1])=='b27476caef23ad7cd378552efcc5a919420c8e801f6f658466290d6b1bbfe710'
oldim=helpers.Image(baseline_paths[1]);old=pickle.loads(gzip.decompress(baseline_paths[4].read_bytes()))
assert old['image']==digest(baseline_paths[1]) and old['session']==digest(baseline_paths[2])
assert raw==baseline_paths[3].read_bytes() and session[856:]==baseline_paths[2].read_bytes()[856:]
def old_read(address,n):
    for base,data in old['memory']:
        if base<=address and address-base+n<=len(data):return data[address-base:address-base+n]
    raise AssertionError(('outside saved baseline',hex(address),n))
old_base=oldim.address('g2d');old_fields=oldim.layouts['g2d_application']
old_commands=old_read(int.from_bytes(old_read(old_base+old_fields['commands'],8),'little'),4096)
pixels=baseline_paths[5].read_bytes();assert len(pixels)==len(raw) and pixels==old['expected']

class Finish(Blend):
    def __init__(self):
        super().__init__();self.case='normal';self.injections=0;self.reset_writes=[];self.ack_writes=[];self.cpu_output_reads=0
        self.u.hook_add(UC_HOOK_MEM_READ,self.output_read,begin=0x78000000,end=0x78ffffff)
    def ready(self):return self.get(self.base+self.fields['finish_ready'],4)
    def output_read(self,u,access,address,size,value,unused):
        output=self.get(self.base+self.fields['output']);n=self.get(self.base+self.fields['bytes'])
        if output and output<=address<output+n:
            assert self.ready() and self.get(self.base+self.fields['complete'],4)
            self.cpu_output_reads+=1
    def inject(self):assert self.injections==0;self.injections=1
    def configure(self):
        super().configure();self.regs[PCK]=0xa5000008;self.regs[PCK+8]=8
        self.regs[BASE+8]=0xa5a50003
    def command_effect(self):
        assert not self.ready() and not self.get(self.base+self.fields['complete'],4)
        commands=bytes(self.u.mem_read(self.mapped(0x10800000,4096,1),4096));assert commands==old_commands
        self.u.mem_write(self.mapped(0x10400000,len(pixels),2),pixels);self.expected=pixels
        self.events.append(('reused-modeled-output',len(pixels)))
    def read_mmio(self,u,access,address,size,value,unused):
        if address==BASE+8:self.put(address,self.regs[address],size);return
        if address==BASE+0x24 and self.started:
            if self.case=='unexpected-reset' and not self.injections:
                self.inject();self.regs[BASE+8]&=~1
            if self.case=='late-taskend' and self.ready() and not self.injections:
                self.inject();self.rcq_status|=1
        super().read_mmio(u,access,address,size,value,unused)
    def write_mmio(self,u,access,address,size,value,unused):
        if address==BASE+0x24 and self.started:
            assert value==1 and not self.ready() and not self.get(self.base+self.fields['complete'],4)
            self.ack_writes.append(value)
            if self.case=='ack-stuck':
                self.inject();self.events.append(('ack-refused',value));return
        if address==BASE+8 and self.started:
            assert not self.ready() and not self.get(self.base+self.fields['complete'],4)
            assert self.ack_writes==[1] and not self.rcq_status&1
            if not self.reset_writes:assert self.rcq_status&4 # W1C preserved CFG.
            self.reset_writes.append(value);self.regs[address]=value
            if not value&1:self.rcq_status=0 # Explicit reset effect; counter is not a fence.
            if self.case==('low-uncertain' if not value&1 else 'high-uncertain'):
                self.inject();self.now=self.get(im.address('board')+im.layouts['a733_board_session']['deadline_us'])
        super().write_mmio(u,access,address,size,value,unused)
    def checkpoint(self,path):
        state=super().checkpoint(path)
        for name in ('case','injections','reset_writes','ack_writes','cpu_output_reads'):state[name]=getattr(self,name)
        path.write_bytes(gzip.compress(pickle.dumps(state),compresslevel=1));return state

def bootstrap(m):
    m.u.mem_write(SCRATCH,session);assert m.call('a733_g2d_session_valid',SCRATCH)==1
    contract=loaded[a.contract.resolve()];assert len(contract)==288
    cf=im.layouts['a733_smp_contract'];m.now=struct.unpack_from('<Q',contract,cf['armed_at'])[0]//24+100
    m.u.mem_write(0x41f00000,session);record=im.address('g2d_admission')
    m.u.mem_write(record,contract);m.u.mem_write(0x42004000,contract)
    assert m.call('a733_smp_contract_valid',record,m.now*24)==1 and m.call('g2d_session_prepare',record)==1
    retained=m.call('g2d_admitted_session');assert bytes(m.u.mem_read(retained,904))==session
    m.put(im.address('board')+im.layouts['a733_board_session']['deadline_us'],struct.unpack_from('<Q',contract,cf['diagnostic_deadline'])[0]//24,8)
    m.pools();m.upload(raw,session);m.configure()

try:
    assert not a.resume and not a.completed
    m=Finish();bootstrap(m)
    for _ in range(1000):
        m.tick()
        if m.get(m.base+m.fields['stage'],4)==2:break
        assert m.result()==0
    else:raise AssertionError('submission boundary not reached')
    assert m.started==1 and not m.ready() and not m.get(m.base+m.fields['complete'],4) and m.cpu_output_reads==0
    m.checkpoint(a.output/'submitted.pkl.gz');state=pickle.loads(gzip.decompress((a.output/'submitted.pkl.gz').read_bytes()));reports=[]
    for case in ('normal','ack-stuck','unexpected-reset','low-uncertain','high-uncertain','late-taskend'):
        m=Finish();m.restore(state)
        assert m.regs==state['regs'] and m.events==state['events'] and m.started==1 and not m.ready() and m.cpu_output_reads==0
        m.case=case;before=len(m.events);m.run()
        assert not [e for e in m.events[before:] if e[0]=='submit']
        assert m.ack_writes==[1] and all(row['state']==2 for row in m.rows()['uncached_pool'])
        copied=m.get(m.base+m.fields['copied']);complete=m.get(m.base+m.fields['complete'],4)
        assert m.get(m.base+m.fields['rcq_status'],4)&1 # Pre-ack observation retained.
        if case=='normal':
            assert m.result()==1 and m.ready()==complete==1 and copied==len(pixels) and m.cpu_output_reads>0
            assert bytes(m.u.mem_read(m.get(m.base+m.fields['payload']),len(pixels)))==pixels
            assert m.injections==0 and m.reset_writes==[0xa5a50002,0xa5a50003] and m.rcq_status==0
        else:
            assert m.result()==2 and m.injections==1 and copied==0 and m.cpu_output_reads==0
            assert m.ready()==complete==(1 if case=='late-taskend' else 0)
            if case in ('ack-stuck','unexpected-reset'):assert not m.reset_writes
            elif case=='low-uncertain':assert m.reset_writes==[0xa5a50002]
            else:assert m.reset_writes==[0xa5a50002,0xa5a50003]
        m.checkpoint(a.output/(case+'.pkl.gz'))
        reports.append(dict(case=case,result=m.result(),injections=m.injections,source_submissions=1,new_submissions=0,finish_ready=m.ready(),complete=complete,copied=copied,cpu_output_reads=m.cpu_output_reads,ack_writes=m.ack_writes,reset_writes=m.reset_writes,retained=True))
        (a.output/'progress.json').write_text(json.dumps(reports,indent=2)+'\n')
    verify_inputs();(a.output/'result.json').write_text(json.dumps(dict(passed=True,cases=reports,source_submissions=1,image_sha256=digest(a.image),session_sha256=digest(a.session),contract_sha256=digest(a.contract),scope=CHECK_SCOPE,physical_access=False),indent=2)+'\n')
except BaseException as error:
    if 'm' in globals():m.checkpoint(a.output/'interrupted.pkl.gz')
    (a.output/'failure.json').write_text(json.dumps(dict(error=repr(error)),indent=2)+'\n');raise
