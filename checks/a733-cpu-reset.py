#!/usr/bin/env python3
"""Check the shared CPU warm-reset terminal sequence in every caller."""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from elftools.elf.elffile import ELFFile
from harness import CLANG, environment, refused

ROOT = Path(__file__).resolve().parents[1]
ASSEMBLY = ['platform/a733/monitor.S', 'platform/a733/probe.S', 'checks/a733-aarch64.S',
            'boot/a733/native-startup-probe.S', 'boot/a733/a64-entry-probe.S',
            'boot/a733/primary-publication-probe.S', 'apps/cpu-runtime-qualification/terminal.S',
            'apps/npu-finite/return.S', 'apps/gpu-finite/return.S', 'apps/card-fel-recovery/entry.S']
C_CALLER = 'boot/a733/stage2/loader.c'
CHECK_INPUTS = ['include/a733_cpu_reset.h', *ASSEMBLY, C_CALLER]


def reset_wait(parts):
    """Require DSB/MSR/ISB/WFI/back-branch at each actual RMR instruction."""
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    count = 0
    for base, raw in parts:
        def instruction(offset):
            return next(decoder.disasm(raw[offset:offset+4], base+offset), None)
        for offset in range(0,len(raw)-3,4):
            ins = instruction(offset)
            if not ins or ins.mnemonic != 'msr' or not ins.op_str.startswith('rmr_el3,'):
                continue
            count += 1
            after = [instruction(offset+n)for n in (4,8,12)]
            assert after[0] and after[0].mnemonic == 'isb' and after[0].op_str in ('','sy'), 'reset needs immediate ISB'
            assert after[1] and after[1].mnemonic == 'wfi', 'reset needs immediate WFI after ISB'
            assert after[2] and after[2].mnemonic == 'b' and \
                int(after[2].op_str.lstrip('#'),0) == base+offset+8, 'reset needs branch back to WFI'
            before = instruction(offset-4) if offset>=4 else None
            assert before and (before.mnemonic,before.op_str) == ('dsb','sy'), 'reset needs immediate DSB before MSR'
    assert count, 'no CPU reset instruction'
    return count


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    cases=[]
    # These sources must invoke the one body; an inline copy defeats this check.
    for name in (*ASSEMBLY,C_CALLER):
        text=(ROOT/name).read_text()
        call='a733_cpu_reset_aarch32()' if name==C_CALLER else 'A733_CPU_RESET_WAIT(x0)'
        assert text.count(call)==1 and not re.search(r'msr\s+rmr_el3',text),name
        cases.append('shared-body-'+name)
    # Only the generated gate offset and binary are needed to assemble the
    # recovery caller. Its actual linked ELF is checked by card-fel-recovery.py.
    (args.output/'gate.bin').write_bytes(bytes(4))
    (args.output/'gate-info.h').write_text('#define CARD_FEL_TEST_OFFSET 0\n')
    c=args.output/'reset.c'
    c.write_text('#include "a733_cpu_reset.h"\n_Noreturn void reset(void){a733_cpu_reset_aarch32();}\n')
    for index,source in enumerate([*(ROOT/name for name in ASSEMBLY),c]):
        label=source.relative_to(ROOT).as_posix() if source.is_relative_to(ROOT) else 'stage2-C-reset'
        obj=args.output/('caller-%d.o'%index)
        command=[str(CLANG),'--target=aarch64-none-elf','-march=armv8-a','-O2','-ffreestanding',
                 '-I',str(ROOT/'include'),'-I',str(args.output),
                 '-DCARD_FEL_GATE_BINARY="'+str(args.output/'gate.bin')+'"','-c',str(source),'-o',str(obj)]
        subprocess.run(command,check=True,capture_output=True,env=environment())
        with obj.open('rb')as stream:
            elf=ELFFile(stream)
            parts=[(int(s['sh_addr']),s.data())for s in elf.iter_sections()if s['sh_flags']&4 and s['sh_size']]
        assert reset_wait(parts)==1,str(source)
        cases.append('compiled-sequence-'+label)
        for delta,reason in ((8,'immediate WFI'),(12,'branch back to WFI')):
            changed=[]
            decoder=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
            for base,raw in parts:
                raw=bytearray(raw)
                for offset in range(0,len(raw)-3,4):
                    ins=next(decoder.disasm(bytes(raw[offset:offset+4]),base+offset),None)
                    if ins and ins.mnemonic=='msr' and ins.op_str.startswith('rmr_el3,'):
                        struct.pack_into('<I',raw,offset+delta,0xd503201f)  # Omit WFI or its loop branch.
                changed.append((base,bytes(raw)))
            refused(lambda:reset_wait(changed),reason,kind=AssertionError)
            cases.append('mutation-missing-%d-'%delta+label)
    assert len(cases)==len(set(cases)), 'reset case names must identify their caller'
    (args.output/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'cases':cases},indent=2)+'\n')
    print('PASS shared CPU reset: %d cases'%len(cases))


if __name__=='__main__':main()
