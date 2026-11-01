#!/usr/bin/env python3
"""Execute returning-probe CPU ownership, delayed acknowledgments and ROM order.

Uses actual linked instructions. PPU/MMIO, counter progression, inherited
CPU_OFF state and cache effects are models; no hardware access or qualification.
"""
import argparse
import gc
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from harness import hook_addresses, Steps
from unicorn import UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('i2s_cpu_shared',ROOT/'checks/board-probe.py')
shared=importlib.util.module_from_spec(spec);spec.loader.exec_module(shared)
BASE=0x07052000

class Model(shared.Model):
    def __init__(self,elf,plan,cores=(1,),variant='normal',wake_enabled=False):
        super().__init__(elf,plan)
        # Fitted A733 A55/DSU geometry, including LoC=3. Omitting LoC would
        # silently skip the actual shared data-cache maintenance loop.
        self.sys['clidr_el1']=0x03000123
        self.ccsidr={0:0x000fe01a,2:0x003fe01a,4:0x007fe07a}
        self.step=240;self.variant=variant;self.trace=[];self.pending={};self.waits=0
        self.policy={c:0x100 if c in cores else 0 for c in range(1,8)}
        self.status=dict(self.policy);self.original=dict(self.policy)
        self.writes=[];self.release=False;self.early_policy_reads=0;self.cache_count=0
        self.data_cache_count=0;self.ic_count=0
        for c in range(1,8):
            self.write(BASE+c*4096,self.policy[c],4);self.write(BASE+c*4096+8,self.status[c],4)
        if wake_enabled:
            for c in range(1,8):self.write(0x07050200+c*4,0,4)
        self.before=bytes(self.uc.mem_read(0x07050000,0xc000))
        self.uc.hook_add(UC_HOOK_MEM_READ,self.ppu_read,begin=0x07050000,end=0x0705bfff)
        self.uc.hook_add(UC_HOOK_MEM_WRITE,self.ppu_write,begin=0x07050000,end=0x0705bfff)
    def ppu_write(self,uc,access,address,size,value,data):
        assert size==4 and address in {BASE+c*4096 for c in range(1,8)},hex(address)
        c=(address-BASE)//4096
        assert not self.pending,'next write before acknowledgment'
        assert value in (0,0x100)
        self.release|=value==0x100
        self.writes.append((address,value));self.trace.append(('policy',c,value))
        target=value
        self.pending[c]=(target,self.now+12000)
    def ppu_read(self,uc,access,address,size,value,data):
        c=(address-BASE)//4096;off=(address-BASE)%4096
        if c not in self.policy or off not in (0,8):return
        if c in self.pending:
            target,ready=self.pending[c]
            deny=self.variant=='denied' or (self.variant=='restore-denied' and target==0x100)
            if self.now>=ready and not deny:
                self.status[c]=target
                if self.variant!='bad-readback':self.policy[c]=target
                del self.pending[c];self.trace.append(('ack',c,target))
                if self.variant=='late':
                    state=self.address('cpu');layout=self.e.layouts['i2s_probe_cpu_state']
                    self.now=self.read(state+layout['deadline'])
            elif off==0:
                self.early_policy_reads+=1
            else:
                self.waits+=1
                if self.variant=='wake-before-ack':self.status[c]=0x108
                if self.variant=='changed-request':self.write(0x07050204,3,4)
                if self.variant=='backwards':self.now=0
        v=self.policy[c] if off==0 else self.status[c]
        self.write(address,v,4)
    def _system(self,uc,address,size,data):
        word=int.from_bytes(uc.mem_read(address,4),'little')
        name,_,_=shared.machine.decode_system(word)
        cache=(word==0xd508751f or isinstance(name,str) and name.startswith('dc ') or
               isinstance(name,tuple) and name[0]==1 and name[2]==7 and name[3] in (6,10,14))
        if cache:
            assert not self.release,'cache maintenance after dynamic release'
            assert not any(self.policy.values()) and not any(self.status.values())
            self.cache_count+=1
            if word==0xd508751f:self.ic_count+=1
            else:self.data_cache_count+=1
        if name=='rmr_el3':self.trace.append(('rom',0,0))
        if name=='sctlr_el1' and self.release:raise AssertionError('EL1 access after release')
        return super()._system(uc,address,size,data)
    def entry(self):
        reached=[]
        hook_addresses(self.uc,[self.address('invalidate_data_cache')],lambda uc,*_: (reached.append(True),uc.emu_stop()))
        self.call('monitor_entry',count=12000000)
        return reached
    def finish(self):
        payload=b'CPU ownership fixture';self.uc.mem_write(0x42000100,payload)
        self.write(shared.probe.SESSION+128,len(payload));self.write(shared.probe.SESSION+136,0)
        self.write(shared.probe.SESSION+144,0x5944414552505353)
        self.call('sunstone_probe_return',0,count=2000000)
    def unchanged_pages(self):
        after=bytes(self.uc.mem_read(0x07050000,0xc000))
        assert after==self.before,'PPU/cluster pages differ after exact restoration'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--elf',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--resume',action='store_true')
    a=p.parse_args();a.out.mkdir(parents=True,exist_ok=a.resume);steps=Steps(a.out,'commands.json');rows=[]
    digest=hashlib.sha256(a.elf.read_bytes()).hexdigest();identity=a.out/'image.sha256'
    if identity.exists():assert identity.read_text().strip()==digest,'Changed image needs new evidence directory'
    else:identity.write_text(digest+'\n')
    input_path=a.out/'input.bin';input_path.write_bytes(struct.pack('<4I',0,24,1,0))
    attempt=1
    while (a.out/f'session-{attempt}').exists():attempt+=1
    plan=shared.probe.prepare(a.elf,shared.probe.inspect(a.elf),[input_path],a.out/f'session-{attempt}')
    def passed(name,m):
        record={'passed':True,'writes':m.writes,'wait_reads':m.waits,'cache_operations':m.cache_count,
                'data_cache_operations':m.data_cache_count,'instruction_cache_operations':m.ic_count,
                'counter_ticks':m.now,'fault':m.rtc(),'trace':m.trace}
        steps.case(name,record);rows.append(name);print('PASS',name,flush=True)
    for label,cores,variant in [('static',(),'normal'),('CPU1-only',(1,),'normal'),
                                ('all-seven',tuple(range(1,8)),'normal'),('wake-completes',(1,),'wake-before-ack'),
                                ('wake-enabled-static',(),'normal'),
                                ('wake-enabled-all-seven',tuple(range(1,8)),'normal'),
                                ('wake-enabled-inflight',(1,),'wake-before-ack')]:
        if steps.case(label):rows.append(label);continue
        m=Model(a.elf,plan,cores,variant,wake_enabled=label.startswith('wake-enabled'));assert m.entry() and not m.rtc()['fault']
        assert not any(m.policy.values()) and not any(m.status.values())
        assert m.writes==[(BASE+c*4096,0) for c in cores]
        m.finish();assert m.events[-2:]==['disarm','rom'],m.events
        receipt=shared.probe.receipt(bytes(m.uc.mem_read(shared.probe.RECEIPT,128)),plan)
        assert receipt['status']==0
        assert m.writes==[(BASE+c*4096,0) for c in cores]+[(BASE+c*4096,0x100) for c in cores]
        assert m.policy==m.original and m.status==m.original and not m.early_policy_reads
        m.unchanged_pages();assert m.data_cache_count==18944 and m.ic_count==1
        if cores:assert m.waits>1
        passed(label,m);del m;gc.collect()
    for variant,site in [('denied',0x8f),('bad-readback',0x89),('late',0x8f),
                         ('changed-request',0x87),('backwards',0x8e),('stopped',0x90)]:
        if steps.case(variant):rows.append(variant);continue
        m=Model(a.elf,plan,variant='denied' if variant=='stopped' else variant)
        if variant=='stopped':m.step=0
        assert not m.entry();fault=m.rtc()
        assert fault['fault']==0xfa030000|site,(variant,fault)
        assert fault['far'] in (0x07053000,0x07053008,0x07050204,0x07050224),(variant,fault)
        if variant=='stopped':assert fault['esr']==65537
        assert not m.disarm_seen and not m.cache_count and 'rom' not in m.events
        passed(variant,m);del m;gc.collect()
    for label,address,value,site in [('bad-last-identity',0x07059fc8,0,0x80),
                                     ('mismatched-ack',0x07053008,0,0x83),
                                     ('software-start-request',0x07050204,1,0x84)]:
        if steps.case(label):rows.append(label);continue
        m=Model(a.elf,plan);m.write(address,value,4)
        if address==0x07053008:m.status[1]=value
        assert not m.entry()
        assert not m.writes and m.rtc()['fault']==0xfa030000|site
        assert m.rtc()['far']==address and m.rtc()['esr']==value
        passed(label,m);del m;gc.collect()
    name='restore-denial-preserves-first-fault-and-armed-watchdog'
    if not steps.case(name):
        m=Model(a.elf,plan,variant='restore-denied');assert m.entry();m.finish();fault=m.rtc()
        assert fault['fault']==0xfa03008f and fault['far'] in (0x07053008,0x07050204,0x07050224)
        assert m.data_cache_count==18944 and m.ic_count==1
        assert not m.disarm_seen and 'rom' not in m.events and not m.read(shared.probe.RECEIPT+120)
        m.call('sunstone_probe_fault',0x99,0x1234,0x5555,0x6666,count=10000)
        assert m.rtc()['fault']==fault['fault'] and m.rtc()['far']==fault['far']
        passed(name,m)
    else:rows.append(name)
    (a.out/'result.json').write_text(json.dumps({'passed':True,'hardware_access':False,'cases':rows,
        'image_sha256':hashlib.sha256(a.elf.read_bytes()).hexdigest(),
        'modeled_boundaries':['PPU latency and policy readback','counter','CPU_OFF predecessor','cache effects']},indent=2)+'\n')
if __name__=='__main__':main()
