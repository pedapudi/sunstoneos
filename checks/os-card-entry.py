#!/usr/bin/env python3
"""Execute the actual linked stage-2 recorded entry and native OS admission.

The authenticated slot payload comes from card.py. SD transfer and the earlier
hardware gates are outside this focused check. Counter, SID and WDT registers
are modeled; actual entry instructions validate, arm, stamp and publish.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as reg


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('build', 'fixture', 'output'): p.add_argument('--'+name, type=Path, required=True)
    a = p.parse_args(); out = a.output; out.mkdir(parents=True, exist_ok=False)
    bundle = a.fixture/'bundle'
    raw = (a.build/'a733-stage2.bin').read_bytes()
    with (a.build/'a733-stage2.elf').open('rb') as stream:
        elf = ELFFile(stream)
        symbols = {s.name:int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols() if s['st_shndx'] != 'SHN_UNDEF'}
    runtime = (bundle/'runtime.bin').read_bytes()
    assert 0 < len(runtime) <= 0xf00000, 'Runtime overlaps session page'
    runtime_span = (len(runtime) + 4095) & ~4095
    session = (bundle/'slot/os-session.bin').read_bytes()
    header = (bundle/'slot/header.bin').read_bytes()
    grant_base = len(session) - 96
    policy = json.loads((bundle/'cold-policy.json').read_text())
    cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    system = {}
    for offset in range(0, len(raw)-3, 4):
        i = next(cs.disasm(raw[offset:offset+4],0x47000000+offset), None)
        if i and i.mnemonic in ('mrs','msr','wfi','wfe'): system[i.address] = (i.mnemonic,i.op_str.lower())
    cases = []
    def run(name, change=None, status=7, detail=None, readback=False, no_grant=False):
        s, h = bytearray(session), bytearray(header)
        if no_grant:
            s = s[:grant_base]; struct.pack_into('<Q', s, 608, grant_base); struct.pack_into('<Q', h, 136, grant_base)
        if change: change(s,h)
        u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        for address,size in ((0x47000000,0x400000),(0x41f00000,0x1000),(0x42000000,0x10000),(0x41000000,runtime_span)):
            u.mem_map(address,size)
        u.mem_write(0x47000000,raw);u.mem_write(0x41f00000,bytes(s));u.mem_write(0x47200000,bytes(h))
        u.mem_write(0x41000000,runtime)
        sid = dict(zip(range(0x200,0x210,4),policy['sid_words']))
        writes,reads,stop = [],[],[]
        state = {'counter':24000123,'instructions':0}
        wdt = {}
        def sid_read(cpu,offset,size,user):
            assert size==4 and offset in sid;reads.append(offset);return sid[offset]
        def forbidden(*unused):raise AssertionError('unexpected MMIO write')
        u.mmio_map(0x03006000,4096,sid_read,None,forbidden,None)
        def page(base):
            def read(cpu,offset,size,user):
                assert size==4
                if base==0x02050000:return 0 if readback and offset==0x14 else wdt.get(offset,0)
                return 0
            def write(cpu,offset,size,value,user):
                assert size==4 and base==0x02050000
                writes.append([base+offset,value]);wdt[offset]=value&0xffff
            return read,write
        for base in (0x02050000,0x03000000,0x07050000,0x07090000):
            r,w=page(base);u.mmio_map(base,4096,r,None,w,None)
        def instruction(cpu,address,size,user):
            state['instructions']+=1
            if address==0x41000000:stop.append('os-entry');cpu.emu_stop();return
            if address==symbols['rom_return']:stop.append('rom-return');cpu.emu_stop();return
            if address not in system:return
            mnemonic,operands=system[address]
            assert mnemonic not in ('wfi','wfe'), (name,mnemonic)
            first,second=operands.split(', ')
            if mnemonic=='mrs':
                assert second=='cntpct_el0'
                cpu.reg_write(getattr(reg,'UC_ARM64_REG_'+first.upper()),state['counter']);state['counter']+=240
            else:assert first=='daifset'
            cpu.reg_write(reg.UC_ARM64_REG_PC,address+4)
        u.hook_add(UC_HOOK_CODE,instruction)
        u.reg_write(reg.UC_ARM64_REG_SP,symbols['a733_stage2_stack_top'])
        u.reg_write(reg.UC_ARM64_REG_X0,0x47200000)
        u.emu_start(symbols['enter'],0,count=200000)
        report=bytes(u.mem_read(symbols['a733_stage2_report'],512))
        got,why=struct.unpack_from('<2Q',report,24)
        assert got==status and (detail is None or why==detail),(name,got,why,stop)
        result=bytes(u.mem_read(0x41f00000,len(s)))
        c=bytes(u.mem_read(0x42004000,224))
        if status==6:
            assert stop==['os-entry'] and reads==[0x200,0x204,0x208,0x20c]
            epoch=struct.unpack_from('<Q',result,24)[0]
            assert epoch and (no_grant or struct.unpack_from('<Q',result,grant_base+32)[0]==epoch)
            assert struct.unpack_from('<2Q',c,24)==(epoch,1)
            assert c[192:224]==hashlib.sha256(result).digest()
            assert struct.unpack_from('<Q',c)[0]==0x415337334f524331
            assert writes==[[0x02050018,0x16aa0002],[0x02050010,0x16aa0001],
                            [0x02050014,0x16aa0041],[0x0205000c,0x14af]]
            if not no_grant:
                (out/'contract.bin').write_bytes(c);(out/'runtime-session.bin').write_bytes(result)
                subprocess.run([a.fixture/'native-admission','--admit',out/'contract.bin',out/'runtime-session.bin',str(epoch+240)],check=True)
        else:
            assert stop==['rom-return'] and result==bytes(s) and not any(c)
            if status==7: assert not writes
        cases.append({'name':name,'status':got,'detail':why,'instructions':state['instructions'],'watchdog_writes':len(writes)})
    run(f'actual{len(session)}-fresh-cpu-authority',status=6)
    run(f'compatible{grant_base}-recorded',status=6,no_grant=True)
    for name,offset,fmt,value,detail in (
            ('persisted-board-epoch',24,'Q',9,3),('slot-generation-mismatch',32,'Q',9,2),
            ('sid-authority-absent',312,'Q',0,3),('wrong-board',320,'I',0,4),
            ('persisted-grant-epoch',grant_base+32,'Q',9,5),('grant-generation-mismatch',grant_base+40,'Q',9,5),
            ('grant-magic',grant_base,'Q',0,5),('grant-version',grant_base+8,'Q',2,5),('grant-size',grant_base+16,'Q',95,5),
            ('grant-owner',grant_base+24,'Q',0,5),('grant-one-cluster',grant_base+48,'I',1,5),('grant-zero-max',grant_base+52,'I',0,5),
            ('grant-zero-budget',grant_base+60,'I',0,5),('grant-over-budget',grant_base+60,'I',65,5),
            ('nonrecorded-grant',400,'I',4,3)):
        run(name,lambda s,h,o=offset,f=fmt,v=value:struct.pack_into('<'+f,s,o,v),detail=detail)
    run('empty-policy-digest',lambda s,h:s.__setitem__(slice(grant_base+64,grant_base+96),bytes(32)),detail=5)
    run('unknown-extent-size',lambda s,h:struct.pack_into('<Q',h,136,grant_base+97),detail=1)
    run('watchdog-readback-stops',status=8,readback=True)
    result={'passed':True,'physical_access':False,'cases':cases,'stage2_sha256':hashlib.sha256(raw).hexdigest(),
            'bundle_sha256':hashlib.sha256((bundle/'bundle.json').read_bytes()).hexdigest(),
            'checker_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            'scope':'Actual linked enter instructions after modeled successful load/gates; actual native OS admission. Counter, SID and watchdog are modeled.'}
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS',len(cases),'cold entry cases')

if __name__=='__main__':main()
