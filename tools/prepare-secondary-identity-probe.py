#!/usr/bin/env python3
"""Extend the demonstrated primary-return overlay with a sequential CPU survey."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import subprocess
from types import SimpleNamespace

from elftools.elf.elffile import ELFFile

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('primary_host',ROOT/'tools/primary-publication-host.py')
primary=importlib.util.module_from_spec(spec);spec.loader.exec_module(primary)
require,digest=primary.require,primary.digest
EXTENSION=0x41021000
REPORT=(0x42018100,0x900)


def branch(address,target):
    displacement=target-address
    require(displacement%4==0 and -(1<<27)<=displacement<(1<<27),'Branch out of range.')
    return 0x14000000|((displacement//4)&0x3ffffff)


def cached_patch_points(native):
    # Exact offsets in the NATIVE-pinned ELF; every displaced word is checked.
    # +0x194 is MSR ICC_CTLR_EL1, XZR, the first EL1 interrupt-interface write.
    entry=native['a733_cached_enter_el1']+0x194
    vector=native['a733_cached_el3_vectors']+8*128+4
    return ((entry,0xd518cc9f,'el1_observe_checkpoint'),
            (vector,branch(vector,native['a733_cached_el3_fault']),'el1_return_checkpoint'))


def header_defines(path=ROOT/'boot/a733/secondary-local-probe.h'):
    """Every plain '#define NAME VALUE' of a probe header, as integers."""
    return {name:int(value,0) for name,value in
            re.findall(r'^#define (\w+) (0x[0-9a-fA-F]+|\d+)\b',path.read_text(),re.M)}


# Return-deadline bisection profiles (docs/designs/workload-bisect.md). The
# record address, magic number and deadline come from the overlay's header.
PROBE=header_defines()
DEADLINE_RECORD=(PROBE['DEADLINE_RECORD'],PROBE['DEADLINE_RECORD_BYTES'])
DEADLINE_RECORD_MAGIC=PROBE['DEADLINE_RECORD_MAGIC']
DEADLINE_TICKS=PROBE['DEADLINE_TICKS']
# Read-only status bits of each local snapshot word, in the order of struct
# secondary_local_state. Before/after equalities ignore them.
STATUS=tuple(PROBE.get('GICR_'+n+'_STATUS',0) for n in
             ('CTLR','WAKER','ISENABLER0','ISPENDR0','ISACTIVER0','IGROUPR0','IGRPMODR0','ICFGR1','IPRIORITYR0','PWRR'))
DISTRIBUTOR_ERRORS={0:'none',PROBE['DISTRIBUTOR_ERROR_CONFIGURATION']:'wrong distributor configuration',
                    PROBE['DISTRIBUTOR_ERROR_RWP_TIMEOUT']:'GICD_CTLR.RWP still set at the deadline'}


def stable(words):
    """A ten-word local snapshot without its read-only status bits."""
    return tuple(w&~s for w,s in zip(words,STATUS,strict=True))


def check_distributor(raw,deadline):
    """Require the distributor record at report offset 0x1a00 to show a complete
    restoration; a failure names the record's error word. Returns the bank
    count and the saved SPI enable words."""
    state,before,after,banks,recorded=struct.unpack_from('<5Q',raw,0x1a00)
    error=struct.unpack_from('<Q',raw,0x1b28)[0]
    detail=f' (state {state}, error {error}: {DISTRIBUTOR_ERRORS.get(error,"unknown code")})'
    require(state==4 and not error and not (before^after)&~PROBE['GICD_CTLR_RWP'] and before&0x70==0x30 and
            1<=banks<=32 and recorded==deadline,'Distributor restoration incomplete'+detail+'.')
    saved=struct.unpack_from('<32I',raw,0x1a28)
    require(saved==struct.unpack_from('<32I',raw,0x1aa8) and not any(raw[0x1b30:0x1c00]),
            'Distributor enables or report padding differ'+detail+'.')
    return banks,saved


def deadline_patch_points(native,quiet):
    # Exact words of the frozen native ELF; each displaced word is checked.
    entry=native['a733_cached_enter_el1']+0x34  # MSR ICC_IGRPEN0_EL1, XZR
    fiq=native['a733_cached_el3_vectors']+0x500  # Lower-EL AArch64 FIQ: MOV X18, #0xa
    points=[(entry,0xd518ccdf,'identity_deadline_arm',True),(fiq,0xd2800152,'identity_deadline_fiq',False)]
    if quiet:
        # The background loop's per-increment BL ucPortGetCoreID becomes an EL0
        # WFE (SCTLR_EL1.nTWE is set); W0 keeps the core index from the first call.
        at=native['background']+0x40
        call=branch(at,native['ucPortGetCoreID'])|0x80000000  # BL ucPortGetCoreID
        points.append((at,call,None,False))
    return points


FLAGS=('local','cached','coherent','interrupts','pre_scheduler','first_task','task_switch',
       'task_workload','return_deadline','quiet')


def profile(**given):
    """The profile flags with every implied stage set; the builder and every
    host module derive the profile's inputs and patches from this value."""
    require(set(given)<=set(FLAGS),'Unknown profile flag.')
    f=SimpleNamespace(**{name:bool(given.get(name,False)) for name in FLAGS})
    require(not f.quiet or f.return_deadline,'The quiet-background profile requires the return deadline.')
    require(not f.return_deadline or f.task_workload,'The return deadline applies to the task workload only.')
    f.task_switch|=f.task_workload
    f.first_task|=f.task_switch
    f.pre_scheduler|=f.first_task
    require(not(f.pre_scheduler and f.interrupts),'Pre-scheduler profile keeps interrupt delivery disabled.')
    f.coherent|=f.interrupts or f.pre_scheduler
    f.cached|=f.coherent
    f.local|=f.cached
    return f


def schema(f):
    for flag,name in (('task_workload','all-core-task-workload'),('task_switch','all-core-task-switch'),
                      ('first_task','all-core-first-task'),('pre_scheduler','all-core-pre-scheduler'),
                      ('interrupts','secondary-interrupt'),('coherent','secondary-coherence'),
                      ('cached','secondary-cache'),('local','secondary-local')):
        if getattr(f,flag):return 'a733-'+name+'-probe-v1'
    return 'a733-secondary-identity-probe-v1'


def sources(f):
    """Source files whose digests bind the build to the tree."""
    source='boot/a733/secondary-identity-probe'
    names=(source+'.c',source+'.S',source+'.h','include/a733_cpu_entry.h','include/a733_cpu_start.h',
           'platform/a733-smp-cached/boot.h','tools/prepare-secondary-identity-probe.py')
    if f.local:names+=('boot/a733/secondary-local-probe.c','boot/a733/secondary-local-probe.h',
                       'boot/a733/secondary-distributor-probe.c','include/a733_cpu_continue.h')
    if f.cached:names+=('boot/a733/native-el3-cache-probe.S','boot/a733/native-el1-cache-probe.S')
    if f.pre_scheduler:names+=('boot/a733/all-core-first-task-probe.S' if f.first_task else 'boot/a733/all-core-pre-scheduler-probe.S','boot/a733/primary-local-cache-probe.c')
    elif f.coherent:names+=('boot/a733/secondary-coherence-probe.S',)
    if f.interrupts:names+=('boot/a733/secondary-interrupt-probe.S',)
    if f.return_deadline:names+=('boot/a733/workload-deadline-probe.S',)
    return names


def report_area(f):
    return (REPORT[0],0x5100 if f.pre_scheduler else 0x4100 if f.coherent else 0x2f00 if f.cached else 0x1b00) if f.local else REPORT


def extension_address(f):
    return 0x41020800 if f.interrupts else EXTENSION


def patch_plan(native,hooks,f):
    """Native words the profile replaces, as (address, original, replacement)
    in application order. Every original word is checked when applied."""
    target=native['a733_cpu_secondary_continue']
    # MRS x4,CurrentEL, the reviewed first word of the secondary continuation.
    plan=[(target,0xd5384244,branch(target,hooks['identity_target']))]
    if f.cached:
        plan+=[(address,original,branch(address,hooks[hook])) for address,original,hook in cached_patch_points(native)]
    if not f.first_task:
        return plan
    # The native entry sets SCTLR_EL1.nTWE and nTWI itself (NATIVE_EL1_SCTLR).
    first=[
        (native['a733_smp_primary_el1'],0xa9bd7bfd,branch(native['a733_smp_primary_el1'],native['a733_smp_diagnostic_start'])),
        (native['platform_tick_setup'],0xd538d088,0xd65f03c0),
        (native['prvIdleTask'],0xa9be7bfd,branch(native['prvIdleTask'],hooks['identity_first_task_el0'])),
        (native['prvPassiveIdleTask'],0xd4000d21,branch(native['prvPassiveIdleTask'],hooks['identity_first_task_el0'])),
        (native['FreeRTOS_SWI_Handler'],0xa9bf07e0,branch(native['FreeRTOS_SWI_Handler'],hooks['identity_first_task_svc']))]
    if f.task_switch:
        # Let both idle functions execute their native yield. Intercept the
        # selected worker before its body, after the real SVC save/select/restore.
        first=first[:2]+[
            # The frozen image also creates a higher-priority USB observer
            # on CPU0. This CPU-only interval must not start that service.
            (native['a733_observe_service_start'],0xa9bf7bfd,0xd65f03c0),
            (native['worker'],0xa9bc7bfd,branch(native['worker'],hooks['identity_first_task_el0'])),
            first[-1],
            # The idle loops wait with DSB; WFI. This profile stops the tick
            # (platform_tick_setup returns at once), so an idle task that
            # resumed after its yield could wait for an interrupt that never
            # comes. Each WFI becomes SVC #0x69, the idle yield, so a resumed
            # idle task yields again until its worker runs.
            (native['prvIdleTask']+0x1c,0xd503207f,0xd4000d21),
            (native['prvPassiveIdleTask']+0x10,0xd503207f,0xd4000d21)]
    if f.task_workload:
        # The workload keeps the tick, the SGIs and the native idle WFI.
        first=[p for p in first if p[0] not in (native['prvIdleTask']+0x1c,native['prvPassiveIdleTask']+0x10)]
        # Keep the real timer setup and complete worker loop. Its saved
        # x19 still identifies the pinned core at the post-loop branch.
        first=[p for p in first if p[0] not in (native['platform_tick_setup'],native['worker'],
                                                native['a733_smp_primary_el1'])]
        at=native['worker']+0x6c
        first.append((at,0x35000513,branch(at,hooks['identity_workload_complete'])))
    if f.return_deadline:
        for address,original,hook,link in deadline_patch_points(native,f.quiet):
            first.append((address,original,0xd503205f if hook is None else
                          branch(address,hooks[hook])|(0x80000000 if link else 0)))
    return plan+first


def apply_plan(plan,payloads,frozen):
    """Apply PLAN to the frozen native segments. Returns the patched bytes by
    segment name and one patch record per applied word, in segment order."""
    data,records,applied={},[],[0]*len(plan)
    for item in payloads:
        segment=bytearray(frozen[item['name']]);start=item['address']
        for index,(address,original,replacement) in enumerate(plan):
            if start<=address<start+len(segment):
                offset=address-start
                require(struct.unpack_from('<I',segment,offset)[0]==original,f'Displaced native word differs at {address:#x}.')
                struct.pack_into('<I',segment,offset,replacement);applied[index]+=1
                records.append({'address':address,'original':original,'replacement':replacement,'space':'native'})
        data[item['name']]=bytes(segment)
    require(applied==[1]*len(plan),'Native patch coverage differs.')
    return data,records


def prepare(args):
    f=profile(**{name:getattr(args,'quiet_background' if name=='quiet' else name,False) for name in FLAGS})
    local,cached,coherent,interrupts=f.local,f.cached,f.coherent,f.interrupts
    pre_scheduler,first_task,task_switch,task_workload=f.pre_scheduler,f.first_task,f.task_switch,f.task_workload
    return_deadline,quiet=f.return_deadline,f.quiet
    extension=extension_address(f)
    report=report_area(f)
    base=json.loads((args.primary/'manifest.json').read_text())
    frozen,reservations=primary.load(args.primary,args.image,base)
    with args.image.open('rb') as stream:
        elf=ELFFile(stream)
        entries=list(elf.get_section_by_name('.symtab').iter_symbols())
        def address(name):
            values=[s['st_value'] for s in entries if s.name==name and s['st_shndx']!='SHN_UNDEF']
            require(len(values)==1,'Missing or ambiguous native symbol '+name)
            return values[0]
        names=('a733_cpu_start_init','a733_cpu_entry_native_init','a733_cpu_entry_poll',
               'a733_cpu_secondary_entry','a733_cpu_secondary_continue','a733_cpu_index',
               'a733_smp_counter','a733_smp_power_address','memset','memcpy')
        if local:names+=('prepare_core','a733_cpu_continue_init','a733_cpu_continue_poll','a733_cached_el3_vectors')
        if cached:names+=('a733_cached_allow_ns_code','a733_cpu_coherence_enable_el3','a733_cached_el3_l1',
                          'a733_cached_enter_el1','a733_cached_el1_l1','a733_cpu_el1_stacks','a733_cached_el3_fault')
        if pre_scheduler:names+=('a733_cached_block_walks','a733_smp_pre_scheduler','probe_arrived','probe_count','probe_lock',
                                 'probe_finished','probe_release')
        if first_task:names+=('a733_cached_el1','a733_smp_primary_el1','a733_smp_diagnostic_start',
                              'platform_tick_setup','prvIdleTask','prvPassiveIdleTask',
                              'FreeRTOS_SWI_Handler','pxCurrentTCBs','xIdleTaskHandles')
        if task_switch:names+=('worker','tasks','stacks','vTaskSwitchContext','a733_observe_service_start')
        if task_workload:names+=('a733_smp_core_results','vTaskDelay')
        if quiet:names+=('background','ucPortGetCoreID')
        native={n:address(n) for n in names}
        if task_switch:
            for name,expected in {'worker':(0x4100123c,372),
                                  'tasks':(0x41241498,2688),
                                  'stacks':(0x41201498,262144)}.items():
                require([(s['st_value'],s['st_size']) for s in entries if s.name==name]==[expected],
                        'Frozen worker layout differs: '+name)
        if first_task:
            data,bss=(elf.get_section_by_name(n) for n in ('.data','.bss'))
            require(data['sh_addr']==0x41200000 and data['sh_addr']+data['sh_size']<=0x41201000 and
                    bss['sh_addr']==0x41201000 and bss['sh_addr']+bss['sh_size']==0x41290f00,
                    'First-task writable extent differs from the reviewed frozen ELF.')
        if pre_scheduler:
            state=native['probe_arrived']&~4095
            require(all(native[n]&~4095==state for n in ('probe_count','probe_lock','probe_finished','probe_release')),
                    'Production pre-scheduler state exceeds the reviewed shared page.')
            require(native['probe_arrived']==state and native['probe_count']==state+32 and
                    native['probe_lock']==state+36 and native['probe_finished']==state+40 and
                    native['probe_release']==state+72,'Production pre-scheduler state layout changed.')
    require(all(report[0]+report[1]<=a or a+n<=report[0] for _,a,n in reservations),
            'Identity report overlaps an existing allocation.')
    require(len(frozen['overlay.bin'])<=extension-primary.probe.OVERLAY[0], 'Base overlay exceeds extension boundary.')
    args.output.mkdir(parents=True,exist_ok=False)
    link=args.output/'identity.ld'
    vectors=' . = 0x41020800; .irq_vectors : { KEEP(*(.irq_vectors)) }\n ASSERT(. == 0x41021000, "Vector table extent differs")\n' if interrupts else ''
    link.write_text('ENTRY(identity_primary_gate)\nSECTIONS {\n'+vectors+'''
 . = 0x41021000;
 .text : { *(.text .text.*) *(.rodata .rodata.*) }
 .data : { *(.data .data.*) }
 .bss (NOLOAD) : { *(.bss .bss.* COMMON) }
 ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "Explicit report storage only")
 ASSERT(. <= 0x41024000, "Identity extension exceeds reservation")
 /DISCARD/ : { *(.comment .note* .eh_frame*) }
}
''')
    source='boot/a733/secondary-identity-probe'
    commands=[]
    units=[(source+'.'+suffix,'identity-'+suffix+'.o') for suffix in ('c','S')]
    if local:units += [('boot/a733/secondary-local-probe.c','local.o'),('boot/a733/secondary-distributor-probe.c','distributor.o')]
    if cached:units += [('boot/a733/native-el3-cache-probe.S','el3-cache.o'),('boot/a733/native-el1-cache-probe.S','el1-cache.o')]
    if pre_scheduler:
        units += [('boot/a733/all-core-first-task-probe.S' if first_task else 'boot/a733/all-core-pre-scheduler-probe.S','pre-scheduler.o'),
                  ('boot/a733/primary-local-cache-probe.c','primary-local.o')]
    elif coherent:units += [('boot/a733/secondary-coherence-probe.S','coherence.o')]
    if interrupts:units += [('boot/a733/secondary-interrupt-probe.S','interrupt.o')]
    if return_deadline:units += [('boot/a733/workload-deadline-probe.S','deadline.o')]
    for unit,object_name in units:
        commands.append([args.clang,'--target=aarch64-none-elf','-march=armv8-a','-mgeneral-regs-only',
            '-O2','-ffreestanding','-fno-builtin','-fno-stack-protector','-ffunction-sections','-fdata-sections',
            '-Wall','-Wextra','-Werror',*(['-DSUNSTONE_SECONDARY_LOCAL=1'] if local else []),
            *(['-DSUNSTONE_SECONDARY_CACHE=1','-DSUNSTONE_PROBE_EL1_CACHE=1'] if cached else []),
            *(['-DSUNSTONE_SECONDARY_COHERENCE=1'] if coherent else []),
            *(['-DSUNSTONE_ALL_CORE_PRE_SCHEDULER=1'] if pre_scheduler else []),
            *(['-DSUNSTONE_FIRST_TASK=1'] if first_task else []),
            *(['-DSUNSTONE_TASK_SWITCH=1'] if task_switch else []),
            *(['-DSUNSTONE_TASK_WORKLOAD=1'] if task_workload else []),
            *(['-DSUNSTONE_WORKLOAD_DEADLINE=1'] if return_deadline else []),
            *(['-DSUNSTONE_SECONDARY_INTERRUPTS=1'] if interrupts else []),'-I',ROOT/'include','-I',ROOT/'platform/a733-smp-cached',
            '-I',ROOT/'boot/a733','-c',ROOT/unit,'-o',args.output/object_name])
    symbols=[f'--defsym={name}={value:#x}' for name,value in native.items()]
    if cached:
        aliases={'allow_ns_code':'a733_cached_allow_ns_code','enable_el3':'a733_cpu_coherence_enable_el3',
                 'el3_root':'a733_cached_el3_l1','enter_el1':'a733_cached_enter_el1',
                 'el1_root':'a733_cached_el1_l1','el1_stacks':'a733_cpu_el1_stacks'}
        symbols += [f'--defsym=probe_production_{a}={native[n]:#x}' for a,n in aliases.items()]
    if pre_scheduler:
        symbols += [f'--defsym=probe_production_pre_scheduler={native["a733_smp_pre_scheduler"]:#x}',
                    f'--defsym=probe_scheduler_count={native["probe_count"]:#x}',
                    f'--defsym=probe_scheduler_state={state:#x}']
    if first_task:
        symbols += [f'--defsym=probe_swi_next={native["FreeRTOS_SWI_Handler"]+4:#x}',
                    '--defsym=probe_writable_start=0x41200000','--defsym=probe_writable_end=0x41291000']
    if task_switch:symbols += [f'--defsym=probe_worker_tcbs={native["tasks"]:#x}']
    if task_workload:symbols += [f'--defsym=probe_worker_results={native["a733_smp_core_results"]:#x}']
    return_address=base['hooks']['primary_publication_return']
    symbols.append(f'--defsym=identity_original_return_next={return_address+4:#x}')
    # The first-task survey runs its pre-release admission (all-core-first-task-probe.S)
    # before identity_primary_run, so a refused state check returns instead of parking.
    admit=['--wrap=identity_primary_run'] if first_task and not task_switch else []
    commands.extend([[args.lld,'-T',link,'--gc-sections',*admit,*symbols,*[args.output/name for _,name in units],
                      '-o',args.output/'identity.elf'],
                     [args.objcopy,'-O','binary',args.output/'identity.elf',args.output/'identity.bin']])
    for command in commands:
        result=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        with (args.output/'build.log').open('a') as stream:stream.write(result.stdout+result.stderr)
        require(result.returncode==0,'Identity extension build failed; inspect build.log.')
    with (args.output/'identity.elf').open('rb') as stream:
        hooks={s.name:s['st_value'] for s in ELFFile(stream).get_section_by_name('.symtab').iter_symbols()
               if s.name.startswith('identity_') or (cached and s.name.startswith(('native_el','el1_')))}
    overlay=bytearray(frozen['overlay.bin'])
    offset=return_address-primary.probe.OVERLAY[0]
    word=struct.unpack_from('<I',overlay,offset)[0]
    require(word&0xff00001f==0x58000000,'Return checkpoint no longer loads x0 from a literal.')
    displacement=(word>>5)&0x7ffff
    if displacement&0x40000:displacement-=0x80000
    require(struct.unpack_from('<Q',overlay,offset+4*displacement)[0]==0x08001004,'Return destination differs.')
    replacement=branch(return_address,hooks['identity_primary_gate'])
    struct.pack_into('<I',overlay,offset,replacement)
    overlay+=bytes(extension-primary.probe.OVERLAY[0]-len(overlay))
    overlay+=(args.output/'identity.bin').read_bytes()
    require(len(overlay)<=primary.probe.OVERLAY[1],'Composite overlay too large.')
    (args.output/'overlay.bin').write_bytes(overlay)
    patches=[{'address':return_address,'original':word,'replacement':replacement,'space':'overlay'}]
    patched,records=apply_plan(patch_plan(native,hooks,f),base['payloads'],frozen)
    patches+=records;payloads=[]
    for item in base['payloads']:
        path=args.output/item['name'];path.write_bytes(patched[item['name']])
        payloads.append(dict(item,sha256=digest(path)))
    names=sources(f)
    manifest={'schema':schema(f),'physical_access':False,'native_sha256':primary.probe.NATIVE,
        'primary_manifest':{'path':str((args.primary/'manifest.json').resolve()),'sha256':digest(args.primary/'manifest.json')},
        'sources':{n:digest(ROOT/n) for n in names},'native_symbols':native,'hooks':hooks,'patches':patches,
        'overlay':dict(base['overlay'],sha256=digest(args.output/'overlay.bin')),'payloads':payloads,
        'identity_report':{'address':report[0],'bytes':report[1]},
        'reservations':reservations+[('identity-report',*report)],'commands':[[str(x) for x in c] for c in commands],
        'files':{p.name:digest(p) for p in args.output.iterdir() if p.is_file()}}
    if first_task:
        manifest['first_task']={'physical_eligible':False,'writable_interval':[0x41200000,0x41291000],
            'cache_lines_per_core':9280,'task_bodies':False,'timer_and_sgi_workload':False,
            'pending':'Instruction execution, return validation and source review required before physical use.'}
    if task_switch:
        manifest['task_switch']={'physical_eligible':False,'worker_body':False,
            'idle_body':True,'timer_and_sgi_workload':False,
            'pending':'Instruction execution, host report validation and root review required.'}
    if task_workload:
        manifest['first_task'].update(task_bodies=True,timer_and_sgi_workload=True)
        manifest['task_switch'].update(worker_body=True,timer_and_sgi_workload=True)
        manifest['task_workload']={'physical_eligible':False,'rounds_per_core':32,
            'completion_patch':native['worker']+0x6c,
            'pending':'Returning workload instruction checks, host validation and root review required.'}
    if return_deadline:
        manifest['workload_bisect']={
            'profile':'all-core-task-workload-quiet-deadline-v1' if quiet else 'all-core-task-workload-deadline-v1',
            'quiet_background':quiet,'deadline_ticks_after_startup':DEADLINE_TICKS,'deadline_interrupt':29,
            'record':{'address':DEADLINE_RECORD[0],'bytes':DEADLINE_RECORD[1]},
            'pending':'Instruction cases, host validation and root review required before physical use.'}
    (args.output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print('Prepared '+('first FreeRTOS task entry for all eight cores' if first_task else 'production pre-scheduler for all eight cores' if pre_scheduler else 'timer and peer IRQs for cores1..7' if interrupts else 'concurrent cached exchange for cores1..7' if coherent else 'cached EL1 entry for cores1..7' if cached else 'local initialization for cores1..7' if local else 'identity survey for cores2..7')+'; no board access.')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','primary','output','clang','lld','objcopy'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument("--local",action="store_true",help="Include actual CPU preparation and local GIC setup/restoration")
    parser.add_argument("--cached",action="store_true",help="Include production cached EL1 entry and restored SMC return")
    parser.add_argument("--coherent",action="store_true",help="Run seven concurrent EL1 shared-memory participants before restored return")
    parser.add_argument("--interrupts",action="store_true",help="Deliver one timer PPI and peer SGI on every secondary before cleaned return")
    parser.add_argument("--pre-scheduler",action="store_true",help="Execute the production barrier and atomic lock on all eight cores before cleaned return")
    parser.add_argument("--first-task",action="store_true",help="Offline-only actual task creation/context entry followed by matched cleanup")
    parser.add_argument("--task-switch",action="store_true",help="Offline-only idle yield and worker selection before matched cleanup")
    parser.add_argument("--task-workload",action="store_true",help="Offline-only complete FP/timer/SGI workload before coordinated return")
    parser.add_argument("--return-deadline",action="store_true",help="With --task-workload: CPU0 returns to the ROM with a record at a fixed deadline or on any CPU0 stop")
    parser.add_argument("--quiet-background",action="store_true",help="With --return-deadline: background tasks wait in WFE between increments")
    prepare(parser.parse_args())
