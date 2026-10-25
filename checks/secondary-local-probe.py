#!/usr/bin/env python3
"""Run production per-core preparation and interrupt setup with modeled hardware."""
import argparse
import copy
import ctypes
import importlib.util
import json
import pickle
from pathlib import Path
import struct
from unicorn import UC_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_PC
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('survey',ROOT/'checks/secondary-identity-probe.py')
survey=importlib.util.module_from_spec(spec);spec.loader.exec_module(survey)
model=survey.model
LOCAL=0x42018a00
DISTRIBUTOR=0x42019a00

class Local(survey.Survey):
    def restore(self,snapshot,fault):
        super().restore(snapshot,fault)
        self.in_survey=self.boundary=True
        self.decoded={}
        self.target_parked=set()
        self.cpus[0].reg_write(UC_ARM64_REG_PC,self.survey_hooks['identity_primary_gate'])
        # Only the unexecuted extension and secondary-entry branch differ from
        # the accepted prefix. Never reload initialized data or native BSS.
        ctypes.memmove(self.ram_pointer+0x20000,self.overlay,len(self.overlay))
        target=self.local_manifest['native_symbols']['a733_cpu_secondary_continue']
        patch=next(p for p in self.local_manifest['patches'] if p['space']=='native')
        assert patch['address']==target
        ctypes.memmove(self.ram_pointer+target-0x41000000,struct.pack('<I',patch['replacement']),4)
        ctypes.memset(self.ram_pointer+survey.REPORT-0x41000000,0,0x1b00)
        self.mmio[0x03400000]=0x37
        self.mmio[0x03400004]=3
        for i in range(1,4):self.mmio[0x03400100+4*i]=0x10101*i
        for c,r in enumerate(self.rdist):
            self.mmio[r+0x24]=0x100
            self.mmio[r+0x10200]=0
            self.mmio[r+0x10080]=0x80000000
            self.mmio[r+0x10d00]=0x100
            self.mmio[r+0x10c04]=0x88000000
            self.mmio[r+0x10400]=0xaabbcc00+c*16
            self.regs[c].update(icc_sre_el3=9,icc_igrpen0_el1=0,icc_igrpen1_el3=0)
            if c>=6:self.regs[c]['midr_el1']=0x414fd0b1
            # Include the actually observed reset values; do not copy the boot CPU.
            if c>=6:self.regs[c]['sctlr_el1']=0x30d50838
            else:self.regs[c]['sctlr_el1']=0xc50838
            if c==6:self.regs[c]['tcr_el1']=0x600e12242aa
            if c==7:self.regs[c]['tcr_el1']=0x8250e02a402a
        self.original_mmio=copy.deepcopy(self.mmio)
        self.original_regs=copy.deepcopy(self.regs)

    def memory(self,u,access,address,size,value,core):
        write=access==UC_MEM_WRITE
        if address in model.EL3_SECURITY_ADDRESSES:
            # The native EL3 security setup, which the pre-scheduler profiles
            # call; the base model checks and records it.
            return model.Machine.memory(self,u,access,address,size,value,core)
        if address<0x41000000:
            if write:self.mmio_writes.append((address,size,value))
            if 0x03400000<=address<0x03600000:
                if address<0x03460000:
                    assert core==0 or (not write and address==0x03400000)
                    assert address in {0x03400000,0x03400004,*range(0x03400104,0x03400110,4),*range(0x03400184,0x03400190,4)}
                    if write and 0x03400184<=address<0x03400190:
                        self.mmio[address-0x80]&=~value;self.mmio[address]=value;return
                    if write and 0x03400104<=address<0x03400110:
                        self.mmio[address]|=value;return
                else:
                    frame=(address-0x03460000)//0x20000;r=self.rdist[frame];offset=address-r
                    assert core and (frame==core or (not write and offset==8)),('cross-core local access',core,hex(address))
                    if write:
                        if offset==0x14:
                            # ChildrenAsleep is read-only; model the documented handshake.
                            self.mmio[address]=(value&~4)|(4 if value&2 else 0);return
                        if offset==0x24:
                            assert value==0;self.mmio[address]=0x100;return
                        if offset in (0x10280,0x10380):
                            self.mmio[address-0x80]&=~value;self.mmio[address]=value;return
                        if offset==0x10080:
                            value &= 0xfff0ffff
                            if self.fault=='group-refusal' and core==6 and value==0xfff0ffff:value &= ~1
                            self.mmio[address]=value;return
                        if offset==0x10400 and self.fault=='cleanup-readback' and core==6 and value==self.original_mmio[address]:
                            self.mmio[address]=value^0x10;return
            elif write and 0x07050204<=address<=0x0705021c:
                target=(address-0x07050200)//4
                if value&1:
                    assert core==0 and not any(self.active[1:])
                    assert target==len(self.started)+1
                    self.started.append(target)
                if core and self.shutdown_steps[core]:
                    assert target==core and self.shutdown_steps[core]==['power','dsb','isb']
                    self.shutdown_steps[core].append('control')
            else:
                allowed={0x08001004,0x08001008}
                for c in range(1,8):
                    allowed.update((0x08001000+c*0x1000,0x08001004+c*0x1000,0x08001008+c*0x1000,
                                    0x07050200+c*4,0x07050220+c*4,0x07052008+c*0x1000))
                assert address in allowed and size==4,('unexpected MMIO',core,hex(address))
                if write and core==0 and address in (0x08001000+c*0x1000 for c in self.started):
                    target=(address-0x08001000)//0x1000
                    if value==self.original_mmio[address]:assert target in self.stopped
        return model.Machine.memory(self,u,access,address,size,value,core)

    def instruction(self,u,address,size,core):
        if address==self.survey_hooks['identity_park']:
            if core:
                self.target_parked.add(core);self.clock=self.deadline+1
            else:self.retained=True
            u.emu_stop();return
        assert address!=self.symbols['a733_smp_stop'],('native stop',core)
        if address not in self.decoded:
            self.decoded[address]=next(self.decoder.disasm(bytes(u.mem_read(address,4)),address))
        ins=self.decoded[address];op,args=ins.mnemonic,ins.op_str.split(', ')
        if op=='msr':
            name,src=args
            if name=='rmr_el3':
                assert core==0 and self.reg(core,src)==2 and not any(self.active[1:])
                assert self.q(DISTRIBUTOR)==4
                self.returned=True;u.emu_stop();return
            if name in ('sctlr_el3','tcr_el3'):raise AssertionError('Unexpected EL3 translation change')
            if name.startswith('icc_'):assert name=='icc_sre_el3','Interrupt delivery must remain disabled'
            if name=='s3_0_c15_c2_7':
                assert core and self.reg(core,src)==0x101
                self.shutdown_steps[core].append('power')
        if core and op=='dsb' and self.shutdown_steps[core]==['power']:self.shutdown_steps[core].append('dsb')
        if core and op=='isb' and self.shutdown_steps[core]==['power','dsb']:self.shutdown_steps[core].append('isb')
        if core and op=='wfi' and address==self.survey_hooks['identity_target_terminal']+4:
            assert self.shutdown_steps[core]==['power','dsb','isb','control']
            assert self.q(survey.SLOTS+core*256+136)==self.epoch
            assert self.q(LOCAL+core*512) in (3,4)
            for n in ('sctlr_el1','tcr_el1'):assert self.regs[core][n]==self.original_regs[core][n]
            self.mmio[0x07052008+core*0x1000]=0;self.active[core]=False;self.stopped.append(core)
            u.emu_stop();return
        return model.Machine.instruction(self,u,address,size,core)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','bundle','prefix','output'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--case',choices=('success','group-refusal','cleanup-readback'),action='append')
    args=parser.parse_args();machine=Local(args.image,args.bundle)
    machine.local_manifest=json.loads((args.bundle/'manifest.json').read_text())
    machine.overlay=(args.bundle/'overlay.bin').read_bytes()
    meta=json.loads(args.prefix.with_suffix('.json').read_text())
    assert survey.primary.probe.digest(args.prefix)==meta['snapshot_sha256']
    for path,sha in meta['inputs'].items():assert survey.primary.probe.digest(Path(path))==sha
    old_manifest=json.loads(next(Path(p).read_text() for p in meta['inputs'] if p.endswith('/manifest.json')))
    assert old_manifest['primary_manifest']==machine.local_manifest['primary_manifest']
    # The executed primary preparation and return prefix must be byte-identical.
    base=Path(old_manifest['primary_manifest']['path'])
    base_manifest=json.loads(base.read_text())
    checkpoint=base_manifest['hooks']['primary_publication_return']-0x41020000
    original=(base.parent/'overlay.bin').read_bytes()
    assert machine.overlay[:checkpoint]==original[:checkpoint]
    snapshot=pickle.loads(args.prefix.read_bytes()) # Trusted local file with checked digest.
    cases=[]
    for name in args.case or ('success','group-refusal','cleanup-readback'):
        machine.restore(snapshot,None if name=='success' else name)
        machine.execute()
        header=struct.unpack('<32Q',machine.read(survey.REPORT,256))
        local=[struct.unpack('<64Q',machine.read(LOCAL+c*512,512)) for c in range(1,8)]
        if name=='cleanup-readback':
            assert machine.retained and not machine.returned and machine.target_parked=={6}
            assert machine.started==list(range(1,7)) and machine.stopped==list(range(1,6))
            # The final snapshot comparison (cleanup error 6) now covers the
            # restored priority word; the separate readback (error 4) is gone.
            assert local[5][11]==6 and local[5][0]==2
            assert machine.q(DISTRIBUTOR)==2
        else:
            assert machine.returned and not machine.retained
            assert machine.started==machine.stopped==list(range(1,8))
            assert header[:8]==(0x4137334c4f434131,1,0x1b00,machine.epoch,machine.deadline,0xfe,2,7)
            for c,r in enumerate(local,1):
                expected=4 if name=='group-refusal' and c==6 else 3
                assert r[0]==expected and r[1:4]==(machine.epoch,c,machine.deadline)
                assert r[11]==0 and r[12]==7 and r[17:27]==r[27:37]
                assert r[5]==r[6] and r[14]&7==7 and not (r[15]&1 or r[16]&3)
            for address,value in machine.original_mmio.items():
                if address==0x03400000 or 0x03400104<=address<0x03400110:
                    assert machine.mmio[address]==value
        record={'case':name,'pass':True,'started':machine.started,'off':machine.stopped,
                'returned':machine.returned,'retained':machine.retained,
                'local_reports':[list(r) for r in local]}
        (args.output.parent/('case-'+name+'.json')).write_text(json.dumps(record,indent=2)+'\n');cases.append(record)
        print('PASS',name,flush=True)
    args.output.write_text(json.dumps({'native_sha256':survey.primary.probe.digest(args.image),
        'manifest_sha256':survey.primary.probe.digest(args.bundle/'manifest.json'),'physical_access':False,
        'primary_prefix_reused':str(args.prefix),'primary_prefix_sha256':meta['snapshot_sha256'],
        'sources':{n:survey.primary.probe.digest(ROOT/n) for n in ('checks/secondary-local-probe.py',
          'checks/secondary-identity-probe.py','checks/primary-publication-probe.py','checks/a733-supervised-publication.py','checks/a733-smp-cached.py')},
        'cases':cases,'limits':'Actual native code; modeled CPU/GIC/power and RAM. No physical interrupt delivery, coherence or shutdown qualification.'},indent=2)+'\n')
if __name__=='__main__':main()
