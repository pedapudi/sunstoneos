#!/usr/bin/env python3
"""Run current core1 local interrupt initialization, restore state, then verify OFF."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import secrets
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
returning=module('probe_returning',ROOT/'tools/a733-returning-host.py')
TCR_UNCACHED=module('startup_tcr',ROOT/'tools/a733_tcr.py').UNCACHED_WALK
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
    'include/a733_cpu_entry.h','include/a733_cpu_start.h','include/a733_cpu_off.h',
    'include/a733_cpu_reset.h','include/a733_cpu_coherence.h','tools/a733_tcr.py')
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
    from elftools.elf.elffile import ELFFile
    with a.observer.open('rb') as f:
        elf=ELFFile(f);symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    require(tuple(symbols[n] for n in prior.SYMBOLS)==prior.LAYOUT,'Native layout changed.')
    segments,_,_=base.image_parts(a.observer)
    with a.observer.open('rb') as stream:
        sections=[v for v in ELFFile(stream).iter_sections() if v['sh_flags']&2 and v['sh_size']]
        require(all(any(address<=v['sh_addr'] and v['sh_addr']+v['sh_size']<=address+size
                        for _,address,size in DRAM) for v in sections),
                'Native allocation exceeds the diagnostic backup plan.')
    require(all(address+size<=OVERLAY[0] or OVERLAY[0]+OVERLAY[1]<=address
                for name,address,size in DRAM if name!='overlay'),'Overlay overlaps a native reservation.')
    a.build=returning.artifact_directory(a.build)
    profile=execution_profile(a.stop_before_power,a.stop_after_cpu_interface,a.stop_before_wake,a.stop_after_wake,a.el3_cache,a.el1_cache)
    commands=[['patch','--silent','--output',a.build/'continue-core.c',ROOT/'platform/a733/cpu_continue.c',
        ROOT/'platform/a733-smp-cached/continuation-core.patch'],
        [a.clang,'--target=aarch64-none-elf','-march=armv8-a','-I',ROOT/'include','-c',ROOT/'boot/a733/native-startup-probe.S','-o',a.build/'overlay.o']]
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
        commands.append([a.clang,'--target=aarch64-none-elf','-march=armv8-a','-I',ROOT/'include','-c',ROOT/'boot/a733/native-el1-cache-probe.S','-o',obj])
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
    save(a.build/'manifest.json',{'schema':'a733-native-startup-probe-v1','physical_access':False,'execution_profile':profile,'native_sha256':digest(a.observer),
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
    require(r['observed']==dict(marker=0x733,CurrentEL=4,sctlr=0x30d01805,tcr=TCR_UNCACHED,
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

def run(a):
    """Route legacy launch commands through the native probe contract."""
    sys.path.insert(0, str(ROOT/'tools'))
    import board
    return board.legacy_run(a, image=a.build/'overlay.elf')

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
    for name in ('build','bundle','fel','evidence'):live.add_argument('--'+name,type=Path,required=True)
    for name in ('software','database','survey'):live.add_argument('--'+name,type=Path,help=argparse.SUPPRESS)
    live.add_argument('--claim',help=argparse.SUPPRESS);live.add_argument('--owner',help=argparse.SUPPRESS);a=p.parse_args()
    if a.operation=='prepare':prepare(a);return 0
    return run(a)
if __name__=='__main__':
    try:sys.exit(main())
    except (ValueError,OSError,RuntimeError,struct.error) as error:sys.exit(f'Stopped: {error}')
