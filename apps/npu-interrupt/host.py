#!/usr/bin/env python3
"""Prepare the optional interrupt NPU returning profile without default admission."""
import importlib.util,json,re,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('npu_irq_finite_host',ROOT/'apps/npu-finite/host.py')
finite=importlib.util.module_from_spec(spec);spec.loader.exec_module(finite)
finite.PROFILE='npu-interrupt-finite-return-v1'
finite.SCHEMA='a733-npu-interrupt-finite-entry-v1'
finite.PROFILES=(*finite.PROFILES,finite.PROFILE,finite.RESTART)
finite.HOST_SOURCES=(*finite.HOST_SOURCES,'apps/npu-interrupt/host.py')
PROFILE,SCHEMA,HOST_SOURCES=finite.PROFILE,finite.SCHEMA,finite.HOST_SOURCES
RESTART_CONDITIONS=(
    'same-retained-providers-service-worker-queues-buffers-and-tokens',
    'exclusive-shared-sram-through-both-routing-windows',
    'reaped-first-inference-close-and-local-stop-before-reopen',
    'stopped-masked-disabled-inactive-pending-clear-route-before-rearm',
    'original-absolute-deadline-covers-both-runs',
    'domain-pll-rail-retained-without-token-release',
    'uncertain-restart-retains-until-manual-whole-board-power-removal')
base=finite.base
_original_image=finite.image_profile
_original_admission=finite.validate_admission
_original_reports=finite.validate_reports

def image_profile(data,manifest):
    result=_original_image(data,manifest)
    from io import BytesIO
    from elftools.elf.elffile import ELFFile
    elf=ELFFile(BytesIO(data));symbols={s.name for s in elf.get_section_by_name('.symtab').iter_symbols() if s['st_value']}
    finite.require({'npu_application_irq','npu_service_irq_notify_from_isr','npu_service_irq_completions',
        'npu_service_irq_stopped'}<=symbols,'Missing native NPU interrupt path.')
    return result

def validate_admission(admission,raw,build,layout,image_hash,cache=None,*,execution_profile=PROFILE):
    profile,collected=_original_admission(admission,raw,build,layout,image_hash,cache,execution_profile=execution_profile)
    record=json.loads(admission);ref=record['interrupt'];data=finite.evidence_ref(ref,cache)
    collected[ref['path']]=data;facts=json.loads(data)
    finite.require(facts.get('schema')=='a733-npu-interrupt-entry-v1' and
        facts.get('image_sha256')==image_hash and facts.get('build_id')==build.hex() and
        facts.get('identity')==record['identity'] and facts.get('port')==record['port'] and
        facts.get('interrupt')==97 and facts.get('cpu')==0 and
        set(facts.get('conditions',{}))=={'exclusive-level-high-SPI65','single-task-event-acknowledgment',
            'masked-notification-and-serialized-rearm','close-source-disable-before-pending-clear',
            'inactive-pending-clear-before-return','saved-route-restored-before-enable'},
        'Interrupt admission does not bind the exact route and returning lifetime.')
    for item in facts['conditions'].values():
        value=finite.evidence_ref(item,cache);collected[item['path']]=value
    if execution_profile==finite.RESTART:
        ref=record['restart'];data=finite.evidence_ref(ref,cache)
        collected[ref['path']]=data;facts=json.loads(data)
        finite.require(facts.get('schema')=='a733-npu-normal-restart-entry-v1' and
            facts.get('image_sha256')==image_hash and facts.get('build_id')==build.hex() and
            facts.get('identity')==record['identity'] and facts.get('port')==record['port'] and
            all(facts.get(k)==profile[k] for k in ('epoch','generation','lease','npu_owner','whole_platform','supervisor')) and
            facts.get('reviewed') is True and facts.get('restart_qualification')=='under-test' and
            set(facts.get('conditions',{}))==set(RESTART_CONDITIONS),
            'Restart admission does not cover the same retained ownership and bounded lifetime.')
        for item in facts['conditions'].values():
            value=finite.evidence_ref(item,cache);collected[item['path']]=value
    return profile,collected

def validate_reports(monitor,platform,receipt,prepared):
    value=_original_reports(monitor,platform,receipt,prepared)
    restart=prepared.get('execution_profile')==finite.RESTART
    count=4 if restart else 3
    marker=f'NPU_IRQ_COMPLETIONS=0x{count:016x}\nNPU_IRQ_STOPPED=0x0000000000000001\n'
    finite.require(value['log'].count('NPU_IRQ_COMPLETIONS=')==1 and
        value['log'].count('NPU_IRQ_STOPPED=')==1 and marker+'NPU_STAGE=QUIESCENT\n' in value['log'],
        'Missing matched interrupt completion and stop receipts.')
    if restart:
        stages=re.findall(r'^NPU_STAGE=([^\n]+)$',value['log'],re.MULTILINE)
        finite.require(stages==['WAIT_STARTUP','OPEN','SUBMIT job=0x0000000000000001',
            'FIRST_CLOSE','RESTART_OPEN','SUBMIT job=0x0000000000000002','CLOSE','WAIT_LOCAL_STOP','QUIESCENT'],
            'Missing or reordered normal restart stages.')
        timing=value.get('timing')
        if timing:
            timing['initial_entry_to_restart_ready_us']=timing.pop('startup_us')
            timing['restart_ready_us']=timing.pop('ready_us')
        value['normal_restart']=True
    value.update(interrupt_completions=count,interrupt_stopped=True)
    return value
finite.image_profile=image_profile
finite.validate_admission=validate_admission
finite.validate_reports=validate_reports
prepare,validate_bundle,run_once=finite.prepare,finite.validate_bundle,finite.run_once
if __name__=='__main__':
    raise SystemExit(finite.main())
