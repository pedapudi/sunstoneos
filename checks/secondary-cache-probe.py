#!/usr/bin/env python3
"""Check per-core cached EL1 entry and return without modeling physical caches."""
import argparse,ctypes,importlib.util,json,pickle,struct
from pathlib import Path
from unicorn import UC_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_PC
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('local_check',ROOT/'checks/secondary-local-probe.py')
local=importlib.util.module_from_spec(spec);spec.loader.exec_module(local)
model=local.model;digest=local.survey.primary.probe.digest
EL3,EL1=0x4201a000,0x4201a800

class Cached(local.Local):
    def restore(self,snapshot,fault):
        super().restore(snapshot,fault)
        for p in self.local_manifest['patches']:
            if p['space']=='native':
                ctypes.memmove(self.ram_pointer+p['address']-0x41000000,struct.pack('<I',p['replacement']),4)
        ctypes.memset(self.ram_pointer+local.survey.REPORT-0x41000000,0,0x2f00)
        self.el3_enabled=set();self.el1_enabled=set();self.el3_seen=set();self.el1_seen=set();self.smc_seen=set()
        for c in range(1,8):
            self.regs[c].update(sctlr_el3=0x30c50838 if c>=6 else 0xc50838,
                tcr_el3=0x12345678+c,ttbr0_el3=0x1357000+c*0x1000,mair_el3=0x2311+c,
                ttbr0_el1=0xace000+c*0x1000,ttbr1_el1=0x135000+c*0x1000,mair_el1=0x1234+c)
        self.original_regs=local.copy.deepcopy(self.regs)

    def memory(self,u,access,address,size,value,core):
        if core and self.regs[core]['currentel']==4:
            assert access!=UC_MEM_WRITE,'EL1 must not dirty Normal memory'
            assert 0x41000000<=address<0x41200000 or self.sealed_el1_table_read(address,size),('EL1 only reads immutable code/literals and sealed tables',core,hex(address),hex(u.reg_read(UC_ARM64_REG_PC)))
        return super().memory(u,access,address,size,value,core)

    def instruction(self,u,address,size,core):
        if address not in self.decoded:
            self.decoded[address]=next(self.decoder.disasm(bytes(u.mem_read(address,4)),address))
        ins=self.decoded[address];op,args=ins.mnemonic,ins.op_str.split(', ');m=self.regs[core]
        if core and op=='wfi' and m['currentel']>0 and address!=self.survey_hooks['identity_target_terminal']+4:
            self.target_parked.add(core);self.clock=self.deadline+1;u.emu_stop();return
        if core and address==self.survey_hooks['el1_observe_checkpoint']:
            assert m['currentel']==4 and core in self.el1_seen
        if core and op=='smc':
            assert m['currentel']==4 and args==['#0x733']
            self.smc_seen.add(core)
            # AArch64 SMC syndrome includes IL and the instruction immediate.
            self.exception(core,3,address+4,(0x17<<26)|(1<<25)|0x733);return
        if core and op=='mrs' and args[1]=='tpidr_el1' and self.fault=='wrong-el1-core' and core==6:
            self.put(core,args[0],7);u.reg_write(UC_ARM64_REG_PC,address+4);return
        if core and op=='msr':
            name,src=args;value=self.reg(core,src)
            if name in ('icc_ap0r0_el1','icc_ap1r0_el1'):
                self.active_priority(core,name,value);u.reg_write(UC_ARM64_REG_PC,address+4);return
            if name in ('sctlr_el3','sctlr_el1','tcr_el3') or name.startswith('icc_'):
                if name=='sctlr_el3':
                    if value&0x1005==0x1005:
                        assert m['currentel']==12 and not m['scr_el3']&0x200
                        assert m['ttbr0_el3']==self.symbols['a733_cached_el3_l1']
                        assert value==self.original_regs[core]['sctlr_el3']|0x1005
                        self.mappings();self.el3_seen.add(core);self.el3_enabled.add(core)
                    else:
                        assert value==self.original_regs[core]['sctlr_el3']
                        self.el3_enabled.discard(core)
                elif name=='sctlr_el1':
                    if value&0x1005==0x1005:
                        assert core in self.el3_enabled and m['ttbr0_el1']==self.symbols['a733_cached_el1_l1']
                        # The overlay zeroed every active-priority copy before EL1 entry.
                        assert self.active_priorities[core]=={'ap0r0','ap1r0_s','ap1r0_ns'},(core,self.active_priorities[core])
                        self.el1_seen.add(core);self.el1_enabled.add(core)
                    else:self.el1_enabled.discard(core)
                elif name.startswith('icc_'):
                    entry=self.symbols['a733_cached_enter_el1']
                    assert name in ('icc_sre_el3','icc_ctlr_el3','icc_igrpen0_el1','icc_igrpen1_el3','icc_sre_el2','icc_sre_el1','icc_pmr_el1')
                    if name in ('icc_igrpen0_el1','icc_igrpen1_el3'):assert value==0
                    # The native entry sets the Secure priority mask at EL3 before Nonsecure entry.
                    if name=='icc_pmr_el1':assert value==255 and m['currentel']==12 and entry<=address<entry+self.sizes['a733_cached_enter_el1']
                m[name]=value;u.reg_write(UC_ARM64_REG_PC,address+4);return
            if name=='ttbr0_el3' and self.fault=='restore-el3' and core==6 and value==self.original_regs[core][name]:
                m[name]=value^4096;u.reg_write(UC_ARM64_REG_PC,address+4);return
        if core and op=='wfi' and address==self.survey_hooks['identity_target_terminal']+4:
            assert core in self.smc_seen and core not in self.el3_enabled and core not in self.el1_enabled
            assert self.q(EL3+core*256)==self.q(EL1+core*256)==3
            for name in ('sctlr_el3','tcr_el3','ttbr0_el3','mair_el3','scr_el3','sctlr_el1','tcr_el1','ttbr0_el1','ttbr1_el1','mair_el1'):
                assert m[name]==self.original_regs[core][name],(core,name)
        return super().instruction(u,address,size,core)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('image','bundle','prefix','output'):p.add_argument('--'+n,type=Path,required=True)
    p.add_argument('--case',choices=('success','wrong-el1-core','restore-el3'),action='append')
    a=p.parse_args();machine=Cached(a.image,a.bundle)
    machine.local_manifest=json.loads((a.bundle/'manifest.json').read_text());machine.overlay=(a.bundle/'overlay.bin').read_bytes()
    meta=json.loads(a.prefix.with_suffix('.json').read_text());assert digest(a.prefix)==meta['snapshot_sha256']
    for path,sha in meta['inputs'].items():assert digest(Path(path))==sha
    prior=json.loads(next(Path(n).read_text() for n in meta['inputs'] if n.endswith('/manifest.json')))
    assert prior['primary_manifest']==machine.local_manifest['primary_manifest']
    base=Path(prior['primary_manifest']['path']);bm=json.loads(base.read_text())
    checkpoint=bm['hooks']['primary_publication_return']-0x41020000
    assert machine.overlay[:checkpoint]==(base.parent/'overlay.bin').read_bytes()[:checkpoint]
    snapshot=pickle.loads(a.prefix.read_bytes()) # Trusted local snapshot, digest checked.
    results=[]
    for name in a.case or ('success','wrong-el1-core','restore-el3'):
        machine.restore(snapshot,None if name=='success' else name);machine.execute()
        raw=machine.read(0x42018000,0x3000)
        (a.output.parent/('report-'+name+'.bin')).write_bytes(raw)
        if name=='success':
            assert machine.returned and not machine.retained and machine.started==machine.stopped==list(range(1,8))
            assert machine.el3_seen==machine.el1_seen==machine.smc_seen==set(range(1,8))
            assert struct.unpack_from('<3Q',raw,256)==(0x4137334341434831,1,0x2f00)
            for c in range(1,8):assert machine.q(EL3+c*256)==machine.q(EL1+c*256)==3
        else:
            assert machine.retained and not machine.returned and machine.target_parked=={6}
            assert machine.stopped==list(range(1,6)) and machine.started==list(range(1,7))
            assert machine.q(local.DISTRIBUTOR)==2
            if name=='wrong-el1-core':assert machine.q(EL1+6*256)==1 and machine.q(EL1+6*256+232)==1
            else:assert machine.q(EL3+6*256)==2 and machine.q(EL3+6*256+152)==1
        record={'case':name,'pass':True,'started':machine.started,'off':machine.stopped,
                'EL3':sorted(machine.el3_seen),'EL1':sorted(machine.el1_seen),'SMC':sorted(machine.smc_seen),
                'returned':machine.returned,'retained':machine.retained,'report_sha256':digest(a.output.parent/('report-'+name+'.bin'))}
        (a.output.parent/('case-'+name+'.json')).write_text(json.dumps(record,indent=2)+'\n');results.append(record);print('PASS',name,flush=True)
    a.output.write_text(json.dumps({'native_sha256':digest(a.image),'manifest_sha256':digest(a.bundle/'manifest.json'),
        'physical_access':False,'primary_prefix_reused':str(a.prefix),'primary_prefix_sha256':meta['snapshot_sha256'],
        'sources':{n:digest(ROOT/n) for n in ('checks/secondary-cache-probe.py','checks/secondary-local-probe.py',
          'checks/secondary-identity-probe.py','checks/primary-publication-probe.py','checks/a733-supervised-publication.py','checks/a733-smp-cached.py')},
        'cases':results,'limits':'Actual production cached-entry code; modeled registers, translations, GIC, power and memory. No physical cache visibility, coherence, interrupt delivery or scheduling result.'},indent=2)+'\n')
if __name__=='__main__':main()
