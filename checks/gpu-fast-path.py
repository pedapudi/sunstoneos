#!/usr/bin/env python3
"""Check GPU hot-path calls and disabled timing against compiled model images."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from elftools.elf.elffile import ELFFile
from harness import environment

ROOT=Path(__file__).resolve().parents[1]
CHECK_INPUTS=['drivers/gpu/pvr_compute_fast.c','drivers/gpu/gpu_queue.c',
              'drivers/gpu/pvr_ccb.c',
              'drivers/gpu/pvr_gpu_service.c','apps/os/gpu.c','include/pvr_compute_fast.h',
              'checks/gpu-queue.c','checks/gpu-fast.inc','checks/gpu-queue.py']
FORBIDDEN=re.compile(r'\b(vTaskDelay|xQueueSend|xQueueReceive|xQueueGenericSend|pvr_gpu_service_send(?:_epoch)?|'
                     r'pvr_gpu_vm_build|pvr_gpu_flush_begin)\s*\(')


def body(source,name):
    match=re.search(r'^(?:static\s+)?(?:int|void|uint64_t|enum\s+\w+)\s+'+name+r'\s*\(',source,re.M)
    assert match,name
    start=source.index('{',match.end())
    depth=1
    for end in range(start+1,len(source)):
        depth+=(source[end]=='{')-(source[end]=='}')
        if not depth:
            return source[start:end+1]
    raise AssertionError('unterminated function '+name)


def layout(path):
    with path.open('rb') as stream:
        dwarf=ELFFile(stream).get_dwarf_info()
        layouts=set()
        for cu in dwarf.iter_CUs():
            for die in cu.iter_DIEs():
                if die.tag=='DW_TAG_structure_type' and die.attributes.get('DW_AT_name') and \
                        die.attributes['DW_AT_name'].value==b'pvr_compute_fast' and 'DW_AT_byte_size' in die.attributes:
                    names=tuple(sorted(child.attributes['DW_AT_name'].value.decode() for child in die.iter_children()
                                       if child.tag=='DW_TAG_member' and 'DW_AT_name' in child.attributes))
                    layouts.add((die.attributes['DW_AT_byte_size'].value,names))
        assert len(layouts)==1,(path,layouts)
        size,names=layouts.pop()
        return dict(bytes=size,members=names)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--plain',type=Path,required=True)
    p.add_argument('--timed',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--objdump',type=Path,default=Path('/usr/bin/llvm-objdump-21'))
    a=p.parse_args()
    a.output.mkdir(parents=True,exist_ok=True)
    selected={
        'drivers/gpu/pvr_compute_fast.c':['pvr_compute_fast_submit','pvr_compute_fast_collect','pvr_compute_fast_snapshot'],
        'drivers/gpu/pvr_ccb.c':['submit_direct'],
        'drivers/gpu/gpu_queue.c':['direct_submit','direct_wait','pvr_gpu_call_dispatch'],
        'apps/os/gpu.c':['pvr_compute_queue_runtime_admit']}
    sources={}
    for path,names in selected.items():
        text=(ROOT/path).read_text()
        for name in names:
            code=body(text,name)
            assert not FORBIDDEN.search(code),(path,name)
            sources[name]=hashlib.sha256(code.encode()).hexdigest()
    # Each forbidden operation inserted into the actual submission body must
    # fail the guard; production sources and images remain untouched.
    submit=body((ROOT/'drivers/gpu/pvr_compute_fast.c').read_text(),'pvr_compute_fast_submit')
    mutations=['vTaskDelay(1);','xQueueSend(0,0,0);','xQueueReceive(0,0,1);',
               'pvr_gpu_service_send_epoch(0,0,0,0,0);','pvr_gpu_vm_build(0,0,0,0,0);']
    assert all(FORBIDDEN.search(submit[:1]+mutation+submit[1:]) for mutation in mutations)
    plain,timed=layout(a.plain),layout(a.timed)
    assert not {'irq_counter','last_irq','last_wake'}&set(plain['members'])
    assert {'irq_counter','last_irq','last_wake'}<=set(timed['members'])
    assert timed['bytes']-plain['bytes']==64
    disassemblies=[]
    for path in (a.plain,a.timed):
        value=subprocess.check_output([str(a.objdump),'-d','--disassemble-symbols=pvr_gpu_service_interrupt',str(path)],
                                      text=True,env=environment()).lower()
        assert '<pvr_gpu_service_interrupt>:' in value
        disassemblies.append(value)
    assert 'cntvct_el0' not in disassemblies[0]
    assert disassemblies[1].count('cntvct_el0')==1
    result=dict(functions=sources,forbidden_mutations=len(mutations),plain_layout=plain,timed_layout=timed,
                images={str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in (a.plain,a.timed)},
                disabled_irq_counter_reads=0,enabled_irq_counter_reads=1)
    (a.output/'results.json').write_text(json.dumps(result,indent=2)+'\n')
    print('PASS GPU hot path: no message hop, delay or mapping update; timing adds zero fields or counter reads when disabled')


if __name__=='__main__':
    main()
