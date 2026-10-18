#!/usr/bin/env python3
"""Run current core1 local interrupt initialization, restore state, then verify OFF."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import secrets
import sqlite3
import struct
import subprocess
import sys
import time
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
prior=module('startup_prior',ROOT/'tools/run-a64-entry-probe.py')
base=prior.base
supervised=module('startup_supervised',ROOT/'tools/check-a733-smp-supervised.py')
require,digest,save=base.require,base.digest,base.save_json
NATIVE=prior.NATIVE
LAUNCH='2b0641c2509963ad08d03e8498ee865b5d858c4536b9ea26fbd26ca576e698ea'
MAGIC=0x4137335354415254
OVERLAY=(0x41020000,0x4000)
PROFILE='core1-local-interrupt-init-report-v2'
EARLY_PROFILE='core1-before-redistributor-write-v1'
INTERFACE_PROFILE='core1-cpu-interface-only-v1'
POWER_PROFILE='core1-before-redistributor-wake-v1'
WAKE_PROFILE='core1-redistributor-wake-sleep-v1'
CACHE_PROFILE='core1-el3-cache-restore-v1'
EL1_CACHE_PROFILE='core1-el1-cache-restore-v2'

def execution_profile(before_power,after_interface,before_wake=False,after_wake=False,el3_cache=False,el1_cache=False):
    require(sum((before_power,after_interface,before_wake,after_wake,el3_cache,el1_cache))<=1,'Choose one diagnostic boundary.')
    if el1_cache:return EL1_CACHE_PROFILE
    if el3_cache:return CACHE_PROFILE
    if after_wake:return WAKE_PROFILE
    return POWER_PROFILE if before_wake else INTERFACE_PROFILE if after_interface else EARLY_PROFILE if before_power else PROFILE
def manifest_profile(m):
    require(type(m.get('stop_before_power')) is bool,'Missing diagnostic boundary.')
    require(type(m.get('stop_after_cpu_interface',False)) is bool,'Invalid CPU-interface boundary.')
    require(type(m.get('stop_before_wake',False)) is bool,'Invalid redistributor wake boundary.')
    require(type(m.get('stop_after_wake',False)) is bool,'Invalid redistributor wake/sleep boundary.')
    require(type(m.get('el3_cache',False)) is bool,'Invalid EL3 cache boundary.')
    require(type(m.get('el1_cache',False)) is bool,'Invalid EL1 cache boundary.')
    profile=execution_profile(m['stop_before_power'],m.get('stop_after_cpu_interface',False),m.get('stop_before_wake',False),m.get('stop_after_wake',False),m.get('el3_cache',False),m.get('el1_cache',False))
    return profile

SOURCES=('boot/a733/native-startup-probe.S','boot/a733/native-startup-probe.ld',
    'boot/a733/native-local-interrupt-probe.c','boot/a733/native-el3-cache-probe.S','boot/a733/native-el1-cache-probe.S','boot/a733/native-ns-publication.S','platform/a733/cpu_continue.c',
    'platform/a733-smp-cached/continuation-core.patch','platform/a733/cpu_start.c',
    'runtime/memory.c','runtime/include/string.h','include/a733_cpu_continue.h',
    'include/a733_cpu_entry.h','include/a733_cpu_start.h')
REPORT=0x42002000
# Includes every byte written by actual stack/BSS initialization and admission.
DRAM=(('native-code',0x41000000,0x12078),('native-data',0x41200000,0x21),
      ('el1-bss',0x41201000,0x8fd10),('result',0x42000000,0x4000),
      ('contract',0x42004000,0x1000),('monitor-bss',0x42005000,0xa590),
      ('boot-stack',0x42010000,0x4000),('overlay',*OVERLAY),
      ('tables',0x421f0000,0x7000))

def branch(source,target):
    offset=target-source;require(offset%4==0 and -(1<<27)<=offset<(1<<27),'Branch out of range.')
    return 0x14000000|((offset//4)&0x3ffffff)

def prepare(a):
    require(digest(a.observer)==NATIVE,'Only the unchanged reviewed native ELF is supported.')
    from elftools.elf.elffile import ELFFile
    with a.observer.open('rb') as f:
        elf=ELFFile(f);symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    require(tuple(symbols[n] for n in prior.SYMBOLS)==prior.LAYOUT,'Native layout changed.')
    segments,_,_=base.image_parts(a.observer)
    require(all(address+size<=OVERLAY[0] or OVERLAY[0]+OVERLAY[1]<=address
                for name,address,size in DRAM if name!='overlay'),'Overlay overlaps a native reservation.')
    a.build.mkdir(parents=True,exist_ok=False)
    profile=execution_profile(a.stop_before_power,a.stop_after_cpu_interface,a.stop_before_wake,a.stop_after_wake,a.el3_cache,a.el1_cache)
    commands=[['patch','--silent','--output',a.build/'continue-core.c',ROOT/'platform/a733/cpu_continue.c',
        ROOT/'platform/a733-smp-cached/continuation-core.patch'],
        [a.clang,'--target=aarch64-none-elf','-march=armv8-a','-c',ROOT/'boot/a733/native-startup-probe.S','-o',a.build/'overlay.o']]
    objects=[a.build/'overlay.o']
    for name,source in [('local',ROOT/'boot/a733/native-local-interrupt-probe.c'),
                        ('controller',a.build/'continue-core.c'),('topology',ROOT/'platform/a733/cpu_start.c'),
                        ('memory',ROOT/'runtime/memory.c')]:
        obj=a.build/(name+'.o');objects.append(obj)
        commands.append([a.clang,'--target=aarch64-none-elf','-march=armv8-a','-mgeneral-regs-only',
            '-O2','-ffreestanding','-fno-builtin','-fno-stack-protector','-ffunction-sections','-fdata-sections',
            '-Wall','-Wextra','-Werror',*(['-DSUNSTONE_PROBE_STOP_BEFORE_POWER=1'] if a.stop_before_power else []),
            *(['-DSUNSTONE_PROBE_STOP_AFTER_CPU_INTERFACE=1'] if a.stop_after_cpu_interface else []),
            *(['-DSUNSTONE_PROBE_STOP_BEFORE_WAKE=1'] if a.stop_before_wake else []),
            *(['-DSUNSTONE_PROBE_STOP_AFTER_WAKE=1'] if a.stop_after_wake else []),
            *(['-DSUNSTONE_PROBE_EL3_CACHE=1'] if a.el3_cache or a.el1_cache else []),
            '-I',ROOT/'include','-I',ROOT/'runtime/include','-c',source,'-o',obj])
    link_symbols=[]
    if a.el3_cache or a.el1_cache:
        obj=a.build/'el3-cache.o';objects.append(obj)
        commands.append([a.clang,'--target=aarch64-none-elf','-march=armv8-a',*(['-DSUNSTONE_PROBE_EL1_CACHE=1'] if a.el1_cache else []),'-c',ROOT/'boot/a733/native-el3-cache-probe.S','-o',obj])
        for alias,symbol in [('allow_ns_code','a733_cached_allow_ns_code'),('enable_el3','a733_cpu_coherence_enable_el3'),('el3_root','a733_cached_el3_l1')]:
            link_symbols.append('--defsym=probe_production_'+alias+'='+hex(symbols[symbol]))
    if a.el1_cache:
        obj=a.build/'el1-cache.o';objects.append(obj)
        commands.append([a.clang,'--target=aarch64-none-elf','-march=armv8-a','-c',ROOT/'boot/a733/native-el1-cache-probe.S','-o',obj])
        link_symbols.append('--defsym=probe_production_enter_el1='+hex(symbols['a733_cached_enter_el1']))
        obj=a.build/'ns-publication.o';objects.append(obj)
        commands.append([a.clang,'--target=aarch64-none-elf','-march=armv8-a','-c',ROOT/'boot/a733/native-ns-publication.S','-o',obj])
    commands.extend([[a.lld,*link_symbols,'-T',ROOT/'boot/a733/native-startup-probe.ld','--gc-sections',*objects,'-o',a.build/'overlay.elf'],
        [a.objcopy,'-O','binary',a.build/'overlay.elf',a.build/'overlay.bin']])
    for command in commands:
        r=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        with (a.build/'build.log').open('a') as f:f.write(r.stdout+r.stderr)
        require(r.returncode==0,'Overlay build failed; inspect log.')
    require(0<(a.build/'overlay.bin').stat().st_size<=OVERLAY[1],'Overlay exceeds reservation.')
    with (a.build/'overlay.elf').open('rb') as f:
        hooks={s.name:s['st_value'] for s in ELFFile(f).get_section_by_name('.symtab').iter_symbols()}
    code=bytearray(segments[0]['data']);patches=[]
    def patch(address,old,target):
        offset=address-0x41000000;require(struct.unpack_from('<I',code,offset)[0]==old,'Native instruction differs.')
        new=branch(address,target);struct.pack_into('<I',code,offset,new);patches.append([address,old,new])
    for address,old,name in [(0x410000d0,0xd102c3ff,'boot_checkpoint'),(0x4100a1e4,0xd5384248,'cached_checkpoint'),
                            (0x410034d4,0x2a1f03e0,'prearm_checkpoint'),
                            (0x410036ec,0x51000428,'release_checkpoint'),(0x410020cc,0xd503205f,'secondary_checkpoint'),
                            (0x4100052c,0x910006f7,'next_checkpoint'),(0x41004eb8,0xd5033f9f,'ready_checkpoint'),
                            (0x41005650,0xb9406408,'continue_write_checkpoint'),
                            (0x410003d0,0xb9000113,'gic_begin'),
                            (0x41008264,0xd5382050,'block_checkpoint'),
                            (0x4100a09c,0x52801fe8,'stop_checkpoint'),(0x4100009c,0xd503205f,'park_checkpoint'),
                            (0x41004440,0xd5384243,'write_checkpoint'),(0x41004d50,0xd503205f,'failed_checkpoint'),
                            (0x41004d7c,0xd503205f,'failed_checkpoint')]:
        patch(address,old,hooks[name])
    if a.el1_cache:patch(0x410081f4,0xd518cc9f,hooks['el1_observe_checkpoint'])
    for slot in range(16):
        address=0x41008800+128*slot
        require(struct.unpack_from('<I',code,address-0x41000000)[0]==0xd2800012+(slot<<5),'Vector slot marker differs.')
        patch(address+4,branch(address+4,0x410082c8),hooks['el1_return_checkpoint' if a.el1_cache and slot==8 else 'fault_checkpoint'])
    payloads=[]
    for index,segment in enumerate(segments):
        if segment['data']:
            data=bytes(code) if index==0 else segment['data'];name=f'segment-{index}.bin'
            (a.build/name).write_bytes(data);payloads.append({'name':name,'address':segment['address'],'bytes':len(data)})
    save(a.build/'commands.json',[[str(x) for x in c] for c in commands])
    save(a.build/'manifest.json',{'schema':'a733-native-startup-probe-v1','physical_access':False,'execution_profile':profile,'native_sha256':NATIVE,
        'stop_before_power':a.stop_before_power,
        'stop_after_cpu_interface':a.stop_after_cpu_interface,
        'stop_before_wake':a.stop_before_wake,
        'stop_after_wake':a.stop_after_wake,'el3_cache':a.el3_cache,'el1_cache':a.el1_cache,
        'el1_smc_return':hooks.get('el1_smc_return'),
        'el1_call_word':(branch(hooks['native_el1_call'],hooks['native_el1_cache_probe'])|0x80000000 if a.el1_cache else None),
        'source_sha256':digest(ROOT/'boot/a733/native-startup-probe.S'),'host_sha256':digest(Path(__file__)),
        'sources':{name:digest(ROOT/name) for name in SOURCES},
        'overlay_reservation':{'address':OVERLAY[0],'bytes':OVERLAY[1]},'patches':patches,'payloads':payloads,'hooks':{n:hooks[n] for n in ('boot_checkpoint','cached_checkpoint','stop_checkpoint','park_checkpoint','fault_checkpoint','write_checkpoint','failed_checkpoint','block_checkpoint','gic_begin','prearm_checkpoint','release_checkpoint','secondary_checkpoint','next_checkpoint','ready_checkpoint','continue_write_checkpoint')},
        'files':{p.name:digest(p) for p in a.build.iterdir() if p.is_file()}})

def decode(raw,nonce):
    require(len(raw)==256,'Short startup report.');v=struct.unpack('<32Q',raw)
    require(v[:3]==(MAGIC,1,256) and v[3] in range(2,14) and raw[240:]==nonce,'Startup report identity differs.')
    names=('checkpoint','entered_c','entry_sp','CurrentEL','MPIDR_EL1','SCTLR_EL3','SCTLR_EL1','CNTFRQ_EL0','counter','status','detail','vector','esr','elr','far','terminal_sp')
    result=dict(zip(names,v[3:19]))
    if v[3]==6:result.update(requested_register=v[12],requested_value=v[13])
    if v[3]==9:result.update(proposed_power_address=v[12],proposed_power_value=v[13])
    names=('saved_sctlr1','saved_tcr1','restore_state','restored_sctlr1','restored_tcr1','live_gicd_ctlr','gicd_typer','proposed_gicd_ctlr','gic_snapshot_valid','checkpoint_sctlr1','checkpoint_tcr1')
    result.update(zip(names,v[19:30]))
    return result

def controller_snapshot(raw,core_raw):
    require(len(raw)==184 and len(core_raw)==128,'Short native controller snapshot.')
    identity=dict(zip(('midr','mpidr','clidr','mmfr0','clusteridr','clustercfr'),struct.unpack_from('<6Q',raw,24)))
    plan=dict(zip(('actlr','actlr2','actlr3','ectlr','clusteractlr'),struct.unpack_from('<5Q',raw,72)))
    requirements,core,revision,patch=struct.unpack_from('<IBBB',raw,112)
    plan.update(requirements=requirements,core=core,revision=revision,patch=patch)
    state=dict(zip(('core','index','phase','error','state'),struct.unpack_from('<5I',raw,160)))
    core_names=('epoch','core','state','error','redistributor','esr','far','elr','sctlr3','ttbr3','clusteridr','startup_deadline','diagnostic_deadline')
    return {'identity':identity,'plan':plan,**state,'core_record':dict(zip(core_names,struct.unpack('<16Q',core_raw)[:13]))}

def gic_snapshot(raw):
    require(len(raw)==512,'Short GIC snapshot.')
    result=dict(zip(('state','original_ctlr','typer','banks','terminal_ctlr','cleanup_deadline','error'),struct.unpack_from('<7Q',raw)))
    result['original_spi']=list(struct.unpack_from('<32I',raw,64))
    result['restored_spi']=list(struct.unpack_from('<32I',raw,192))
    result['prearm']=dict(zip(('core','reason','status_before','control','mode','status_after','state','counter'),struct.unpack_from('<8Q',raw,320)))
    result['configuration']=dict(zip(('state','core','base','original_arch','original_low','original_high','terminal_arch','terminal_low','terminal_high',
        'restored_arch','restored_low','restored_high','error','permitted_attempts','restore_status','restore_requests'),struct.unpack_from('<16Q',raw,384)))
    return result

def observation_snapshot(raw):
    require(len(raw)==512,'Short pre-write failure snapshot.')
    names=('state','origin','epoch','core','CurrentEL','MPIDR_EL1','SCTLR_EL3','SCTLR_EL1','TCR_EL1',
           'ESR_EL3','ELR_EL3','FAR_EL3','control','mode','gicr_state','MIDR_EL1')
    return {**dict(zip(names,struct.unpack_from('<16Q',raw))),
            'controller':controller_snapshot(raw[128:312],raw[312:440]),
            'walks':dict(zip(('blocked_sctlr1','blocked_tcr1','typer','pwrr',
                'restored_sctlr1','restored_tcr1','state'),struct.unpack_from('<7Q',raw,440)))}

def rdist_snapshot(raw):
    require(len(raw)==512,'Short redistributor snapshot.')
    names=('state','reason','base','original_ctlr','original_waker','original_enable','original_pending','original_active',
           'original_group','original_modifier','original_config','original_priority','cleanup_deadline','target_sp',
           'saved_sctlr1','saved_tcr1','blocked_sctlr1','blocked_tcr1','restored_waker','restored_enable',
           'restored_group','restored_modifier','restored_config','restored_priority','original_sre','requested_sre',
           'original_igrpen0','original_igrpen1','disabled_igrpen0','disabled_igrpen1','error_address','writes_attempted')
    result=dict(zip(names,struct.unpack_from('<32Q',raw)))
    result['native_continue']={'redistributor':struct.unpack_from('<Q',raw,256+72)[0],
        **dict(zip(('core','frame','phase','state','error','initialized','entered','writes_attempted'),struct.unpack_from('<8I',raw,256+100)))}
    result['typers']=list(struct.unpack_from('<8Q',raw,392))
    result.update(zip(('esr','elr','far','requested_address','requested_value'),struct.unpack_from('<5Q',raw,456)))
    return result

def local_interrupt_snapshot(raw):
    require(len(raw)==96, 'Short local interrupt snapshot.')
    return dict(zip(('state','base','ctlr','waker','enable','pending','active',
        'group','modifier','config','priority','pwrr_after'),struct.unpack('<12Q',raw)))

def local_initialization_snapshot(raw):
    require(len(raw)==256,'Short local initialization result.')
    names=('state','epoch','core','deadline','controller_state','phase','error','writes',
        'redistributor','original_control','restored_control','sre_before','sre_after',
        'igrpen0','igrpen1','waker','enable','pending','active','group','modifier',
        'config','priority','pwrr','cleanup_error','cleanup_writes','stopped_before_power','cpu_interface_observed','stopped_before_wake',
        'stopped_after_wake','awake_waker','controller_refusal_restored')
    return dict(zip(names,struct.unpack('<32Q',raw)))

def el3_cache_snapshot(raw):
    require(len(raw)==160,'Short EL3 cache record.')
    v=struct.unpack('<20Q',raw)
    names=('sctlr','tcr','ttbr','mair','scr')
    return {'state':v[0],'epoch':v[1],'deadline':v[2],'helper_result':v[3],
        'original':dict(zip(names,v[4:9])),'enabled':dict(zip(names,v[9:14])),
        'restored':dict(zip(names,v[14:19])),'failure':v[19]}

def validate_el3_cache(r):
    require(r['state']==3 and r['helper_result']==1 and r['failure']==0,'EL3 cache transition did not restore.')
    old=r['original'];live=r['enabled']
    require(not old['sctlr']&0x02001005 and r['restored']==old,'EL3 controls differ after restoration.')
    require(live==dict(sctlr=old['sctlr']|0x1005,tcr=0x80823019,ttbr=0x421f0000,
        mair=0x44ff00,scr=old['scr']&~0x200),'EL3 adoption readbacks differ.')

def el1_cache_snapshot(raw):
    require(len(raw)==256,'Short EL1 cache record.')
    v=struct.unpack('<32Q',raw)
    names=('sctlr','tcr','ttbr0','ttbr1','mair')
    observed=('marker','CurrentEL','sctlr','tcr','ttbr0','mair','ttbr1','sp','tpidr','cntfrq','mpidr','igrpen1')
    return {'state':v[0],'epoch':v[1],'deadline':v[2],'saved_sp':v[3],
        'original':dict(zip(names,v[4:9])),'observed':dict(zip(observed,v[9:21])),
        'esr':v[21],'elr':v[22],'spsr':v[23],'restored':dict(zip(names,v[24:29])),
        'failure':v[29],'vector':v[30],'return_el':v[31]}

def validate_el1_cache(r,return_address):
    require(r['state']==3 and not r['failure'] and r['restored']==r['original'],'EL1 controls did not restore.')
    require(r['original']['sctlr']==0xc50839 and r['original']['tcr']==0x800080,'EL1 original walk block differs.')
    require(r['observed']==dict(marker=0x733,CurrentEL=4,sctlr=0x30d01805,tcr=0x200803019,
        ttbr0=0x421f2000,mair=0x44ff00,ttbr1=0,sp=0x41253000,tpidr=1,cntfrq=24000000,
        mpidr=0x81000100,igrpen1=0),'EL1 production entry readbacks differ.')
    require(r['esr']==0x5e000733 and r['elr']==return_address and r['spsr']&0x3ff==0x3c5 and
        r['vector']==8 and r['return_el']==12,'EL1 SMC return identity differs.')

def ns_publication_snapshot(raw):
    require(len(raw)==128,'Short Nonsecure publication record.')
    names=('state','epoch','deadline','code_lines','table_lines','ttbr','sctlr','tcr','mair',
        'restored_sctlr','restored_tcr','restored_ttbr','restored_mair','failure','secure_word','nonsecure_word')
    return dict(zip(names,struct.unpack('<16Q',raw)))

def validate_ns_publication(r,cache,call_word):
    require(r['state']==3 and not r['failure'] and r['epoch']==cache['epoch'],'Nonsecure publication incomplete.')
    require(r['code_lines']==32768 and r['table_lines']==128 and
        r['ttbr']==0x421f5000 and r['sctlr']==cache['original']['sctlr']|1 and
        r['tcr']==0x80823019 and r['mair']==0x44ff00,'Nonsecure publication mapping or extent differs.')
    require(all(r['restored_'+n]==cache['original'][n] for n in ('sctlr','tcr','ttbr','mair')),
        'Publication controls did not restore.')
    require(r['secure_word']==r['nonsecure_word']==call_word,'Changed instruction aliases differ.')

def controller_access_snapshot(raw):
    require(len(raw)==32,'Short controller access record.')
    return dict(zip(('address','value','operation','completed'),struct.unpack('<4Q',raw)))

def activation_snapshot(raw):
    require(len(raw)==256,'Short activation snapshot.')
    names=('state','epoch','request','ack','core','initial_control','initial_mode','last_ppu','deadline',
           'secondary_pwr_original','secondary_pwr_set','secondary_mode','secondary_control',
           'restored_control','restored_mode','error','secondary_state','secondary_el','secondary_midr',
           'secondary_sctlr','secondary_mpidr')
    return dict(zip(names,struct.unpack('<32Q',raw)))

def secondary_snapshot(power,owner,mailbox):
    require(len(power)==128 and len(owner)==248 and len(mailbox)==256,'Short secondary snapshot.')
    p=dict(zip(('boot_mpidr','entry','lease','deadline'),struct.unpack_from('<4Q',power,40)))
    p['core']=struct.unpack_from('<I',power,72)[0];p['last_now']=struct.unpack_from('<Q',power,80)[0]
    p.update(zip(('control','mode','architecture','power_status','state','error','step','initialized','entered','writes_attempted'),struct.unpack_from('<10I',power,88)))
    o=dict(zip(('last_now','deadline','lease'),struct.unpack_from('<3Q',owner,192)))
    o.update(zip(('core','phase','initialized','entered','writes_attempted','state','error'),struct.unpack_from('<7I',owner,216)))
    command=dict(zip(('magic','epoch','core','state','inverse_epoch'),struct.unpack_from('<5Q',mailbox)))
    report=dict(zip(('epoch','core','midr','mpidr','sctlr','state','esr','far','elr','spsr','reason'),struct.unpack_from('<11Q',mailbox,128)))
    return {'power':p,'owner':o,'command':command,'report':report}

def caller_interval(survey):
    require(survey.get('complete') is True and survey.get('sram_restored') is True and survey.get('rom_usable') is True,'Completed current returning survey required.')
    sp=survey['result']['sp'];require(type(sp) is int and 0x4003c<=sp<=0x100000 and sp%4==0,'Caller SP outside reviewed SRAM.')
    require(all(sp<=address or sp-60>=address+size for _,address,size in base.REGIONS),'Caller stack overlaps launcher reservations.')
    return sp-60,60

def claimed(a,expected):
    db=sqlite3.connect(a.database.resolve().as_uri()+'?mode=ro',uri=True);db.row_factory=sqlite3.Row
    try:
        require(not db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()[0],'Queue halted.')
        row=db.execute('SELECT * FROM tests WHERE id=?',(a.claim,)).fetchone()
        require(row and row['state']=='claimed' and row['owner']==a.owner,'Coordinator claim required.')
        submission,review=json.loads(row['submission']),json.loads(row['review'])
        require(set(submission['prerequisites'])==set(review['prerequisites'])=={'supervised-ram-recovery','native-startup-software'},'Unexpected prerequisites.')
        _,software=prior.reference(review['prerequisites']['native-startup-software'])
        require(hashlib.sha256(software).hexdigest()==expected[a.software.resolve()],'Reviewed software differs.')
        covered={}
        for item in submission['artifacts']:
            path,data=prior.reference(item);covered[path]=hashlib.sha256(data).hexdigest()
        require(all(covered.get(path)==identity for path,identity in expected.items()),'Queue covers different inputs.')
        _,raw=prior.reference(review['prerequisites']['supervised-ram-recovery']);return json.loads(raw),hashlib.sha256(raw).hexdigest()
    finally:db.close()

def run(a):
    a.build=a.build.resolve();raw=(a.build/'manifest.json').read_bytes();m=json.loads(raw);manifest_hash=hashlib.sha256(raw).hexdigest()
    primary = None
    is_interrupts = m.get('schema') == 'a733-secondary-interrupt-probe-v1'
    is_coherent = is_interrupts or m.get('schema') == 'a733-secondary-coherence-probe-v1'
    is_cached = is_coherent or m.get('schema') == 'a733-secondary-cache-probe-v1'
    is_local = is_cached or m.get('schema') == 'a733-secondary-local-probe-v1'
    is_identity = is_local or m.get('schema') == 'a733-secondary-identity-probe-v1'
    is_primary = is_identity or m.get('schema') == 'a733-primary-publication-probe-v1'
    prepared=supervised.validate_bundle(a.bundle)
    if is_primary:
        primary=module('returning_cpu_host',ROOT/'tools'/('secondary-interrupt-host.py' if is_interrupts else 'secondary-coherence-host.py' if is_coherent else 'secondary-cache-host.py' if is_cached else 'secondary-local-host.py' if is_local else 'secondary-identity-host.py' if is_identity else 'primary-publication-host.py'))
        frozen,dram=primary.load(a.build,a.bundle/'image.elf',m)
        native=primary.probe.NATIVE;profile=primary.PROFILE
    else:
        profile=manifest_profile(m)
        require(m['schema']=='a733-native-startup-probe-v1' and m.get('execution_profile')==profile and m['native_sha256']==NATIVE,'Wrong startup image.')
        require(m['source_sha256']==digest(ROOT/'boot/a733/native-startup-probe.S') and m['host_sha256']==digest(Path(__file__)),'Implementation changed.')
        require(m.get('sources')=={name:digest(ROOT/name) for name in SOURCES},'Compiled controller inputs changed.')
        require(m.get('overlay_reservation')=={'address':OVERLAY[0],'bytes':OVERLAY[1]},'Overlay reservation differs.')
        frozen={name:(a.build/name).read_bytes() for name in m['files']}
        require(all(Path(n).name==n and hashlib.sha256(b).hexdigest()==m['files'][n] for n,b in frozen.items()),'Frozen build changed.')
        native=NATIVE;dram=DRAM
    require(prepared['image_sha256']==native and prepared['launcher_sha256']==LAUNCH,'Native/launcher identity differs.')
    survey_raw=a.survey.read_bytes();survey=json.loads(survey_raw);caller=caller_interval(survey)
    require(all(survey['result']['launcher_gates'].values()),'Actual A32 launcher gates fail; establish cache-off state separately.')
    software_raw=a.software.read_bytes();software=json.loads(software_raw)
    if is_primary:
        primary.validate_software(software,manifest_hash)
    else:
        require(software.get('schema')=='a733-native-startup-software-v1' and software.get('physical_access') is False and
                software.get('execution_profile')==profile and software.get('manifest_sha256')==manifest_hash and software.get('instruction_cases') is True,'Focused instruction evidence required.')
    expected={a.build/'manifest.json':manifest_hash,a.software.resolve():hashlib.sha256(software_raw).hexdigest(),
              a.survey.resolve():hashlib.sha256(survey_raw).hexdigest(),(a.bundle/'prepared.json').resolve():prepared['_prepared_sha256']}
    recovery,recovery_hash=claimed(a,expected);require(digest(a.fel)==base.FEL_HASH,'Pinned FEL executable required.')
    a.evidence.mkdir(parents=True,exist_ok=False)
    record={'physical_access':True,'complete':False,'launch_attempted':False,'transactions':[],'claim':a.claim,'owner':a.owner,
        'execution_profile':profile,'native_sha256':native,
        'manifest_sha256':expected[a.build/'manifest.json'],'recovery_sha256':recovery_hash,'caller_interval':caller,'sram_after_warm_return':'Owned by new ROM; old bytes are evidence only.'}
    def journal():save(a.evidence/'session.json',record)
    access=module('startup_access',ROOT/'tools/usb-access.py');sys.path.insert(0,str(Path.home()/'git/mimus/tools'));import usb_guard
    usb_guard.check_health()
    with access.board_lock() as node:
        access.require_session(node);access.require_rom(node);identity=access.identity(node);port=access.target(node);config={'port':port.name,'sysfs':str(port.resolve())};failed=False;dirty=False
        require(survey.get('identity')==identity and survey.get('port')==port.name and recovery.get('identity')==identity and recovery.get('port')==port.name,'Survey/recovery covers another live instance.')
        supervised.validate_admission(json.loads(prepared['_frozen']['admission.json']),native,identity,port.name)
        record.update(initial_identity=identity,initial_node=str(node),port=port.name);journal()
        def transaction(*arguments,pass_fds=()):
            nonlocal failed
            require(not failed and access.identity(node)==identity,'Transport failed or instance changed; stop.');access.require_rom(node)
            entry={'arguments':list(map(str,arguments))};record['transactions'].append(entry);journal()
            try:
                result=subprocess.run([str(a.fel),'-d',f'{int(node.parent.name)}:{int(node.name)}',*map(str,arguments)],capture_output=True,text=True,timeout=15,pass_fds=pass_fds)
                entry.update(exit_code=result.returncode,stdout=result.stdout,stderr=result.stderr);require(result.returncode==0,'FEL transaction failed.')
            except BaseException as error:failed=True;entry['failure']=str(error);journal();raise
            journal();return result.stdout
        def transfer(address,data,destination):
            with base.sealed_source(data) as (path,fd):transaction('write',hex(address),path,'read',hex(address),str(len(data)),destination,pass_fds=(fd,))
            require(destination.read_bytes()==data,'Transfer comparison differs.')
        try:
            require('soc=00001903(A733)' in transaction('version'),'Not an A733 ROM instance.')
            words=transaction('readl','0x20','readl','0x24','readl','0x28','readl','0x3c','readl','0x40')
            require([int(v,16) for v in words.split()]==[0xeb00008f,0xe3a00911,0xe3a0193d,0xe3a0605c,0xea00004d],'Forced-FEL destination changed.')
            backups={}
            for name,address,size in (*dram,*base.REGIONS,('caller-evidence',*caller)):
                path=a.evidence/(name+'-before.bin');transaction('read',hex(address),str(size),path)
                data=path.read_bytes();require(len(data)==size,'Short backup.');base.sync_file(path);backups[name]=data
            nonce=secrets.token_bytes(16);initial=bytearray(0x4000)
            if not is_primary:initial[0x2000+240:0x2000+256]=nonce
            record['nonce']=nonce.hex();dirty=True;journal()
            uploads=[(s['address'],frozen[s['name']],s['name']) for s in m['payloads']]
            uploads += [(0x41020000,frozen['overlay.bin'],'overlay.bin'),(0x42000000,bytes(initial),'result.bin'),
                        (0x42004000,prepared['_frozen']['contract.bin'],'contract.bin'),(0x52c00,prepared['_frozen']['request.bin'],'request.bin'),
                        (0x52d00,bytes(128),'launch-report.bin'),(0x52000,prepared['_frozen']['launch.bin'],'launch.bin')]
            if is_primary:
                uploads.insert(0,(primary.probe.REPORT[0],primary.initial_report(nonce),'primary-report.bin'))
            for address,data,name in uploads:transfer(address,data,a.evidence/('upload-'+name))
            _,current_hash=claimed(a,expected);require(current_hash==recovery_hash,'Recovery review changed.')
            with usb_guard.reenumeration('sunstoneos-a733','Native returning CPU diagnostic with verified restoration',2):
                record['launch_attempted']=True;journal();transaction('exe','0x52000')
                deadline=time.monotonic()+10;old=identity
                while True:
                    candidate=access.session_node(config)
                    if candidate is not None and access.identity(candidate)!=old and os.access(candidate,os.R_OK|os.W_OK):break
                    require(time.monotonic()<deadline,'No new FEL instance; no protocol retry.');time.sleep(.05)
                node=candidate;identity=access.identity(node);access.require_session(node);access.require_rom(node)
                record.update(returned_identity=identity,returned_node=str(node));journal()
            require('soc=00001903(A733)' in transaction('version'),'Return is not A733 ROM service.')
            for name,address,size in ([('report',primary.probe.REPORT[0],primary.probe.REPORT[1])] if is_primary else [('report',REPORT,256),('native-result',0x42000000,128),('admission',0x42004000,224),('tables',0x421f0000,0x7000),
                                      ('coherence',0x4200e480,184),('core-record',0x4200e080,128),('gic',0x42002100,512),
                                      ('observation',0x42002700,512),('local-interrupts',0x42002900,96),('controller-access',0x42002960,32),('local-initialization',0x42002980,256),('el3-cache',0x42002a80,160),('el1-cache',0x42002b20,256),('ns-publication',0x42002c20,128),('redistributor',0x42002500,512),('activation',0x42002400,256),('power',0x4200eb48,128),('owner',0x4200eec8,248),('mailbox1',0x42005100,256)]):
                transaction('read',hex(address),str(size),a.evidence/(name+'.bin'));base.sync_file(a.evidence/(name+'.bin'))
            error=None
            try:
                if is_primary:
                    record['result']=primary.probe.decode((a.evidence/'report.bin').read_bytes(),nonce,
                        *([m['hooks']['el1_smc_return']] if is_cached else []))
                    if is_identity:
                        contract=prepared['_frozen']['contract.bin']
                        require(record['result']['epoch']==struct.unpack_from('<Q',contract,24)[0],
                                'Secondary survey covers a different startup contract.')
                else:
                    record['result']=decode((a.evidence/'report.bin').read_bytes(),nonce)
                    record['controller']=controller_snapshot((a.evidence/'coherence.bin').read_bytes(),(a.evidence/'core-record.bin').read_bytes())
                    record['gic']=gic_snapshot((a.evidence/'gic.bin').read_bytes())
                    record['activation']=activation_snapshot((a.evidence/'activation.bin').read_bytes())
                    record['redistributor']=rdist_snapshot((a.evidence/'redistributor.bin').read_bytes())
                    record['observation']=observation_snapshot((a.evidence/'observation.bin').read_bytes())
                    record['local_interrupts']=local_interrupt_snapshot((a.evidence/'local-interrupts.bin').read_bytes())
                    record['local_initialization']=local_initialization_snapshot((a.evidence/'local-initialization.bin').read_bytes())
                    record['el3_cache']=el3_cache_snapshot((a.evidence/'el3-cache.bin').read_bytes())
                    if m.get('el3_cache',False) or m.get('el1_cache',False):validate_el3_cache(record['el3_cache'])
                    record['el1_cache']=el1_cache_snapshot((a.evidence/'el1-cache.bin').read_bytes())
                    if m.get('el1_cache',False):validate_el1_cache(record['el1_cache'],m['el1_smc_return'])
                    record['ns_publication']=ns_publication_snapshot((a.evidence/'ns-publication.bin').read_bytes())
                    if m.get('el1_cache',False):validate_ns_publication(record['ns_publication'],record['el3_cache'],m['el1_call_word'])
                    record['controller_access']=controller_access_snapshot((a.evidence/'controller-access.bin').read_bytes())
                    record['secondary']=secondary_snapshot((a.evidence/'power.bin').read_bytes(),(a.evidence/'owner.bin').read_bytes(),(a.evidence/'mailbox1.bin').read_bytes())
                    observation=record['observation'];activation=record['activation'];secondary=record['secondary']
                    require(record['result']['checkpoint']==13 and observation['state']==5 and observation['origin'] in (1,3), 'No completed target observation.')
                    require(activation['state']==5 and activation['epoch']==activation['request']==activation['ack']==observation['epoch'] and not activation['last_ppu']&15, 'No matching observed-OFF handshake.')
                    require(record['redistributor']['state']==1 and record['redistributor']['writes_attempted']==0 and not any(record['redistributor']['typers']), 'Redistributor boundary crossed.')
                    require(secondary['command']['state']==secondary['report']['state']==2 and secondary['command']['epoch']==observation['epoch'], 'Actual RELEASE not observed.')
                    require(record['gic']['state']==3 and record['gic']['configuration']['state']==3 and record['result']['restore_state']==3, 'Restoration incomplete.')
                    record['preparation_observed']=observation['origin']==3
                    if record['preparation_observed']:
                        walks=observation['walks']
                        require(walks['state']==2 and walks['restored_sctlr1']==observation['SCTLR_EL1'] and
                            walks['restored_tcr1']==observation['TCR_EL1'], 'Target control restoration incomplete.')
                        require(walks['blocked_sctlr1']==(observation['SCTLR_EL1']|1) and
                            walks['blocked_tcr1']==(observation['TCR_EL1']|0x800080), 'Target walk blocking differs.')
                        require(walks['typer']>>32==0x100, 'Observed frame is not core 1.')
                        record['target_walks_and_frame_observed']=True
                        local=record['local_interrupts']
                        require(local['base']==0x03480000 and local['state'] in (2,3), 'Local interrupt observation incomplete.')
                        record['local_interrupts_observed']=local['state']==2
                        init=record['local_initialization']
                        if local['state']==2:
                            require(init['state'] in (3,4) and init['epoch']==observation['epoch'] and init['core']==1,'Local initialization did not complete or refuse safely.')
                            before_power=init['state']==4 and init['stopped_before_power']==1
                            before_wake=init['state']==4 and init['stopped_before_wake']==1
                            after_wake=init['state']==4 and init['stopped_after_wake']==1
                            refusal=init['state']==4 and init['controller_refusal_restored']==1
                            if init['cpu_interface_observed']:
                                require(m.get('stop_after_cpu_interface') is True and init['state']==4 and
                                    (init['sre_after']&7)==7 and not (init['igrpen0']&1 or init['igrpen1']&3) and
                                    not any(init[k] for k in ('controller_state','phase','error','writes','cleanup_writes',
                                                             'cleanup_error','stopped_before_power','stopped_before_wake','stopped_after_wake')),
                                    'CPU-interface-only boundary differs.')
                            if m.get('stop_after_cpu_interface'):
                                require(init['cpu_interface_observed']==1,'CPU-interface observation missing.')
                            if before_power:
                                require(m['stop_before_power'] and init['controller_state']==5 and init['error']==1 and
                                    init['writes']==2 and init['cleanup_writes']==1 and init['cleanup_error']==0,
                                    'Pre-write boundary differs.')
                            if before_wake:
                                require(m.get('stop_before_wake') is True and init['controller_state']==5 and init['phase']==8 and
                                    init['error']==1 and init['writes']>=3 and init['cleanup_writes']==1 and
                                    not any(init[k] for k in ('cleanup_error','stopped_before_power','cpu_interface_observed')),
                                    'Pre-wake boundary differs.')
                            if after_wake:
                                require(m.get('stop_after_wake') is True and init['controller_state']==5 and init['phase']==10 and
                                    init['error']==1 and init['writes']>=4 and init['cleanup_writes']==2 and
                                    not any(init[k] for k in ('awake_waker','cleanup_error','stopped_before_power','stopped_before_wake','cpu_interface_observed')),
                                    'Wake/sleep boundary differs.')
                            if refusal:
                                require(profile==PROFILE and init['controller_state']==5 and 10<=init['phase']<=26 and
                                    init['error'] in (1,2,3,4) and init['writes']>=4 and init['cleanup_writes']==7 and
                                    not any(init[k] for k in ('cleanup_error','stopped_before_power','stopped_before_wake','stopped_after_wake','cpu_interface_observed')),
                                    'Restored controller refusal differs.')
                            if init['state']==3 or before_power or before_wake or after_wake or refusal:
                                if init['state']==3:
                                    require(not m['stop_before_power'] and not m.get('stop_before_wake',False) and not m.get('stop_after_wake',False) and
                                        init['stopped_before_power']==init['stopped_before_wake']==init['stopped_after_wake']==0 and
                                        init['controller_state']==2 and init['error']==init['cleanup_error']==0,
                                        'Local initialization differs.')
                                require(init['redistributor']==local['base'] and init['restored_control']==init['original_control'],
                                    'Local control restoration differs.')
                                require(all(init[k]==local[k] for k in ('waker','enable','pending','active','group','modifier','config','priority')) and
                                    init['pwrr']==local['pwrr_after'], 'Local interrupt restoration differs.')
                        record['before_power_observed']=init['state']==4 and init['stopped_before_power']==1
                        record['cpu_interface_observed']=init['cpu_interface_observed']==1
                        record['before_wake_observed']=init['state']==4 and init['stopped_before_wake']==1
                        record['wake_sleep_observed']=init['state']==4 and init['stopped_after_wake']==1
                        record['local_initialization_observed']=init['state']==3
                        record['controller_refusal_observed']=init['state']==4 and init['controller_refusal_restored']==1
            except (ValueError,struct.error) as e:error=e
            for name,address,size in dram:transfer(address,backups[name],a.evidence/(name+'-restored.bin'))
            dirty=False;record.update(rom_usable=True,dram_restored=True,complete=error is None);journal()
            if error:raise error
        except BaseException as error:
            record.update(failure=str(error),transport_failed=failed)
            if dirty or failed:
                try:base.stop_after_failure(record,journal,lambda:access.quarantine_instance(node,'Native startup diagnostic stopped; no retry; coordinator recovery required.',identity),error)
                finally:
                    if dirty and not record['launch_attempted']:base.hold_ownership()
            else:journal()
            raise
    return 0

def main():
    p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='operation',required=True)
    build=sub.add_parser('prepare')
    for name in ('build','observer','clang','lld','objcopy'):build.add_argument('--'+name,type=Path,required=True)
    boundary=build.add_mutually_exclusive_group()
    boundary.add_argument('--stop-before-power',action='store_true',help='Stop before the first redistributor write and restore unchanged local state.')
    boundary.add_argument('--stop-after-cpu-interface',action='store_true',help='Observe EL3 GIC access and disabled groups without invoking the local controller.')
    boundary.add_argument('--stop-before-wake',action='store_true',help='Complete the redistributor power handshake, then stop before WAKER and preserve the asleep state.')
    boundary.add_argument('--el1-cache',action='store_true',help='Execute production cached EL1 entry, observe controls and return before interrupt delivery.')
    boundary.add_argument('--el3-cache',action='store_true',help='After local initialization, adopt production EL3 tables and restore controls before shutdown.')
    boundary.add_argument('--stop-after-wake',action='store_true',help='Complete redistributor wake, stop before local configuration and restore sleep before shutdown.')
    live=sub.add_parser('run')
    for name in ('build','bundle','software','survey','database','fel','evidence'):live.add_argument('--'+name,type=Path,required=True)
    live.add_argument('--claim',required=True);live.add_argument('--owner',required=True);a=p.parse_args()
    if a.operation=='prepare':prepare(a);return 0
    return run(a)
if __name__=='__main__':
    try:sys.exit(main())
    except (ValueError,OSError,RuntimeError,sqlite3.Error,struct.error) as error:sys.exit(f'Stopped: {error}')
