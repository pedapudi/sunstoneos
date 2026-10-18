#!/usr/bin/env python3
"""Execute production local interrupt initialization and returning cleanup in a model."""
import argparse
import atexit
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
p.add_argument('--case',help='Run only the named focused case; preserve other completed evidence separately.')
a=p.parse_args();spec=importlib.util.spec_from_file_location('startup_host',ROOT/'tools/run-native-startup-probe.py');host=importlib.util.module_from_spec(spec);spec.loader.exec_module(host)
m=json.loads((a.build/'manifest.json').read_bytes());bundle=json.loads((a.bundle/'prepared.json').read_bytes())
assert bundle['image_sha256']==host.NATIVE and bundle['launcher_sha256']==host.LAUNCH
before_power=m.get('stop_before_power',False)
after_interface=m.get('stop_after_cpu_interface',False)
before_wake=m.get('stop_before_wake',False)
after_wake=m.get('stop_after_wake',False)
profile=host.execution_profile(before_power,after_interface,before_wake,after_wake)
assert m.get('execution_profile')==profile
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
# Preserve completed cases if a later focused case fails.
atexit.register(lambda:a.output.with_suffix('.partial.json').write_text(json.dumps(reports,indent=2)+'\n'))
def check(name,scenario='normal'):
    if a.case and name!=a.case:return
    cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    for address,size in [(0x41000000,0x1200000),(0x02050000,0x1000),(0x03400000,0x1000),(0x03460000,0x100000),(0x07050000,0x5000),(0x08001000,0x2000)]:cpu.mem_map(address,size)
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
    power_registers={0x07053008:0x100,0x07050204:0x10000,0x07050224:0x40,0x08002000:0x20,0x08002004:0x48000000,0x08002008:0}
    writes=[];controls=[];ticks=[];stopped=[];injected=[];publication=[];cleaned=[];power_reads=[];events=[];configuration_writes=[];barriers=[];power_writes=[];secondary_started=[];secondary_terminal=[];active=[0];per_core={0:registers,1:dict(registers,mpidr_el1=0x81000100,tpidr_el3=0,s3_0_c15_c2_7=0,icc_sre_el3=0,icc_igrpen0_el1=0,icc_igrpen1_el3=0)}
    if scenario in ('target-admission','capture-fault'):per_core[1]['sctlr_el1']=0xc51838
    if scenario=='interface-group-enabled':per_core[1]['icc_igrpen1_el3']=2
    capture_entries=[]
    frame_cores=list(range(8));rbase=0x03480000;gbase=rbase+0x10000
    original_rdist={'ctlr':0,'waker':0 if scenario=='waker-refusal' else 6,'enable':0,'pending':1 if scenario=='pending-refusal' else 0,
        'active':0,'group':0x1122aabb,'modifier':0x00550000,'config':0xaaaaaaaa,'priority':0x12345678}
    rdist=original_rdist.copy();rdist['rwp']=0;rdist_writes=[];typer_reads=[];commands=[];private_wait_seen=[];sleep_attempted=[]
    local_registers={rbase:rdist['ctlr'],rbase+0x14:rdist['waker'],gbase+0x100:rdist['enable'],
        gbase+0x200:rdist['pending'],gbase+0x300:rdist['active'],gbase+0x80:rdist['group'],
        gbase+0xd00:rdist['modifier'],gbase+0xc04:rdist['config'],gbase+0x400:rdist['priority']}
    frame_power=0x101 if scenario=='frame-power-off' else 0x104 if scenario=='frame-power-transition' else 0x100
    expected_frame_reads=[rbase+8,rbase+0x24]+([] if frame_power!=0x100 else list(local_registers)+[rbase+0x24])
    ranges=((0x41000000,0x41200000),(0x41200000,0x41290d10),(0x421f0000,0x421f5000))
    def instruction(uc,address,size,unused):
        assert address!=0x41004444,'CPU/DSU write guard must not be crossed.'
        registers=per_core[active[0]]
        activation_state=struct.unpack('<Q',uc.mem_read(0x42002400,8))[0]
        rdist_state=struct.unpack('<Q',uc.mem_read(0x42002500,8))[0]
        if active[0]==1 and address==labels['secondary_wait_request'] and rdist_state==0:private_wait_seen.append(address)
        observation_state=struct.unpack('<Q',uc.mem_read(0x42002700,8))[0]
        if address==labels['observe_prewrite']:capture_entries.append(active[0])
        capture_fault=(scenario=='capture-fault' and active[0]==1 and address==labels['observe_requests'] and observation_state==1 or
            scenario=='frame-read-fault' and active[0]==1 and address==labels['target_frame_read'])
        if active[0]==1 and address==labels['target_block']:
            for register in range(20,26):uc.reg_write(UC_ARM64_REG_X0+register,0xdead0000+register)
        if capture_fault and not injected:
            registers.update(elr_el3=address,far_el3=0x07050204);injected.append(True)
            uc.reg_write(UC_ARM64_REG_PC,0x41008a00);return
        assert address!=0x41000480 or not secondary_started,'No second CPU loop iteration.'
        assert address not in (0x41005094,0x410050ac,0x4100822c,0x41004760),'No native cache/SCR stage.'
        if address==0x4100a2d0:barriers.append(address)
        if address==0x41004558:
            begin,end=ranges[len(publication)]
            assert uc.reg_read(UC_ARM64_REG_X0)==begin and uc.reg_read(UC_ARM64_REG_X0+1)==end-begin
            publication.append({'begin':begin,'end':end,'lines':0});cleaned.clear()
        if address==0x4100460c:
            item=publication[-1];assert cleaned==list(range(item['begin'],item['end'],64));item['lines']=len(cleaned)
        text=ops.get(address,'')
        if text=='sev':
            assert struct.unpack('<Q',uc.mem_read(0x42002270,8))[0]==1
            assert struct.unpack('<Q',uc.mem_read(0x42005118,8))[0] in (1,2)
            events.append([active[0],address])
        if re.match(r'dc\s+cvac,\s*x0$',text,re.I):
            assert address==0x410045e0;cleaned.append(uc.reg_read(UC_ARM64_REG_X0));uc.reg_write(UC_ARM64_REG_PC,address+4);return
        assert not re.match(r'(dc|ic|tlbi)\s',text,re.I),text
        read=re.match(r'mrs\s+x(\d+),\s*(\S+)',text,re.I)
        if read:
            key=read[2].lower()
            if key=='cntpct_el0':
                cleanup_state=struct.unpack('<Q',uc.mem_read(0x42002100,8))[0]
                activation_state=struct.unpack('<Q',uc.mem_read(0x42002400,8))[0]
                rdist_state=struct.unpack('<Q',uc.mem_read(0x42002500,8))[0]
                # Any parked secondary without a shutdown ACK leaves only the
                # primary deadline running. Advance that modeled wait by state,
                # so adding a refusal case does not need another scenario list.
                ack=struct.unpack('<Q',uc.mem_read(0x42002418,8))[0]
                retained_wait=secondary_terminal and not ack
                step=500000 if (retained_wait or scenario=='wake-timeout' and rdist_writes or
                    scenario=='sleep-timeout' and sleep_attempted or
                    secondary_terminal and activation_state==2 and rdist_state in (4,6)) else 1
                value=(ticks[-1] if ticks else 200)+step;ticks.append(value)
            else:assert key in registers,(name,key,hex(address));value=registers[key]
            uc.reg_write(UC_ARM64_REG_X0+int(read[1]),value);uc.reg_write(UC_ARM64_REG_PC,address+4)
        elif text.lower().startswith('msr'):
            if 'DAIFSet' in text or 'SPSel' in text:uc.reg_write(UC_ARM64_REG_PC,address+4);return
            write=re.match(r'msr\s+(\S+),\s*(x\d+|xzr)',text,re.I);assert write,(name,text)
            key=write[1].rstrip(',').lower();value=0 if write[2]=='xzr' else uc.reg_read(UC_ARM64_REG_X0+int(write[2][1:]));controls.append([active[0],key,value])
            assert key in ('vbar_el3','cntfrq_el0','cntps_ctl_el1','cntp_ctl_el0','cntv_ctl_el0','rmr_el3','sctlr_el1','tcr_el1','tpidr_el3','s3_0_c15_c2_7','icc_sre_el3','icc_igrpen0_el1','icc_igrpen1_el3'),(name,key)
            if key=='rmr_el3':
                assert active[0]==0,'Secondary must never execute RMR.'
                assert value==2 and writes[-2:]==[[0x08001004,4,0x20],[0x08001008,4,0]];stopped.append('return');uc.emu_stop()
            else:
                if scenario=='target-restore-fault' and active[0]==1 and key=='sctlr_el1' and value==0xc50838:
                    injected.append(True)  # Model a failed restoration readback.
                elif scenario=='interface-sre-refusal' and active[0]==1 and key=='icc_sre_el3':injected.append(True)
                else:registers[key]=value
                uc.reg_write(UC_ARM64_REG_PC,address+4)
        elif text=='wfi':
            if active[0]:
                secondary_terminal.append(address)
                if address==labels['secondary_terminal']+4 and scenario!='ack-without-off':power_registers[0x07053008]=0x100
            else:stopped.append('park')
            uc.emu_stop()
    def read(uc,access,address,size,value,unused):
        if 0x03460000<=address<0x03560000:
            assert active[0]==1
            assert per_core[1]['sctlr_el1']==0xc50839 and per_core[1]['tcr_el1']==0x800080
            init=host.local_initialization_snapshot(bytes(uc.mem_read(0x42002980,256)))
            if (address-0x03460008)%0x20000==0:
                assert size==8 and 0x03460008<=address<=0x03540008
                index=(address-0x03460008)//0x20000
                data=(index<<40)|0x121|(16 if index==7 else 0)
            elif address==rbase+0x24:
                assert size==4
                data=frame_power
                if scenario=='power-reject' and len([w for w in rdist_writes if w[0]==rbase+0x24])==1:data|=1
            else:
                assert size==4 and address in local_registers,hex(address)
                assert frame_power==0x100
                keys={rbase:'ctlr',rbase+0x14:'waker',gbase+0x100:'enable',gbase+0x200:'pending',
                    gbase+0x300:'active',gbase+0x80:'group',gbase+0xd00:'modifier',gbase+0xc04:'config',gbase+0x400:'priority'}
                data=rdist[keys[address]]
                if address==rbase and rdist['rwp']:data|=8;rdist['rwp']-=1
                if address==gbase+0x80 and (scenario=='cleanup-fault' and init['state']==2 or scenario=='prepower-baseline-fault' and init['stopped_before_power'] or scenario=='prewake-baseline-fault' and init['stopped_before_wake']):data^=1
            uc.mem_write(address,data.to_bytes(size,'little'));typer_reads.append(address);return
        if address in power_registers:
            assert size==4;power_reads.append(address)
            data=power_registers[address]
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
        if address==0x42005118 and value:commands.append(value)
        if 0x03460000<=address<0x03560000:
            assert not before_power,'Early diagnostic cannot write a redistributor.'
            if before_wake:assert address==rbase+0x24,'Power-only diagnostic cannot write WAKER or SGI/PPI state.'
            if after_wake:assert address in (rbase+0x24,rbase+0x14),'Wake/sleep diagnostic cannot write SGI/PPI state.'
            assert size==4 and active[0]==1
            assert struct.unpack('<Q',uc.mem_read(0x42002900,8))[0]==2
            assert not (per_core[1]['icc_igrpen0_el1']&1 or per_core[1]['icc_igrpen1_el3']&3)
            assert per_core[1]['icc_sre_el3']&7==7
            rdist_writes.append([address,value])
            if address==rbase+0x24:assert value==0
            elif address==rbase+0x14:
                assert value in (0,2,4,6)  # ChildrenAsleep is read-only.
                if value&2:
                    rdist['waker']=(value&~4) if scenario=='sleep-timeout' else value|4
                    sleep_attempted.append(address)
                else:rdist['waker']=4 if scenario=='wake-timeout' else 0
            elif address==gbase+0x180:rdist['enable']&=~value;rdist['rwp']=2
            elif address==gbase+0x100:rdist['enable']|=value;rdist['rwp']=2
            elif address==gbase+0x280:rdist['pending']&=~value
            elif address==gbase+0x380:rdist['active']&=~value
            else:
                keys={gbase+0x80:'group',gbase+0xd00:'modifier',gbase+0xc04:'config',gbase+0x400:'priority'}
                assert address in keys,hex(address)
                rdist[keys[address]]=value
            return
        assert not (0x07053000<=address<0x07054000),'PPU status is read-only.'
        if address in (0x07050204,0x07050224):
            assert size==4
            activation_state=struct.unpack('<Q',uc.mem_read(0x42002400,8))[0]
            assert activation_state in (1,2,4)
            if activation_state==4:assert not power_registers[0x07053008]&15
            power_writes.append([active[0],activation_state,address,value]);power_registers[address]=value
            if active[0]==0 and activation_state==1 and address==0x07050204 and value&1:
                assert power_registers[0x07050224]&1
                power_registers[0x07053008]=0x108;secondary_started.append(True)
            return
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
    cpu.reg_write(UC_ARM64_REG_PC,0x41000000);contexts={0:cpu.context_save()}
    instruction_budget=0
    # A parked primary does not stop the secondary. Let that core reach its
    # own retained terminal state before asserting timeout cleanup results.
    while not stopped or (stopped==['park'] and secondary_started and not secondary_terminal):
        for core in (0,1):
            if core==0 and stopped:continue
            if core==1:
                if not secondary_started  or secondary_terminal:continue
                if core not in contexts:
                    cpu.reg_write(UC_ARM64_REG_SP,0);cpu.reg_write(UC_ARM64_REG_PC,0x41002000);contexts[core]=cpu.context_save()
            active[0]=core;cpu.context_restore(contexts[core])
            cpu.emu_start(cpu.reg_read(UC_ARM64_REG_PC),0,count=500)
            contexts[core]=cpu.context_save();instruction_budget+=500
            assert instruction_budget<3000000,(name,'instruction budget',core,hex(cpu.reg_read(UC_ARM64_REG_PC)),host.rdist_snapshot(bytes(cpu.mem_read(0x42002500,512))),host.activation_snapshot(bytes(cpu.mem_read(0x42002400,256))))
            if stopped==['return']:break
    active[0]=0;cpu.context_restore(contexts[0])
    snapshot=host.gic_snapshot(bytes(cpu.mem_read(0x42002100,512)))
    configuration=snapshot['configuration']
    activation=host.activation_snapshot(bytes(cpu.mem_read(0x42002400,256)))
    rdist_record=host.rdist_snapshot(bytes(cpu.mem_read(0x42002500,512)))
    observation=host.observation_snapshot(bytes(cpu.mem_read(0x42002700,512)))
    initialization=host.local_initialization_snapshot(bytes(cpu.mem_read(0x42002980,256)))
    a.output.with_suffix('.last-observation.json').write_text(json.dumps({
        'case':name,'assertions_complete':False,'stopped':stopped,
        'initialization':initialization,'activation':activation,
        'rdist_writes':rdist_writes,'secondary_terminal':secondary_terminal},indent=2)+'\n')
    if scenario in ('interface-group-enabled','interface-sre-refusal'):
        assert after_interface and stopped==['park'] and observation['state']==1 and not activation['ack']
        assert initialization['state']==1 and not initialization['cpu_interface_observed']
        assert not rdist_writes and typer_reads==expected_frame_reads
        assert not any(key=='rmr_el3' for core,key,value in controls)
        if scenario=='interface-group-enabled':assert initialization['igrpen1']==2
        else:assert initialization['sre_after']==0 and injected
        reports.append({'name':name,'retained':True,'initialization':initialization,'activation':activation});return
    if scenario in ('prepower-baseline-fault','prewake-baseline-fault'):
        assert stopped==['park'] and observation['state']==1 and not activation['ack']
        assert initialization['state']==1
        if before_power:assert initialization['stopped_before_power']==1 and not rdist_writes
        else:assert before_wake and initialization['stopped_before_wake']==1 and rdist_writes==[[rbase+0x24,0]]
        assert initialization['controller_state']==5 and initialization['error']==1
        assert initialization['group']!=original_rdist['group'] and not initialization['cleanup_writes']
        reports.append({'name':name,'retained':True,'initialization':initialization,'activation':activation});return
    if scenario in ('cleanup-fault','wake-timeout','sleep-timeout'):
        assert stopped==['park'] and observation['state']==1 and not activation['ack']
        assert initialization['state'] in (1,2) and rdist_writes
        if scenario=='cleanup-fault':assert initialization['cleanup_error']==4 and initialization['controller_state']==2
        elif scenario=='sleep-timeout':
            assert after_wake and initialization['cleanup_error']==5 and initialization['stopped_after_wake']==1
            assert initialization['awake_waker']==0 and initialization['cleanup_writes']==1
            assert initialization['controller_state']==5 and initialization['phase']==10
        else:assert initialization['controller_state']==5 and initialization['error']==2
        assert not any(key=='rmr_el3' for core,key,value in controls)
        reports.append({'name':name,'retained':True,'initialization':initialization,'rdist_writes':rdist_writes,'activation':activation});return
    if scenario in ('capture-fault','frame-read-fault','target-restore-fault'):
        assert stopped==['park'] and observation['state']==1 and observation['origin']==(1 if scenario=='capture-fault' else 3)
        assert capture_entries==[1] and len(injected)==1 and activation['state']==1
        assert configuration['state']==snapshot['state']==rdist_record['state']==1
        assert (not typer_reads if scenario!='target-restore-fault' else typer_reads==expected_frame_reads)
        assert not rdist_writes and commands==[1,2] and not activation['request'] and not activation['ack']
        assert not any(key in ('rmr_el3','s3_0_c15_c2_7') for core,key,value in controls)
        reports.append({'name':name,'retained':True,'observation':observation,'activation':activation,'redistributor':rdist_record,'capture_entries':capture_entries,'system_writes':controls});return
    assert stopped==['return'],(name,hex(cpu.reg_read(UC_ARM64_REG_PC)),activation,snapshot['configuration'],bytes(cpu.mem_read(host.REPORT,48)).hex())
    result=host.decode(bytes(cpu.mem_read(host.REPORT,256)),nonce)
    secondary=host.secondary_snapshot(bytes(cpu.mem_read(0x4200eb48,128)),bytes(cpu.mem_read(0x4200eec8,248)),bytes(cpu.mem_read(0x42005100,256)))
    assert result['restore_state']==3 and result['saved_sctlr1']==result['restored_sctlr1']==0xc50838
    assert result['saved_tcr1']==result['restored_tcr1']==0 and result['checkpoint_tcr1']==0x800080
    assert [[key,value] for core,key,value in controls if core==0 and key in ('sctlr_el1','tcr_el1')]==[
        ['tcr_el1',0x800080],['sctlr_el1',0xc50839],['sctlr_el1',0xc50838],['tcr_el1',0]]
    assert gic['ctlr']==original_ctlr and gic['spi']==original_spi and not gic['rwp']
    assert snapshot['state']==3 and not snapshot['error']
    assert secondary['power']['initialized']==1 and secondary['owner']['initialized']==1
    assert secondary['power']['core']==secondary['owner']['core']==1
    assert snapshot['prearm']['state']==1
    assert configuration['state']==3 and configuration['error']==0 and configuration['permitted_attempts']==7
    assert [configuration['original_arch'],configuration['original_low'],configuration['original_high']]==[0x20,0x48000000,0]
    assert [configuration['restored_arch'],configuration['restored_low'],configuration['restored_high']]==[0x20,0x48000000,0]
    assert [power_registers[address] for address in (0x08002000,0x08002004,0x08002008)]==[0x20,0x48000000,0]
    assert result['checkpoint']==13 and activation['state']==5 and observation['state']==5
    assert activation['epoch']==activation['request']==activation['ack']==observation['epoch']==bundle['epoch']
    assert not rdist_record['writes_attempted'] and not rdist_record['reason']
    assert activation['last_ppu']&15==0 and activation['restored_control']==0x10000 and activation['restored_mode']==0x40
    assert capture_entries==[1]
    assert observation['CurrentEL']==12 and observation['SCTLR_EL3']==0xc50838 and observation['MIDR_EL1']==0x412fd050
    assert observation['control']==0x10000 and observation['mode']==0x40
    assert [power_registers[a] for a in (0x07050204,0x07050224)]==[0x10000,0x40]
    assert secondary['power']['state']==2 and secondary['power']['step']==18 and secondary['power']['writes_attempted']==7
    expected_power=[[0,1,0x07050224,0x41],[0,1,0x07050204,0x10001],[0,1,0x07050204,0x10000],[0,1,0x07050224,0x40],
        [1,2,0x07050224,0x40],[1,2,0x07050204,0x10002],[0,4,0x07050204,0x10000],[0,4,0x07050224,0x40]]
    if initialization['state']==3 or any(initialization[k] for k in ('stopped_before_power','stopped_before_wake','stopped_after_wake')):expected_power[4:4]=[[1,1,0x07050204,0x10000],[1,1,0x07050204,0x10000]]
    assert power_writes==expected_power
    assert observation['core']==1 and observation['MPIDR_EL1']==0x81000100
    assert observation['gicr_state']==rdist_record['state']==1
    assert commands==[1,2] and secondary['owner']['state']==2 and secondary['owner']['phase']==5
    assert secondary['report']['state']==2 and not injected
    target_controls=[[key,value] for core,key,value in controls if core==1 and key in ('sctlr_el1','tcr_el1')]
    if scenario=='target-admission':
        assert not target_controls and not typer_reads
        assert observation['origin']==1 and observation['SCTLR_EL1']==0xc51838
        assert observation['controller']['state']==5 and observation['controller']['index']==2 and observation['controller']['error']==3
        assert observation['controller']['core_record']['state']==6
    else:
        assert target_controls==[['tcr_el1',0x800080],['sctlr_el1',0xc50839],['sctlr_el1',0xc50838],['tcr_el1',0]]
        assert typer_reads[:len(expected_frame_reads)]==expected_frame_reads
        assert observation['walks']==dict(blocked_sctlr1=0xc50839,blocked_tcr1=0x800080,
            typer=(0x100<<32)|0x121,pwrr=frame_power,restored_sctlr1=0xc50838,restored_tcr1=0,state=2)
        local=host.local_interrupt_snapshot(bytes(cpu.mem_read(0x42002900,96)))
        assert local['base']==rbase and local['state']==(2 if frame_power==0x100 else 3)
        if frame_power==0x100:
            assert all(local[k]==v for k,v in original_rdist.items())
            assert local['pwrr_after']==frame_power
        else:assert not any(v for k,v in local.items() if k not in ('base','state'))
        assert observation['origin']==3 and observation['SCTLR_EL1']==0xc50838 and observation['TCR_EL1']==0
        assert observation['controller']['state']==4 and observation['controller']['error']==0
        assert observation['controller']['core']==1 and observation['controller']['identity']['mpidr']==0x81000100
        assert observation['controller']['core_record']['state']==1 and observation['controller']['core_record']['epoch']==bundle['epoch']
        assert not any(observation['controller']['plan'][key] for key in ('actlr','actlr2','actlr3','ectlr','clusteractlr','patch'))
    if scenario=='pending-refusal':
        assert initialization['state']==4 and not rdist_writes
    elif after_interface:
        assert initialization['state']==4 and initialization['cpu_interface_observed']==1
        assert initialization['epoch']==bundle['epoch'] and initialization['core']==1
        assert initialization['sre_after']==7 and initialization['igrpen0']==initialization['igrpen1']==0
        assert not any(initialization[k] for k in ('controller_state','phase','error','writes','cleanup_error',
                                                  'cleanup_writes','stopped_before_power'))
        assert typer_reads==expected_frame_reads and not rdist_writes and not sleep_attempted
    elif before_power or before_wake or after_wake:
        assert initialization['state']==4
        if before_power:
            assert initialization['stopped_before_power']==1 and not rdist_writes and initialization['writes']==2
        elif before_wake:
            assert initialization['stopped_before_wake']==1 and initialization['phase']==8
            assert not initialization['stopped_before_power'] and not initialization['cpu_interface_observed']
            assert initialization['writes']==(4 if scenario=='power-reject' else 3)
            assert rdist_writes==[[rbase+0x24,0]]*(2 if scenario=='power-reject' else 1)
        else:
            assert initialization['stopped_after_wake']==1 and initialization['phase']==10
            assert not any(initialization[k] for k in ('stopped_before_power','stopped_before_wake','cpu_interface_observed','awake_waker'))
            assert initialization['writes']==4
            assert rdist_writes==[[rbase+0x24,0],[rbase+0x14,4],[rbase+0x14,6]]
        assert initialization['controller_state']==5 and initialization['error']==1
        assert initialization['cleanup_writes']==(2 if after_wake else 1) and initialization['cleanup_error']==0
        assert initialization['epoch']==bundle['epoch'] and initialization['core']==1
        assert initialization['redistributor']==rbase and initialization['sre_after']==7
        assert initialization['pwrr']==frame_power and initialization['restored_control']==0x10000
        assert all(initialization[k]==original_rdist[k] for k in ('waker','enable','pending','active','group','modifier','config','priority'))
        assert bool(sleep_attempted)==after_wake
    elif scenario in ('normal','power-reject'):
        assert initialization['state']==3 and initialization['controller_state']==2 and not initialization['error']
        assert initialization['writes']==(12 if scenario=='power-reject' else 11)
        assert initialization['cleanup_writes']==7 and initialization['cleanup_error']==0
        assert initialization['epoch']==bundle['epoch'] and initialization['core']==1
        assert initialization['redistributor']==rbase and initialization['sre_after']==7
        assert initialization['pwrr']==frame_power and initialization['restored_control']==0x10000
        assert all(initialization[k]==original_rdist[k] for k in ('waker','enable','pending','active','group','modifier','config','priority'))
        assert [w for w in rdist_writes if w[0]==rbase+0x24]==[[rbase+0x24,0]]*(2 if scenario=='power-reject' else 1)
        assert sleep_attempted
    assert all(rdist[n]==original_rdist[n] for n in original_rdist)
    assert snapshot['state']==3 and snapshot['error']==0 and snapshot['banks']==9 and snapshot['original_ctlr']==0x37
    assert snapshot['original_spi'][1:9]==snapshot['restored_spi'][1:9]==original_spi[1:9]
    assert snapshot['terminal_ctlr']==0x33
    assert len(publication)==3 and all(item['lines'] for item in publication)
    reports.append({'name':name,'result':result,'gic':snapshot,'secondary':secondary,'events':events,'power_reads':power_reads,'system_writes':controls,'writes':writes,'configuration_writes':configuration_writes,'barriers':barriers,'activation':activation,'power_writes':power_writes,'secondary_terminal':secondary_terminal,'redistributor':rdist_record,'rdist_writes':rdist_writes,'typer_reads':typer_reads,'commands':commands,'observation':observation,'capture_entries':capture_entries,'initialization':initialization})
if after_interface:
    check('cpu-interface-observed-without-redistributor-writes')
    check('enabled-interface-group-retains-without-shutdown','interface-group-enabled')
    check('sre-readback-refusal-retains-without-shutdown','interface-sre-refusal')
elif before_power:
    check('pre-power-boundary-and-unchanged-local-state')
    check('pre-power-state-mismatch-retains-without-shutdown','prepower-baseline-fault')
elif before_wake:
    check('power-handshake-and-asleep-state-restored')
    check('power-handshake-retries-rejected-request','power-reject')
    check('pre-wake-state-mismatch-retains-without-shutdown','prewake-baseline-fault')
elif after_wake:
    check('wake-then-sleep-without-local-configuration')
    check('wake-timeout-retains-without-shutdown','wake-timeout')
    check('sleep-timeout-retains-without-shutdown','sleep-timeout')
else:
    check('current-controller-ready-and-local-state-restored')
    check('current-controller-retries-one-rejected-power-request','power-reject')
    check('local-initialization-refuses-pending-interrupt','pending-refusal')
    check('local-cleanup-mismatch-retains-without-shutdown','cleanup-fault')
    check('local-wake-timeout-retains-without-shutdown','wake-timeout')
a.output.write_text(json.dumps({'schema':'a733-native-startup-software-v1','physical_access':False,'execution_profile':profile,'instruction_cases':True,
    'manifest_sha256':host.digest(a.build/'manifest.json'),'check_sha256':host.digest(Path(__file__)),'cases':reports,
    'limits':'Actual RELEASE and selected diagnostic boundary with modeled system registers, GIC and PPU. Interface-only mode omits the local controller. Does not identify physical failures, qualify fault delivery or recover stalled MMIO.'},indent=2)+'\n')
print(f'PASS {len(reports)} focused local initialization cases; observed OFF or retained failure.')
