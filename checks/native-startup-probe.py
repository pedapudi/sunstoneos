#!/usr/bin/env python3
"""Execute OFF-secondary configuration and restoration before any power request."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import subprocess
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
for name in ('build','bundle','output','objdump'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();spec=importlib.util.spec_from_file_location('startup_host',ROOT/'tools/run-native-startup-probe.py');host=importlib.util.module_from_spec(spec);spec.loader.exec_module(host)
m=json.loads((a.build/'manifest.json').read_bytes());bundle=json.loads((a.bundle/'prepared.json').read_bytes())
assert bundle['image_sha256']==host.NATIVE and bundle['launcher_sha256']==host.LAUNCH
ops={};labels={}
for path in (a.bundle/'image.elf',a.build/'overlay.elf'):
    result=subprocess.run([str(a.objdump),'-d','--no-show-raw-insn',str(path)],capture_output=True,text=True,check=True).stdout
    for line in result.splitlines():
        label=re.match(r'^([0-9a-f]+) <([^>]+)>:$',line)
        if label:labels[label[2]]=int(label[1],16)
        match=re.match(r'^\s*([0-9a-f]+):\s+(.+)$',line)
        if match:ops[int(match[1],16)]=match[2].strip()
for address,old,new in m['patches']:ops[address]=''
nonce=bytes(range(1,17));reports=[]
def check(name,scenario='normal'):
    cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    for address,size in [(0x41000000,0x1200000),(0x02050000,0x1000),(0x03400000,0x1000),(0x07050000,0x5000),(0x08001000,0x2000)]:cpu.mem_map(address,size)
    for item in m['payloads']:cpu.mem_write(item['address'],(a.build/item['name']).read_bytes())
    cpu.mem_write(0x41020000,(a.build/'overlay.bin').read_bytes());cpu.mem_write(host.REPORT+240,nonce)
    contract=bytearray((a.bundle/'contract.bin').read_bytes());start=100
    struct.pack_into('<Q',contract,0,0x4153373353554333)
    struct.pack_into('<4Q',contract,80,start,start+24000000,start+480000000,start+480000000)
    cpu.mem_write(0x42004000,bytes(contract));cpu.reg_write(UC_ARM64_REG_SP,0x41414100)
    registers={'currentel':12,'mpidr_el1':0x81000000,'sctlr_el3':0xc50838,'sctlr_el1':0xc50838,
        'cntfrq_el0':0,'daif':0x3c0,'ctr_el0':0x8444c004,'midr_el1':0x412fd050,'clidr_el1':0xc3000023,
        'id_aa64mmfr0_el1':0x101122,'s3_0_c15_c3_1':0x41,'s3_0_c15_c3_0':121618999,'tcr_el1':0,
        'esr_el3':0x96000010,'elr_el3':0,'far_el3':0}
    original_ctlr=7 if scenario=='are-guard' else 0x37
    original_spi=[0xa55aa55a]+[(0x11223344^(i*0x10201))&0xffffffff for i in range(1,32)]
    gic={'ctlr':original_ctlr,'typer':0x7b0408,'spi':original_spi.copy(),'rwp':0}
    power_registers={0x07053008:8 if scenario=='prearm-on' else 0,0x07050204:0x10000,0x07050224:0x40,0x08002000:0x20,0x08002004:0x48000000,0x08002008:0}
    writes=[];controls=[];ticks=[];stopped=[];injected=[];publication=[];cleaned=[];power_reads=[];events=[];configuration_writes=[];barriers=[]
    ranges=((0x41000000,0x41200000),(0x41200000,0x41290d10),(0x421f0000,0x421f5000))
    def instruction(uc,address,size,unused):
        assert address!=0x41004444,'CPU/DSU write guard must not be crossed.'
        config_state=struct.unpack('<Q',uc.mem_read(0x42002280,8))[0]
        partial=scenario=='configuration-fault' and address==0x4100a25c and uc.reg_read(UC_ARM64_REG_X0+19)==0x08002008
        cleanup_fault=scenario=='cleanup-read-fault' and address==labels['configuration_require_off']+4 and config_state==2
        if (partial or cleanup_fault) and not injected:
            registers.update(elr_el3=address,far_el3=0x08002008 if partial else 0x07053008);injected.append(True)
            uc.reg_write(UC_ARM64_REG_PC,0x41008a00);return
        if address==0x4100a2d0:barriers.append(address)
        if address==0x41004558:
            begin,end=ranges[len(publication)]
            assert uc.reg_read(UC_ARM64_REG_X0)==begin and uc.reg_read(UC_ARM64_REG_X0+1)==end-begin
            publication.append({'begin':begin,'end':end,'lines':0});cleaned.clear()
        if address==0x4100460c:
            item=publication[-1];assert cleaned==list(range(item['begin'],item['end'],64));item['lines']=len(cleaned)
        text=ops.get(address,'')
        if text=='sev':
            assert 0x410034d4<=address<0x410036ec and not power_registers[0x07053008]&15
            assert struct.unpack('<Q',uc.mem_read(0x42002270,8))[0]==1
            assert struct.unpack('<Q',uc.mem_read(0x42005118,8))[0]==1
            events.append(address)
        if re.match(r'dc\s+cvac,\s*x0$',text,re.I):
            assert address==0x410045e0;cleaned.append(uc.reg_read(UC_ARM64_REG_X0));uc.reg_write(UC_ARM64_REG_PC,address+4);return
        assert not re.match(r'(dc|ic|tlbi)\s',text,re.I),text
        read=re.match(r'mrs\s+x(\d+),\s*(\S+)',text,re.I)
        if read:
            key=read[2].lower()
            if key=='cntpct_el0':
                cleanup_state=struct.unpack('<Q',uc.mem_read(0x42002100,8))[0]
                step=500000 if scenario=='cleanup-timeout' and cleanup_state==2 else 1
                value=(ticks[-1] if ticks else 200)+step;ticks.append(value)
            else:assert key in registers,(name,key,hex(address));value=registers[key]
            uc.reg_write(UC_ARM64_REG_X0+int(read[1]),value);uc.reg_write(UC_ARM64_REG_PC,address+4)
        elif text.lower().startswith('msr'):
            if 'DAIFSet' in text or 'SPSel' in text:uc.reg_write(UC_ARM64_REG_PC,address+4);return
            write=re.match(r'msr\s+(\S+),\s*(x\d+|xzr)',text,re.I);assert write,(name,text)
            key=write[1].rstrip(',').lower();value=0 if write[2]=='xzr' else uc.reg_read(UC_ARM64_REG_X0+int(write[2][1:]));controls.append([key,value])
            assert key in ('vbar_el3','cntfrq_el0','cntps_ctl_el1','cntp_ctl_el0','cntv_ctl_el0','rmr_el3','sctlr_el1','tcr_el1'),(name,key)
            if key=='rmr_el3':
                assert value==2 and writes[-2:]==[[0x08001004,4,0x20],[0x08001008,4,0]];stopped.append('return');uc.emu_stop()
            else:registers[key]=value;uc.reg_write(UC_ARM64_REG_PC,address+4)
        elif text=='wfi':stopped.append('park');uc.emu_stop()
    def read(uc,access,address,size,value,unused):
        if address in power_registers:
            assert size==4;power_reads.append(address)
            data=power_registers[address]
            config_state=struct.unpack('<Q',uc.mem_read(0x42002280,8))[0]
            if scenario=='native-readback-refusal' and address==0x08002004 and uc.reg_read(UC_ARM64_REG_PC)==0x4100a1bc and not injected:
                data^=4;injected.append(True)
            if scenario=='changed-power' and config_state==2 and address==0x07053008:data=8
            uc.mem_write(address,struct.pack('<I',data));return
        if not 0x03400000<=address<0x03401000:return
        assert size==4
        if address==0x03400000:
            cleanup_state=struct.unpack('<Q',uc.mem_read(0x42002100,8))[0]
            busy=scenario=='cleanup-timeout' and cleanup_state==2 or gic['rwp']>0
            if gic['rwp']:gic['rwp']-=1
            data=gic['ctlr']|(0x80000000 if busy else 0)
        elif address==0x03400004:data=gic['typer']
        else:
            bank=(address-0x03400100)//4
            assert address==0x03400100+4*bank and 1<=bank<9,hex(address)
            data=gic['spi'][bank]
        uc.mem_write(address,struct.pack('<I',data))
    def write(uc,access,address,size,value,unused):
        assert not (0x07050200<=address<0x07050240 or 0x07053000<=address<0x07054000),'No secondary power writes permitted.'
        if 0x08002000<=address<0x08003000:
            assert size==4 and address in (0x08002000,0x08002004,0x08002008)
            state=struct.unpack('<Q',uc.mem_read(0x42002280,8))[0]
            assert state in (1,2) and not power_registers[0x07053008]&15
            configuration_writes.append([state,address,value]);power_registers[address]=value;return
        if 0x42005100<=address<0x42005180 and value:
            assert struct.unpack('<Q',uc.mem_read(0x42002270,8))[0]==1,'No armed mailbox before stable OFF admission.'
        if 0x03400000<=address<0x03401000:
            assert size==4 and struct.unpack('<Q',uc.mem_read(0x42002100,8))[0] in (1,2)
            assert original_ctlr&0x30==0x30
            if address==0x03400000:
                assert value&0x30==0x30 and not value&0x40 and not gic['rwp']
                if value&7:assert gic['spi'][1:9]==(original_spi[1:9] if value==original_ctlr else [0]*8)
                gic['ctlr']=0x31 if scenario=='native-refusal' and uc.reg_read(UC_ARM64_REG_PC)==0x41000428 else value
                gic['rwp']=2
            elif 0x03400184<=address<=0x034001a0:
                bank=(address-0x03400180)//4;assert not gic['ctlr']&7
                gic['spi'][bank]&=~value;gic['rwp']=2
            elif 0x03400104<=address<=0x03400120:
                bank=(address-0x03400100)//4;assert not gic['ctlr']&7 and not gic['rwp']
                gic['spi'][bank]|=value
            else:raise AssertionError(hex(address))
            writes.append([address,size,value]);return
        assert any(begin<=address and address+size<=begin+length for _,begin,length in host.DRAM) or address in (0x08001004,0x08001008),(name,hex(address),size)
        if address<0x41000000:writes.append([address,size,value])
    cpu.hook_add(UC_HOOK_CODE,instruction);cpu.hook_add(UC_HOOK_MEM_READ,read);cpu.hook_add(UC_HOOK_MEM_WRITE,write)
    cpu.emu_start(0x41000000,0x41021000,count=1500000)
    snapshot=host.gic_snapshot(bytes(cpu.mem_read(0x42002100,512)))
    configuration=snapshot['configuration']
    if scenario in ('changed-power','cleanup-read-fault'):
        assert stopped==['park'] and configuration['state']==(4 if scenario=='changed-power' else 2)
        assert not any(key=='rmr_el3' for key,value in controls)
        assert len(configuration_writes)==3 and all(item[0]==1 for item in configuration_writes)
        assert snapshot['state']==1,'GIC groups must not be restored before configuration cleanup.'
        if scenario=='changed-power':assert configuration['error']==1 and configuration['restore_status']==8
        else:assert len(injected)==1
        reports.append({'name':name,'cleanup_retained':True,'gic':snapshot,'configuration_writes':configuration_writes,'system_writes':controls});return
    assert stopped==['return'],(name,stopped,hex(cpu.reg_read(UC_ARM64_REG_PC)))
    result=host.decode(bytes(cpu.mem_read(host.REPORT,256)),nonce)
    secondary=host.secondary_snapshot(bytes(cpu.mem_read(0x4200eb48,128)),bytes(cpu.mem_read(0x4200eec8,248)),bytes(cpu.mem_read(0x42005100,256)))
    assert result['restore_state']==3 and result['saved_sctlr1']==result['restored_sctlr1']==0xc50838
    assert result['saved_tcr1']==result['restored_tcr1']==0 and result['checkpoint_tcr1']==0x800080
    assert [item for item in controls if item[0] in ('sctlr_el1','tcr_el1')]==[
        ['tcr_el1',0x800080],['sctlr_el1',0xc50839],['sctlr_el1',0xc50838],['tcr_el1',0]]
    assert gic['ctlr']==original_ctlr and gic['spi']==original_spi and not gic['rwp']
    assert snapshot['state']==3 and not snapshot['error']
    assert secondary['power']['initialized']==1 and secondary['owner']['initialized']==1
    assert secondary['power']['core']==secondary['owner']['core']==1
    assert snapshot['prearm']['state']==1 and len(events)==1
    assert configuration['state']==3 and configuration['error']==0
    assert [configuration['original_arch'],configuration['original_low'],configuration['original_high']]==[0x20,0x48000000,0]
    assert [configuration['restored_arch'],configuration['restored_low'],configuration['restored_high']]==[0x20,0x48000000,0]
    assert [power_registers[address] for address in (0x08002000,0x08002004,0x08002008)]==[0x20,0x48000000,0]
    native=[item[1:] for item in configuration_writes if item[0]==1]
    restored=[item[1:] for item in configuration_writes if item[0]==2]
    assert restored==[[0x08002004,0x48000000],[0x08002008,0],[0x08002000,0x20]]
    if scenario=='configuration-fault':
        assert result['checkpoint']==5 and injected and configuration['permitted_attempts']==2
        assert native==[[0x08002004,0x41002000]]
    elif scenario=='native-readback-refusal':
        assert result['checkpoint']==3 and result['status']==2 and result['detail']==33 and injected
        assert configuration['permitted_attempts']==2 and native==[[0x08002004,0x41002000],[0x08002008,0]]
    else:
        assert result['checkpoint']==9 and result['proposed_power_address']==0x07050224 and result['proposed_power_value']==0x41
        assert native==[[0x08002004,0x41002000],[0x08002008,0],[0x08002000,0x21]]
        assert configuration['permitted_attempts']==3 and barriers
        assert secondary['power']['step']==11 and secondary['power']['writes_attempted']==4
    if events:
        assert secondary['command']['magic']==0x53435045 and secondary['command']['core']==1 and secondary['command']['state']==1
        assert secondary['command']['epoch']==bundle['epoch'] and secondary['command']['inverse_epoch']==bundle['epoch']^((1<<64)-1)
        assert bytes(cpu.mem_read(0x42005180,128))==bytes(128)
    assert snapshot['state']==3 and snapshot['error']==0 and snapshot['banks']==9 and snapshot['original_ctlr']==0x37
    assert snapshot['original_spi'][1:9]==snapshot['restored_spi'][1:9]==original_spi[1:9]
    assert snapshot['terminal_ctlr']==0x33
    assert len(publication)==3 and all(item['lines'] for item in publication)
    reports.append({'name':name,'result':result,'gic':snapshot,'secondary':secondary,'events':events,'power_reads':power_reads,'system_writes':controls,'writes':writes,'configuration_writes':configuration_writes,'barriers':barriers})
check('native-three-configuration-writes-before-power-request')
check('native-entry-readback-refusal-restores-configuration','native-readback-refusal')
check('partial-configuration-write-exception-restores-off-core','configuration-fault')
check('changed-power-retains-before-configuration-restore','changed-power')
check('configuration-cleanup-read-fault-retains-without-retry','cleanup-read-fault')
a.output.write_text(json.dumps({'schema':'a733-native-startup-software-v1','physical_access':False,'instruction_cases':True,
    'manifest_sha256':host.digest(a.build/'manifest.json'),'check_sha256':host.digest(Path(__file__)),'cases':reports,
    'limits':'Actual native configuration sequence with modeled system registers, counter, power registers and GIC. Cannot establish physical OFF stability, external exclusivity, reset-entry semantics or secondary operation.'},indent=2)+'\n')
print('PASS five OFF-secondary configuration cases; no power request executes and uncertain cleanup retains.')
