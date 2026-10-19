#!/usr/bin/env python3
"""Record linked GPU adapter requirements for the coordinator's returning platform."""
import argparse,hashlib,json
from pathlib import Path
from elftools.elf.elffile import ELFFile
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True);a=p.parse_args();e=a.evidence.resolve();path=e/'build/adapter.elf';r=Path(__file__).resolve().parents[2]
with path.open('rb') as f:
    elf=ELFFile(f);symbols={s.name:(int(s['st_value']),int(s['st_size'])) for s in elf.get_section_by_name('.symtab').iter_symbols()}
    sections={s.name:[int(s['sh_addr']),int(s['sh_addr']+s['sh_size'])] for s in elf.iter_sections() if int(s['sh_flags'])&2}
def sym(name):return symbols[name][0]
for name in ['gpu_runtime_contract','gpu_runtime_secondary_off','sealed_session','session_sealed']:
    start,size=symbols[name]
    assert size and sym('__gpu_retained_start')<=start and start+size<=sym('__gpu_retained_end'),name
    assert start+size<=sym('__bss_start') or start>=sym('__bss_end'),name
record={
 'development_only':True,'physical_eligible':False,'elf_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'macro':'SUNSTONE_GPU_FINITE_RETURN=1',
 'retained_start':sym('__gpu_retained_start'),'retained_end':sym('__gpu_retained_end'),'bss_start':sym('__bss_start'),'bss_end':sym('__bss_end'),'stack_end':sym('__exception_stack_top'),
 'pool_start':sym('__device_pool_start'),'pool_end':sym('__device_pool_end'),'uncached_pool_start':sym('__uncached_pool_start'),'uncached_pool_end':sym('__uncached_pool_end'),
 'clean_start':sections['.data'][0]&~63,'clean_end':sym('__exception_stack_top'),
 'session_upload':0x41f00000,'session_bytes':592,'contract_upload':0x42004000,'contract_bytes':288,
 'el3_interfaces':['a733_smp_contract_valid','gpu_application_contract_digest','gpu_application_entry_digest','gpu_application_seal_session','a733_device_memory_boot_admit','a733_cached_memory_prepare'],
 'retained_runtime':['gpu_runtime_contract','gpu_runtime_secondary_off','sealed_session','session_sealed','device-memory layout admission state within __gpu_retained bounds'],
 'el1_interfaces':['gpu_application_session','sd_single_core_contract','sd_single_core_secondaries_admitted','a733_cached_runtime','a733_smp_device_preparation_admitted','a733_device_memory_prepare','a733_device_memory_ready'],
 'retained_symbol_extents':{n:{'start':symbols[n][0],'bytes':symbols[n][1]} for n in ['gpu_runtime_contract','gpu_runtime_secondary_off','sealed_session','session_sealed','gpu_admission']},
 'sections':sections,
 'cpu_mapping':{'application_and_stacks':'Identity Normal WB, attributes0xff, inner-shareable3, no alias; private monitor RAM inaccessible to EL1.','wb_pool':'Identity Normal WB, attributes0xff, shareability3, exclusive64byte cache ownership.','nc_pool':'Identity Normal noncacheable, attributes0x44, shareability2or3; no cacheable alias.','mmio':'GPU01800000,01801000,01803000,0180a000,0180f000 and GICD03400000 pages must be pinned identity Device-nGnRnE. Existing PMIC/CCU/PCK providers retain their mappings.'},
 'fixed_compute_allocations':{'firmware_wb_pages':34,'payload_wb_pages':88,'table_wb_pages':12,'firmware_nc_pages':45,'compute_firmware_nc_pages':6,'total_wb_bytes':134*4096,'total_nc_bytes':51*4096,'known_wb_allocation_interval':[0x48000000,0x48000000+134*4096],'known_nc_allocation_interval':[0x78000000,0x78000000+51*4096],'producer_fixture':'checks/gpu-compute-reserved.c','producer_evidence':str(Path.home()/'.cache/sunstoneos/evidence/gpu-compute-reserved-20261016/batch/results.json'),'limit':'Allocation counts do not narrow the authenticated large-pool geometry or prove physical ownership. Recompute addresses if composition introduces another pool consumer.'},
 'stop_contract':'Fresh forced-idle/OFF ACK1 plus firmware states2/0, BIF/SLC/wrapper idle, reset readback, asynchronous platform OFF/token0/delegated0 and firmware-memory reclaim. Keep allocation parents, compute mappings and copied output owned.',
 'task_stop':'Finite adapter suspends GPU worker after STOPPED, parks PMIC outside polling, checks empty slots/no borrows/unheld mutexes and disabled/nonpending/nonactive INT95 with RWP clear, then enters a task critical section before platform_exit(0). No USB task/controller is constructed. Root must stop EL1 scheduler/timers/interrupt delivery before restoring any task stack or image RAM.',
 'root_cleanup':'Retain authenticated session/contract across BSS clearing and EL3-to-EL1 transition. Clean writable image/retained/BSS/stacks, dirty GPU backing and translation tables before cache disable and physical restoration; maintain distinct secure monitor state/stack until the final operation. Freeze final linked bounds and actual device-allocation closure. No timeout, failure or unexpected return may authorize restoring possibly active DMA memory.',
 'monitor_gap':'Development ELF retains the earlier full single-core monitor/runtime objects; platform_exit does not establish a working returning monitor. Common returning platform must implement authenticated retention, large-memory mappings/verification, scheduler stop, final cache maintenance/restoration and confirmed ROM return.',
 'physical_prerequisites':['Coordinator-only target identity, DDR ownership of complete admitted geometry and all-master exclusion','Supported owned400MHz parent and settled DCDC4 supply, PMIC/CCU/PCK exclusion','Initial GPU reset/gate state and exclusion of earlier undrained activity','Authenticated image/epoch/generation/lease/session/entry-evidence binding','Qualified common returning monitor with failure retention and supervised recovery; no automatic retry'],
}
(e/'integration-prerequisites.json').write_text(json.dumps(record,indent=2)+'\n');print('Recorded exact linked GPU retained/BSS/stack/pool and returning-platform requirements.')
