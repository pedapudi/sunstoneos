#!/usr/bin/env python3
"""Execute timer/peer interrupt delivery and quiet shutdown with modeled GIC."""
import argparse,ctypes,importlib.util,json,pickle,struct
from pathlib import Path
from unicorn import UC_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_PC
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('coherence_check',ROOT/'checks/secondary-coherence-probe.py')
coherent=importlib.util.module_from_spec(spec);spec.loader.exec_module(coherent)
cached=coherent.cached;local=cached.local;digest=cached.digest;SHARED=coherent.SHARED

class Interrupts(coherent.Coherent):
    def execute(self):
        super().execute()
        # The parent stops at primary refusal. A deadline can expire there
        # before the secondary observes it; complete that observation without
        # restarting the machine or allowing shared cleanup.
        if self.fault=='pending-at-stop' and self.retained:
            self.primary_refusal_observation={'parked':sorted(self.target_parked),
                'pending_core6':sorted(self.pending[6]),'clock':self.clock,'deadline':self.deadline}
            for _ in range(20):
                if 6 in self.target_parked:break
                u=self.cpus[6]
                u.emu_start(u.reg_read(UC_ARM64_REG_PC),0,count=500)
                self.clock+=1000
            assert 6 in self.target_parked, 'Secondary did not observe the expired shutdown deadline'

    def restore(self,snapshot,fault):
        super().restore(snapshot,fault)
        self.pending=[set() for _ in range(8)];self.irq_active=[None]*8
        self.delivered={c:[] for c in range(1,8)};self.sent=set();self.stopped_delivery=set();self.fault_injected=False
        self.primary_refusal_observation=None
        self.priority_before={c:0xf0f0f0f0 for c in range(1,8)}
        for c,value in self.priority_before.items():self.mmio[0x03460000+c*0x20000+0x1041c]=value
        self.secure_pmr={c:0 for c in range(1,8)}

    def memory(self,u,access,address,size,value,core):
        if core and self.regs[core]['currentel']==4 and address<0x41000000:
            base=0x03460000+core*0x20000
            assert address in (base,base+0x10100,base+0x10200,base+0x10300,base+0x1041c)
            if access==UC_MEM_WRITE:
                if address==base+0x1041c:
                    assert value in (self.priority_before[core],(self.priority_before[core]&~0xff0000)|0xa00000)
                else:assert address==base+0x10100 and value==1<<30
            elif address==base+0x10200:self.mmio[address]=sum(1<<n for n in self.pending[core])
            elif address==base+0x10300:self.mmio[address]=0 if self.irq_active[core] is None else 1<<self.irq_active[core]
            return local.Local.memory(self,u,access,address,size,value,core)
        return super().memory(u,access,address,size,value,core)

    def instruction(self,u,address,size,core):
        m=self.regs[core]
        if core and m['currentel']==4 and m['cntp_ctl_el0']&3==1 and self.clock>=m['cntp_cval_el0'] and self.irq_active[core]!=30:
            assert self.mmio[0x03460000+core*0x20000+0x10100]&(1<<30)
            self.pending[core].add(30)
        if address not in self.decoded:self.decoded[address]=next(self.decoder.disasm(bytes(u.mem_read(address,4)),address))
        ins=self.decoded[address];op,args=ins.mnemonic,ins.op_str.split(', ')
        if core and op=='mrs' and args[1]=='icc_pmr_el1':
            mask=self.secure_pmr[core]
            value=mask if m['currentel']==12 else (mask<<1)&255 if mask&128 else 0
            self.put(core,args[0],value);u.reg_write(UC_ARM64_REG_PC,address+4);return
        if core and op=='msr' and args[0]=='icc_pmr_el1':
            value=self.reg(core,args[1])
            if m['currentel']==12:self.secure_pmr[core]=value&0xf8
            else:
                assert m['currentel']==4 and m['scr_el3']&4
                if self.secure_pmr[core]&128:self.secure_pmr[core]=((value>>1)|128)&0xf8
            m['icc_pmr_el1']=(self.secure_pmr[core]<<1)&255 if self.secure_pmr[core]&128 else 0
            u.reg_write(UC_ARM64_REG_PC,address+4);return
        if core and address==self.survey_hooks['identity_interrupt_groups_disabled']:
            assert m['daif']&0x80 and not m['cntp_ctl_el0']&3 and not m['icc_igrpen1_el1']
            assert self.delivered[core] in ([0,30],[30,0]) and self.irq_active[core] is None
            self.stopped_delivery.add(core)
            if self.fault=='pending-at-stop' and core==6:
                assert not self.fault_injected
                self.pending[core].add(0);self.fault_injected=True;self.clock=self.deadline-1000
        if core and op=='mrs' and args[1]=='icc_iar1_el1':
            assert m['currentel']==4 and m['daif']&0x80
            value=min(self.pending[core]) if self.pending[core] else 1023
            if value<1020:
                assert self.irq_active[core] is None
                self.pending[core].remove(value);self.irq_active[core]=value;self.delivered[core].append(value)
            self.put(core,args[0],value);u.reg_write(UC_ARM64_REG_PC,address+4);return
        if core and op=='msr' and args[0] in ('icc_ctlr_el1','icc_bpr1_el1','icc_pmr_el1','icc_igrpen1_el1','icc_sgi1r_el1','icc_eoir1_el1'):
            name,src=args;value=self.reg(core,src)
            assert m['currentel']==4
            if name=='icc_igrpen1_el1':
                assert value in (0,1)
                if value==1:
                    assert self.mmio[0x03460000+core*0x20000+0x10100]==0x40000001
                    assert (self.mmio[0x03460000+core*0x20000+0x1041c]>>16)&255==0xa0
                    assert m['vbar_el1']==self.survey_hooks['identity_interrupt_vectors']
                else:assert all(self.q(SHARED+0x100+c*128+80)==self.epoch for c in range(1,8))
                m['icc_igrpen1_el3']=(m['icc_igrpen1_el3']&~1)|value
            elif name=='icc_sgi1r_el1':
                target=core+1 if core<7 else 1
                assert value==(target<<16)|1 and core not in self.sent
                assert all(self.q(SHARED+0x100+c*128+64)==self.epoch for c in range(1,8))
                self.sent.add(core)
                if self.fault=='wrong-irq' and target==6:self.pending[target].add(31);self.fault_injected=True
                else:self.pending[target].add(0)
            elif name=='icc_eoir1_el1':
                assert self.irq_active[core]==value and not m['icc_ctlr_el1']&2
                if value==30:assert not m['cntp_ctl_el0']&3
                self.irq_active[core]=None
            m[name]=value;u.reg_write(UC_ARM64_REG_PC,address+4);return
        if core and op=='dc' and args[0]=='civac' and not self.production_table_clean(address):
            assert self.mmio[0x03460000+core*0x20000+0x1041c]==self.priority_before[core]
            assert self.stopped_delivery==set(range(1,8)) and not any(self.pending) and not any(x is not None for x in self.irq_active)
            assert all(not self.regs[c]['icc_igrpen1_el1'] and not self.regs[c]['cntp_ctl_el0']&3 for c in range(1,8))
        if core and op=='wfi' and address==self.survey_hooks['identity_target_terminal']+4:
            assert self.secure_pmr[core]==0, 'Secure PMR not restored before OFF'
        return super().instruction(u,address,size,core)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for n in ('image','bundle','prefix','output'):parser.add_argument('--'+n,type=Path,required=True)
    parser.add_argument('--case',choices=('success','wrong-irq','pending-at-stop'),action='append')
    a=parser.parse_args();machine=Interrupts(a.image,a.bundle)
    machine.local_manifest=json.loads((a.bundle/'manifest.json').read_text());machine.overlay=(a.bundle/'overlay.bin').read_bytes()
    meta=json.loads(a.prefix.with_suffix('.json').read_text());assert digest(a.prefix)==meta['snapshot_sha256']
    for path,sha in meta['inputs'].items():assert digest(Path(path))==sha
    prior=json.loads(next(Path(n).read_text() for n in meta['inputs'] if n.endswith('/manifest.json')))
    assert prior['primary_manifest']==machine.local_manifest['primary_manifest']
    base=Path(prior['primary_manifest']['path']);bm=json.loads(base.read_text())
    checkpoint=bm['hooks']['primary_publication_return']-0x41020000
    assert machine.overlay[:checkpoint]==(base.parent/'overlay.bin').read_bytes()[:checkpoint]
    snapshot=pickle.loads(a.prefix.read_bytes()) # Trusted local snapshot, digest checked.
    for name in a.case or ('success','wrong-irq','pending-at-stop'):
        machine.restore(snapshot,None if name=='success' else name);machine.execute()
        raw=machine.read(0x42018000,0x4200);(a.output.parent/('report-'+name+'.bin')).write_bytes(raw)
        if name=='success':
            assert machine.returned and not machine.retained and machine.started==machine.stopped==list(range(1,8))
            assert machine.sent==machine.clean_complete==machine.stopped_delivery==set(range(1,8))
            assert all(sorted(v)==[0,30] for v in machine.delivered.values())
            assert raw[0x3200:]==machine.read(SHARED,4096)
            assert struct.unpack_from('<3Q',raw,256)==(0x4137334952515331,1,0x4100)
            for c in range(1,8):
                assert struct.unpack_from('<8Q',raw,0x3200+0x100+c*128+48)==(1,1,machine.epoch,machine.epoch,machine.epoch,0,0,0)
        else:
            assert machine.retained and not machine.returned and 6 in machine.target_parked and machine.fault_injected
            assert machine.started==list(range(1,8)) and machine.stopped==[] and not machine.clean_complete
            assert machine.q(local.DISTRIBUTOR)==2
            if name=='wrong-irq':assert 31 in machine.delivered[6]
            else:assert 6 in machine.stopped_delivery and machine.pending[6]=={0}
        record={'case':name,'pass':True,'native_sha256':digest(a.image),'manifest_sha256':digest(a.bundle/'manifest.json'),
            'check_sha256':digest(Path(__file__)),'started':machine.started,'off':machine.stopped,
            'sent':sorted(machine.sent),'delivered':machine.delivered,'stopped_delivery':sorted(machine.stopped_delivery),
            'clean_complete':sorted(machine.clean_complete),'returned':machine.returned,'retained':machine.retained,
            'primary_refusal_observation':machine.primary_refusal_observation,
            'report_sha256':digest(a.output.parent/('report-'+name+'.bin'))}
        (a.output.parent/('case-'+name+'.json')).write_text(json.dumps(record,indent=2)+'\n');print('PASS',name,flush=True)
    results=[]
    for name in ('success','wrong-irq','pending-at-stop'):
        record=json.loads((a.output.parent/('case-'+name+'.json')).read_text())
        assert record['pass'] and record['native_sha256']==digest(a.image) and record['manifest_sha256']==digest(a.bundle/'manifest.json')
        assert record['report_sha256']==digest(a.output.parent/('report-'+name+'.bin'));results.append(record)
    sources=('checks/secondary-interrupt-probe.py','checks/secondary-coherence-probe.py','checks/secondary-cache-probe.py','checks/secondary-local-probe.py',
        'checks/secondary-identity-probe.py','checks/primary-publication-probe.py','checks/a733-supervised-publication.py','checks/a733-smp-cached.py')
    a.output.write_text(json.dumps({'native_sha256':digest(a.image),'manifest_sha256':digest(a.bundle/'manifest.json'),
        'physical_access':False,'primary_prefix_reused':str(a.prefix),'primary_prefix_sha256':meta['snapshot_sha256'],
        'sources':{n:digest(ROOT/n) for n in sources},'cases':results,
        'limits':'Actual instructions; modeled GIC/timer, shared RAM, exclusive monitors and power. Physical delivery/cache visibility, full interrupt context saving and FreeRTOS scheduling remain unqualified.'},indent=2)+'\n')
if __name__=='__main__':main()
