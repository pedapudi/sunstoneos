#!/usr/bin/env python3
"""Execute concurrent cached exchange and verify cleaning/shutdown ordering."""
import argparse,ctypes,importlib.util,json,pickle,struct
from pathlib import Path
from unicorn import UC_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_PC
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('cache_check',ROOT/'checks/secondary-cache-probe.py')
cached=importlib.util.module_from_spec(spec);spec.loader.exec_module(cached)
local=cached.local;model=cached.model;digest=cached.digest
SHARED=0x41243000

class Coherent(cached.Cached):
    def restore(self,snapshot,fault):
        super().restore(snapshot,fault)
        ctypes.memset(self.ram_pointer+0x42018100-0x41000000,0,0x4100)
        assert self.read(SHARED,4096)==bytes(4096),'Borrowed stack page must be initialized/published by native boot'
        self.cleaned={c:[] for c in range(1,8)};self.clean_complete=set();self.dirty=set()
        self.payload_fault=False;self.overlap=False;self.highest_active=0

    def memory(self,u,access,address,size,value,core):
        write=access==UC_MEM_WRITE
        if 0x07050204<=address<=0x0705021c and write and value&1:
            target=(address-0x07050200)//4
            assert core==0 and target==len(self.started)+1
            self.started.append(target)
            self.highest_active=max(self.highest_active,sum(self.active[1:])+1)
            return model.Machine.memory(self,u,access,address,size,value,core)
        if SHARED<=address<SHARED+4096:
            assert address+size<=SHARED+4096
            if core==0:
                assert not write and self.stopped==list(range(1,8)), 'Primary reads only after every writer is OFF'
            else:
                assert self.regs[core]['currentel']==4 and core in self.el1_enabled
                if write:
                    assert not self.cleaned[core], 'Normal store after cache cleaning'
                    self.dirty.add(address&~63);self.changed(address,size)
                if not write and address==SHARED+128 and core==6 and self.fault=='payload-corruption' and self.q(SHARED)==5:
                    assert not self.payload_fault
                    u.mem_write(address,(self.q(address)^1).to_bytes(8,'little'));self.payload_fault=True
            return
        if core and self.regs[core]['currentel']==4:
            assert not write and (0x41000000<=address<0x41200000 or self.sealed_el1_table_read(address,size)), ('EL1 access outside code/borrowed page',core,hex(address))
        return local.Local.memory(self,u,access,address,size,value,core)

    def instruction(self,u,address,size,core):
        if address not in self.decoded:self.decoded[address]=next(self.decoder.disasm(bytes(u.mem_read(address,4)),address))
        ins=self.decoded[address];op,args=ins.mnemonic,ins.op_str.split(', ')
        if core and op=='dc' and args[0]=='civac' and not self.production_table_clean(address):
            assert self.regs[core]['currentel']==4
            assert self.started==list(range(1,8)) and self.el1_seen==set(range(1,8))
            assert all(self.q(SHARED+0x100+c*128+16)==self.epoch for c in range(1,8)), 'Cleaning before last store'
            line=self.reg(core,args[1]);assert line==SHARED+64*len(self.cleaned[core])
            assert line<SHARED+4096
            self.cleaned[core].append(line)
        if core and address==self.survey_hooks['identity_coherence_clean_done']:
            assert len(self.cleaned[core])==64
            previous=next(self.decoder.disasm(bytes(u.mem_read(address-4,4)),address-4))
            assert previous.mnemonic=='dsb' and previous.op_str=='sy'
            self.clean_complete.add(core)
        if core and op=='smc':
            assert core in self.clean_complete and self.q(SHARED)==self.q(SHARED+64)==224
            self.overlap=True
        if core and op=='wfi' and address==self.survey_hooks['identity_target_terminal']+4:
            assert self.started==list(range(1,8)) and core in self.clean_complete
        return super().instruction(u,address,size,core)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for n in ('image','bundle','prefix','output'):parser.add_argument('--'+n,type=Path,required=True)
    parser.add_argument('--case',choices=('success','payload-corruption','restore-el3'),action='append')
    a=parser.parse_args();machine=Coherent(a.image,a.bundle)
    machine.local_manifest=json.loads((a.bundle/'manifest.json').read_text());machine.overlay=(a.bundle/'overlay.bin').read_bytes()
    meta=json.loads(a.prefix.with_suffix('.json').read_text());assert digest(a.prefix)==meta['snapshot_sha256']
    for path,sha in meta['inputs'].items():assert digest(Path(path))==sha
    prior=json.loads(next(Path(n).read_text() for n in meta['inputs'] if n.endswith('/manifest.json')))
    assert prior['primary_manifest']==machine.local_manifest['primary_manifest']
    base=Path(prior['primary_manifest']['path']);bm=json.loads(base.read_text())
    checkpoint=bm['hooks']['primary_publication_return']-0x41020000
    assert machine.overlay[:checkpoint]==(base.parent/'overlay.bin').read_bytes()[:checkpoint]
    snapshot=pickle.loads(a.prefix.read_bytes()) # Trusted local snapshot, hash checked above.
    results=[]
    for name in a.case or ('success','payload-corruption','restore-el3'):
        machine.restore(snapshot,None if name=='success' else name);machine.execute()
        raw=machine.read(0x42018000,0x4200)
        (a.output.parent/('report-'+name+'.bin')).write_bytes(raw)
        if name=='success':
            assert machine.returned and not machine.retained and machine.started==machine.stopped==list(range(1,8))
            assert machine.highest_active==7 and machine.clean_complete==set(range(1,8)) and machine.overlap
            assert raw[0x3200:]==machine.read(SHARED,4096)
            assert struct.unpack_from('<3Q',raw,256)==(0x413733434f484531,1,0x4100)
            for c in range(1,8):
                assert struct.unpack_from('<8Q',raw,0x3000+c*64)==(3,c,machine.epoch,32,224,machine.epoch^223,64,0)
        else:
            assert machine.retained and not machine.returned and 6 in machine.target_parked
            assert machine.started==list(range(1,8)) and machine.q(local.DISTRIBUTOR)==2
            if name=='payload-corruption':assert machine.payload_fault and machine.stopped==[] and not machine.clean_complete
            else:
                assert machine.stopped==list(range(1,len(machine.stopped)+1)) and 6 not in machine.stopped
                assert 6 in machine.clean_complete
                assert machine.q(cached.EL3+6*256)==2 and machine.q(cached.EL3+6*256+152)==1
        record={'case':name,'pass':True,'native_sha256':digest(a.image),'manifest_sha256':digest(a.bundle/'manifest.json'),
            'check_sha256':digest(Path(__file__)),'parked':sorted(machine.target_parked),'started':machine.started,'off':machine.stopped,
            'simultaneous_secondaries':machine.highest_active,'clean_complete':sorted(machine.clean_complete),
            'cleaned_lines':{str(c):len(v) for c,v in machine.cleaned.items()},
            'returned':machine.returned,'retained':machine.retained,'report_sha256':digest(a.output.parent/('report-'+name+'.bin'))}
        (a.output.parent/('case-'+name+'.json')).write_text(json.dumps(record,indent=2)+'\n');results.append(record)
        print('PASS',name,flush=True)
    results=[]
    for name in ('success','payload-corruption','restore-el3'):
        record=json.loads((a.output.parent/('case-'+name+'.json')).read_text())
        assert record['pass'] and record['native_sha256']==digest(a.image)
        assert record['manifest_sha256']==digest(a.bundle/'manifest.json')
        assert record['report_sha256']==digest(a.output.parent/('report-'+name+'.bin'))
        results.append(record)
    sources=('checks/secondary-coherence-probe.py','checks/secondary-cache-probe.py','checks/secondary-local-probe.py',
        'checks/secondary-identity-probe.py','checks/primary-publication-probe.py','checks/a733-supervised-publication.py','checks/a733-smp-cached.py')
    a.output.write_text(json.dumps({'native_sha256':digest(a.image),'manifest_sha256':digest(a.bundle/'manifest.json'),
        'physical_access':False,'primary_prefix_reused':str(a.prefix),'primary_prefix_sha256':meta['snapshot_sha256'],
        'sources':{n:digest(ROOT/n) for n in sources},'cases':results,
        'limits':'Actual instructions and modeled shared RAM, exclusive monitors, registers, GIC and power. Validates ordering/maintenance requests; physical cache visibility, coherence and timing remain unproven.'},indent=2)+'\n')
if __name__=='__main__':main()
