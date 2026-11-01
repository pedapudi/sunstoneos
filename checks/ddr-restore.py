#!/usr/bin/env python3
"""Execute real restore admission, patched BLX and abort in Unicorn.

boot0 setup/training/timing are modeled; MMIO is memory with explicit faults.
This establishes instruction ordering, not PHY or physical RAM operation.
Byte-lane register 0x73 (offset 0xe6 in each lane) reads 0 whatever is
written, as port 3-1 showed on 2026-10-31. With --reproduce MAILBOX only the
nominal case runs, and restore words 7-13 must equal that board mailbox's.
The ACSM memory (0x0a982000-0x0a983fff, 64-bit words) and the PHY data memory
(0x0a9b0000-0x0a9bffff, 32-bit words) hold each halfword write until the
word's highest halfword is written, and read the stored word until then, as
the port 3-1 survey showed; the helper must write each touched word whole,
in ascending order, with the other halfwords' stored values.
For a survey build (tools/prepare-ddr.py --survey) every case also checks the
survey at 0x96000, and the survey-classes case makes one entry each drop its
write, read 0, read another value, change after a later write and already
hold its value; tools/ddr-survey.py must decode each class.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UC_HOOK_MEM_READ
from unicorn.arm_const import *
import sys
sys.path.insert(0, str(Path(__file__).parent))
from ddr_clock_model import ROM, ClockModel

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('ddr_survey', ROOT/'tools/ddr-survey.py')
survey_tool = importlib.util.module_from_spec(spec); spec.loader.exec_module(survey_tool)
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('build', type=Path)
p.add_argument('--record', type=Path, required=True)
p.add_argument('--other-record', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--reproduce', type=Path, help='board mailbox.bin whose restore words 7-13 the nominal case must reproduce')
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
spec = importlib.util.spec_from_file_location('training', ROOT/'tools/training-record.py')
t = importlib.util.module_from_spec(spec); spec.loader.exec_module(t)
original = a.record.read_bytes(); parsed = t.parse(original)
other = a.other_record.read_bytes()
assert t.parse(other)['chip_id'] != parsed['chip_id']
manifest = json.loads((a.build/'manifest.json').read_text())
assert manifest['variant'] == 'restore'
phy = [(run['base']+2*i, value) for run in parsed['runs'][1:] for i,value in enumerate(run['values'])]
masks = {m['address']:m['mask'] for m in parsed['masks']}
PHY_COUNT = manifest['restore']['phy_registers']
assert len(phy) == PHY_COUNT and len(parsed['runs'][0]['values']) == 40
# Board observation, evidence/dev-runs-1031/3-1-ddr-restore: 0x0a9200e6 read
# 0 after 6 was written. The lanes share one register map, so every lane's
# copy is modelled the same way. Such registers are left out of the set.
HARDWARE_OWNED = {0x0a920000 + 0x2000*block + 0x200*lane + 0xe6 for block in range(4) for lane in range(9)}
if not a.reproduce:
    assert not HARDWARE_OWNED & set(masks)
# readback-survey: three profile entries that read a mismatching value.
WIDE = ((0x0a982000, 0x2000, 8), (0x0a9b0000, 0x10000, 4))
def width(address):
    return next((w for base, size, w in WIDE if base <= address < base+size), 2)
# Halfwords of touched wide words that are not profile entries hold this value.
OTHER = 0xc3c3
WIDE_WORDS = sorted({address & ~(width(address)-1) for address,_ in phy if width(address) > 2})
SURVEY = [phy[0][0], phy[100][0], phy[500][0]]
results = []
# Quick check layout (boot/a733/ddr-restore.c): 31 address-line words and
# sixteen 4 KiB windows. Only these DRAM pages exist in the fixture.
LINES = [0x40000000 + (1 << (n+1) if n else 0) for n in range(31)]
WINDOWS = [0x40000000 + w*0x0c000000 for w in range(16)]
DRAM_PAGES = sorted({address & ~0xfff for address in LINES} | set(WINDOWS))
SAMPLE = manifest['restore']['sample_read_bytes']
assert SAMPLE == 31*4*2 + 16*4096*4

def repair(data, payload=False):
    if payload: struct.pack_into('<I', data, 20, t.crc(data[128:]))
    struct.pack_into('<I', data, 124, t.crc(data[:124]))

cases = {'nominal':0, 'wrong-board':4, 'header-crc':2, 'payload-crc':3, 'strap':6,
         'boot0':5, 'geometry':1, 'run-overflow':1, 'unauthorized-address':1,
         'unauthorized-mask':1, 'mode-register-section':1, 'parameter-high-half':0,
         'readback':7, 'readback-survey':7, 'final-readback':7, 'memory':9, 'address-alias':9,
         'window-bit':9, 'size':8, 'missing-hook':10}
SURVEYED = manifest['restore'].get('survey')
if SURVEYED:
    cases['survey-classes'] = 7
    # Entries whose write is observable from the fixture's 0x5a5a and whose
    # expected value is nonzero under the mask.
    observable = [n for n,(address,value) in enumerate(phy)
                  if (0x5a5a^value)&masks[address] and value&masks[address] and width(address) == 2 and n+1 < PHY_COUNT
                  and width(phy[n+1][0]) == 2]
    SURVEY_CLASS = dict(zip([observable[k*len(observable)//5] for k in range(5)],
                            ('unchanged', 'reads-zero', 'other-value', 'changed-later', 'already-at-value')))
    assert len(SURVEY_CLASS) == 5 and all(n+1 < PHY_COUNT for n in SURVEY_CLASS)
if a.reproduce:
    board = struct.unpack_from('<7I', a.reproduce.read_bytes(), 28)
    cases = {'nominal': board[0]}
for case, error in cases.items():
    record = bytearray(original)
    if case=='wrong-board': record = bytearray(other)
    if case=='header-crc': record[24] ^= 1
    if case=='payload-crc': record[136] ^= 1
    if case=='strap': record[56] ^= 1; repair(record)
    if case=='boot0': record[48] ^= 1; repair(record)
    if case=='geometry': struct.pack_into('<I', record,16,0xffffffff); repair(record)
    if case=='run-overflow': struct.pack_into('<I', record,132,0xffffffff); repair(record,True)
    if case=='unauthorized-address': struct.pack_into('<I', record,128+88,0x03006200); repair(record,True)
    if case=='unauthorized-mask': record[128+struct.unpack_from('<I',record,84)[0]+4] ^= 1; repair(record,True)
    if case=='mode-register-section': struct.pack_into('<I',record,96,1); repair(record)
    if case=='parameter-high-half': record[138] ^= 0x80; repair(record,True)
    cpu = Uc(UC_ARCH_ARM,UC_MODE_ARM)
    cpu.mem_map(0x40000,0xc0000); cpu.mem_map(0x03006000,0x1000);cpu.mem_map(0x0a900000,0x200000)
    # Wide memories: stored halfwords, halfwords held per word, and every write.
    wide={'stored':{},'held':{},'writes':[]}
    def wide_read(uc,offset,size,base):
        assert size==2,('wide read size',size)
        return wide['stored'].get(base+offset,0)
    def wide_write(uc,offset,size,value,base):
        assert size==2,('wide write size',size)
        address=base+offset;w=width(address);word=address&~(w-1)
        wide['writes'].append((address,value&0xffff))
        wide['held'].setdefault(word,{})[address]=value&0xffff
        if address==word+w-2:wide['stored'].update(wide['held'].pop(word))
    for base,size,_ in WIDE:
        cpu.mem_unmap(base,size);cpu.mmio_map(base,size,wide_read,base,wide_write,base)
    def poke(uc,address,value):
        if width(address)>2:wide['stored'][address]=value&0xffff
        else:uc.mem_write(address,struct.pack('<H',value&0xffff))
    def peek(uc,address):
        return wide['stored'].get(address,0) if width(address)>2 else struct.unpack('<H',uc.mem_read(address,2))[0]
    for word in WIDE_WORDS:
        for h in range(word,word+width(word),2):wide['stored'][h]=OTHER
    for page in DRAM_PAGES:cpu.mem_map(page,0x1000)
    fel = bytes(range(256))*48
    cpu.mem_write(0x47000,fel)
    for address,name in ((0x8e000,'boot0-head.bin'),(0x4a000,'boot0-body.bin'),(0x85000,'ddr-entry.bin'),(0x92000,'ddr-restore.bin')):
        cpu.mem_write(address,(a.build/name).read_bytes())
    cpu.mem_write(0x87000,bytes(record));cpu.mem_write(0x03006200,struct.pack('<4I',*parsed['chip_id']))
    for address,_ in phy:poke(cpu,address,0x5a5a)
    if case=='survey-classes':
        already=next(n for n,c in SURVEY_CLASS.items() if c=='already-at-value')
        poke(cpu,phy[already][0],(0x5a5a&~masks[phy[already][0]])|(phy[already][1]&masks[phy[already][0]]))
        injected={phy[n][0]:c for n,c in SURVEY_CLASS.items()}
        later={phy[n+1][0]:phy[n][0] for n,c in SURVEY_CLASS.items() if c=='changed-later'}
    cpu.mem_write(0x0aaa0000,struct.pack('<H',1))
    # boot0 sets UcclkHclkEnables to 3 before the firmware run; MicroReset 1 stalls it.
    cpu.mem_write(0x0aa80100,struct.pack('<H',3));cpu.mem_write(0x0aaa0132,struct.pack('<H',1))
    cpu.reg_write(UC_ARM_REG_CPSR,0x60000053);cpu.reg_write(UC_ARM_REG_SP,0xf3000);cpu.reg_write(UC_ARM_REG_LR,0xf0000)
    regs=(UC_ARM_REG_R4,UC_ARM_REG_R5,UC_ARM_REG_R6,UC_ARM_REG_R7,UC_ARM_REG_R8,UC_ARM_REG_R9,UC_ARM_REG_R10,UC_ARM_REG_R11)
    for i,reg in enumerate(regs):cpu.reg_write(reg,0x12340000+i)
    state={'instructions':0,'init':0,'timing':0,'continuation':0,'injections':0,'dram_reads':0,'dram_writes':0,'phy_writes':[], 'faulted':False, 'survey':set()}
    selected=(a.build/'boot0-head.bin').read_bytes()[0x574:0x5f4]
    wanted=list(struct.unpack('<32I',selected));wanted[30]&=~4;wanted[27]=(wanted[27]&~0x3fff0000)|0x10000;wanted[23]|=0x80000000
    def ret(uc,result=0):
        uc.reg_write(UC_ARM_REG_R0,result);uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))
    def code(uc,address,size,data):
        state['instructions']+=1
        if address==0x49898:ret(uc)
        elif address==0x48fac:uc.mem_write(0x47038,selected);ret(uc)
        elif address==0x49bdc:
            state['init']+=1
            assert bytes(uc.mem_read(0x831f8,128))==struct.pack('<32I',*wanted)
            assert bytes(uc.mem_read(0x83278,80))==bytes(record[136:216])
            state['return']=uc.reg_read(UC_ARM_REG_LR)
            if case=='missing-hook':ret(uc,12288);return
            # Model a nested boot0 frame with every callee-saved register live.
            state['saved']=[uc.reg_read(reg) for reg in regs]
            state['sp']=uc.reg_read(UC_ARM_REG_SP)
            uc.reg_write(UC_ARM_REG_SP,state['sp']-0x100)
            for i,reg in enumerate(regs):uc.reg_write(reg,0xaabb0000+i)
            uc.reg_write(UC_ARM_REG_R0,0);uc.reg_write(UC_ARM_REG_R1,0x831f8)
            uc.reg_write(UC_ARM_REG_PC,0x56143) # Execute actual linked Thumb BLX.
        elif address==0x4bca8:
            state['timing']+=1
            assert uc.reg_read(UC_ARM_REG_R0)==0 and uc.reg_read(UC_ARM_REG_R1)==0x831f8
            assert uc.reg_read(UC_ARM_REG_LR)==0x56147
            assert struct.unpack('<H',uc.mem_read(0x0aaa0000,2))[0]==1
            ret(uc)
        elif address==0x56146:
            state['continuation']+=1
            assert not state['faulted']
            if case=='final-readback':
                address,value=phy[0];poke(uc,address,value^masks[address]);state['injections']+=1
            for reg,value in zip(regs,state['saved']):uc.reg_write(reg,value)
            uc.reg_write(UC_ARM_REG_SP,state['sp']);uc.reg_write(UC_ARM_REG_LR,state['return'])
            ret(uc,4096 if case=='size' else 12288)
        elif 0x47000<=address<0x83000:
            assert address==0x56142,hex(address)
    def memory(uc,access,address,size,value,data):
        if access==16 and address in HARDWARE_OWNED:uc.mem_write(address,b'\0\0')
        if access==16 and case=='readback-survey' and address in SURVEY:
            poke(uc,address,dict(phy)[address]^masks[address]);state['survey'].add(address);state['faulted']=True
        if address>=0x40000000:
            assert not state['faulted'],('DRAM after fault',hex(address))
            if access==17:
                state['dram_writes']+=size
                # Address line 20 stuck low: DRAM + 2^20 also lands on DRAM.
                if case=='address-alias' and address==LINES[19]:
                    uc.mem_write(LINES[0],struct.pack('<I',value&0xffffffff));state['injections']+=1
            else:
                state['dram_reads']+=size
                if case=='memory' and not state['injections']:
                    uc.mem_write(address,b'\xff'*size);state['injections']+=1;state['faulted']=True
                if case=='address-alias' and address==LINES[0] and state['injections']:state['faulted']=True
                # Data bit 8 stuck low in the last window, read in the last pass.
                if case=='window-bit' and address==WINDOWS[15]+4*0x20 and state['dram_reads']>SAMPLE-4096:
                    current=struct.unpack('<I',uc.mem_read(address,4))[0]
                    if current&0x100:
                        uc.mem_write(address,struct.pack('<I',current&~0x100));state['injections']+=1;state['faulted']=True
        if access==17 and address in masks:
            assert size==2
            assert struct.unpack('<H',uc.mem_read(0x0aaa0000,2))[0]==0
            state['phy_writes'].append(address)
        elif access==17 and 0x0a900000<=address<0x0ab00000 and address not in (0x0aaa0000,):
            # Only the other halfwords of a touched wide word, with their stored value.
            assert width(address)>2 and address&~(width(address)-1) in WIDE_WORDS,('PHY write outside the profile',hex(address))
            assert value&0xffff==wide['stored'][address],('refill changes a halfword',hex(address))
            assert struct.unpack('<H',uc.mem_read(0x0aaa0000,2))[0]==0
        if case=='survey-classes':
            if access==17 and address in later:
                # A later entry's write changes the earlier one, as a hardware copy would.
                earlier=later[address];poke(uc,earlier,peek(uc,earlier)^masks[earlier]);state['injections']+=1
            if access==16 and injected.get(address) in ('unchanged','reads-zero','other-value') and address in state['phy_writes']:
                kind=injected[address];m=masks[address];written=(0x5a5a&~m)|(dict(phy)[address]&m)
                value={'unchanged':0x5a5a,'reads-zero':0,'other-value':written^m^0x8000}[kind]
                poke(uc,address,value);state['survey'].add(address);state['faulted']=True
        if access==16 and case=='readback' and state['phy_writes'] and address==phy[0][0] and not state['injections']:
            poke(uc,address,phy[0][1]^masks[address]);state['injections']+=1;state['faulted']=True
    clocks = ClockModel(cpu, manifest['clock_ticks']) if 'clock_ticks' in manifest else None
    cpu.hook_add(UC_HOOK_CODE,code);cpu.hook_add(UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE,memory)
    try:
        cpu.emu_start(0x85000,0xf0000,count=4000000)
    except Exception:
        (a.output/'failure.json').write_text(json.dumps({'case':case,'state':{k:v for k,v in state.items() if k!='survey'},
            'pc':cpu.reg_read(UC_ARM_REG_PC),'lr':cpu.reg_read(UC_ARM_REG_LR),
            'sp':cpu.reg_read(UC_ARM_REG_SP)},indent=2)+'\n')
        raise
    assert cpu.reg_read(UC_ARM_REG_PC)==0xf0000,(case,state)
    assert cpu.reg_read(UC_ARM_REG_SP)==0xf3000
    assert cpu.reg_read(UC_ARM_REG_CPSR)==0x60000053
    for i,reg in enumerate(regs):assert cpu.reg_read(reg)==0x12340000+i,(case,i)
    assert bytes(cpu.mem_read(0x47000,0x3000))==fel
    mailbox=struct.unpack('<32I',cpu.mem_read(0x86000,128))
    if clocks:
        # Every return after board setup puts CPU-L and the DSU on the ROM point.
        clocks.check_sequence('cpu-l',0x03010000);clocks.check_sequence('dsu',0x03010105)
        assert {k:clocks.words[k] for k in ROM}==ROM and mailbox[15]==0,(case,mailbox[15])
    assert mailbox[0:2]==(0x53524444,5)
    assert mailbox[7]==error,(case,mailbox[:14],state)
    if SURVEYED:
        data=bytes(cpu.mem_read(SURVEYED['address'],SURVEYED['bytes']))
        if not mailbox[9]:
            # No hook call: the survey stays as the host cleared it.
            assert data==bytes(SURVEYED['bytes']),case
        else:
            entries,summary=survey_tool.decode(data,[address for address,_ in phy],[value for _,value in phy])
            assert [int(e['mask'],16) for e in entries]==[masks[address] for address,_ in phy]
            # Hook-time control words as the fixture holds them: the mux was 1.
            assert summary['control']['micro_cont_mux_sel_at_hook']=='0x0001'
            assert summary['control']['ucclk_hclk_enables']=='0x0003' and summary['control']['micro_reset']=='0x0001'
            # Fixture classes: an entry whose 0x5a5a already matches cannot show its
            # write; a wide-memory entry below its word's highest halfword reads
            # the stored word straight after its write.
            natural={n for n,(address,value) in enumerate(phy) if not (0x5a5a^value)&masks[address]}
            late={n for n,(address,value) in enumerate(phy) if n not in natural and width(address)>2
                  and address!=(address&~(width(address)-1))+width(address)-2}
            expected_class={n:'already-at-value' if n in natural else 'stored-later' if n in late else 'held'
                            for n in range(PHY_COUNT)}
            assert summary['refilled_halfwords']==sum(1 for word in WIDE_WORDS for h in range(word,word+width(word),2)
                                                      if h not in masks),summary['refilled_halfwords']
            # The survey describes the write pass; final-readback fails only the later pass.
            assert summary['mismatches']==(0 if case=='final-readback' else mailbox[27]),(case,summary,mailbox[27])
            if case=='survey-classes':
                expected_class.update(SURVEY_CLASS)
                assert {e['index']:e['class'] for e in entries}==expected_class,summary
                assert mailbox[27]==4,mailbox[27]
            elif case in ('readback','readback-survey'):
                assert set(summary['classes'])<={'held','already-at-value','stored-later','other-value','unchanged'},summary
            else:
                assert {e['index']:e['class'] for e in entries}==expected_class,(case,summary)
                assert all(int(e['before'],16)==0x5a5a for e in entries)
    if a.reproduce:
        # The board restore stopped before boot0 resumed; so must this one.
        assert mailbox[7:14]==board,(mailbox[7:14],board)
        assert state['timing']==state['continuation']==0 and not state['dram_reads'] and not state['dram_writes']
    elif case in ('nominal','parameter-high-half'):
        assert state['phy_writes']==[a for a,v in phy]
        for address,value in phy:
            actual=peek(cpu,address)
            assert actual==((0x5a5a&~masks[address])|(value&masks[address]))
        # Each touched wide word is written once, whole and in ascending order;
        # its other halfwords keep their values and nothing stays held.
        words=[address&~(width(address)-1) for address,_ in wide['writes']]
        assert sorted(set(words))==WIDE_WORDS and not wide['held']
        for word in WIDE_WORDS:
            assert [address for address,_ in wide['writes'] if address&~(width(address)-1)==word]==list(range(word,word+width(word),2)),hex(word)
            assert all(wide['stored'][h]==OTHER for h in range(word,word+width(word),2) if h not in masks)
        assert mailbox[8:11]==(PHY_COUNT,1,SAMPLE) and mailbox[27:32]==(0,)*5
        assert state['dram_reads']==state['dram_writes']==SAMPLE
        assert mailbox[25] or mailbox[26]
    elif error in (1,2,3,4,5,6):
        assert state['init']==0 and not state['phy_writes'] and not state['dram_reads'] and not state['dram_writes']
        assert bytes(cpu.mem_read(0x831f8,128))==selected
    elif case=='readback':
        # A mismatch does not stop the pass; boot0 still never resumes.
        assert state['injections']==1 and state['phy_writes']==[a for a,v in phy]
        assert state['timing']==state['continuation']==0 and not state['dram_reads'] and not state['dram_writes']
        assert mailbox[8:14]==(PHY_COUNT,1,0,phy[0][0],phy[0][1],phy[0][1]^masks[phy[0][0]])
        assert mailbox[27:32]==(1,0,0,0,0)
    elif case=='readback-survey':
        assert state['survey']=={*SURVEY} and state['phy_writes']==[a for a,v in phy]
        assert state['timing']==state['continuation']==0 and not state['dram_reads'] and not state['dram_writes']
        assert mailbox[11:14]==(phy[0][0],phy[0][1],phy[0][1]^masks[phy[0][0]])
        assert mailbox[27:32]==(3,100<<16|phy[100][1]^masks[phy[100][0]],500<<16|phy[500][1]^masks[phy[500][0]],0,0)
    elif case in ('size','final-readback','missing-hook'):assert not state['dram_reads'] and not state['dram_writes']
    elif case=='survey-classes':
        assert state['phy_writes']==[a for a,v in phy] and state['injections']==1 and len(state['survey'])==3
        assert state['timing']==state['continuation']==0 and not state['dram_reads'] and not state['dram_writes']
    elif case=='memory':
        assert state['injections']==1 and state['dram_reads']==4 and state['dram_writes']==31*4
        assert mailbox[10:12]==(4,LINES[0])
    elif case=='address-alias':
        # The aliased base word is the first one read back.
        assert state['injections']==1 and mailbox[10:13]==(4,LINES[0],0x01010101)
        assert mailbox[13]==0x14141414 and state['dram_writes']==31*4
    elif case=='window-bit':
        assert state['injections']==1 and mailbox[11]==WINDOWS[15]+4*0x20
        assert mailbox[12]^mailbox[13]==0x100 and mailbox[10]==SAMPLE-4096+4*0x21
        assert state['dram_writes']==SAMPLE
    results.append({'case':case,'error':error,'instructions':state['instructions'],
                    'phy_writes':len(state['phy_writes']),'dram_read_bytes':state['dram_reads'],
                    'dram_write_bytes':state['dram_writes'],'injections':state['injections'],'pass':True})
(a.output/'results.json').write_text(json.dumps({'physical_access':False,'instruction_limit':4000000,
    'reproduce':str(a.reproduce) if a.reproduce else None,'hardware_owned':sorted(HARDWARE_OWNED),
    'record_sha256':hashlib.sha256(original).hexdigest(),'other_record_sha256':hashlib.sha256(other).hexdigest(),
    'manifest':manifest,'cases':results},indent=2)+'\n')
print('PASS restore wrapper:',len(results),'cases; actual BLX/abort; full-width parameters; bounded samples.')
