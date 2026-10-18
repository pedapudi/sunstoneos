#!/usr/bin/env python3
"""Check the private DMA image's EL1t exception routes in actual instructions.

System registers and exception entry are modeled. The check seeds one CPU's
registers and stacks; it does not execute boot, the scheduler or physical IRQs.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def execute(image, model_path):
    spec = importlib.util.spec_from_file_location('cached_model', model_path)
    model = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(model)
    machine = model.Machine(image)
    cpu = 3
    u = machine.cpus[cpu]
    state = machine.regs[cpu]
    vector = machine.symbols['a733_cached_el1_vectors']
    cold = machine.symbols['a733_cached_cold_fault']
    exception_sp = machine.symbols['a733_cpu_el1_stacks'] + (cpu + 1) * 32768
    task_sp = machine.symbols['a733_cpu_el1_stacks'] + 32768
    resume = machine.symbols['a733_smp_entry']
    initial = [0x1234000000000000 + index for index in range(31)]

    def branch(address):
        ins = next(machine.decoder.disasm(machine.read(address, 4), address))
        assert ins.mnemonic == 'b', (hex(address), ins.mnemonic)
        return int(ins.op_str.removeprefix('#'), 0)

    # All runtime table groups retain 128-byte slots. EL1t shares the existing
    # EL1h/EL0 handlers; unsupported classes and the cold table still fault.
    sync, irq = branch(vector), branch(vector + 128)
    for offset in (0, 0x200, 0x400):
        assert branch(vector + offset) == sync
        assert branch(vector + offset + 128) == irq
        assert branch(vector + offset + 256) == cold
        assert branch(vector + offset + 384) == cold
    for slot in range(12, 16):
        assert branch(vector + slot * 128) == cold
    for slot in range(16):
        assert branch(machine.symbols['a733_cached_cold_vectors'] + slot * 128) == cold

    cases = []

    def run(name, old_el, esr, target, is_irq=False, slot=None, returns=False):
        state.update(currentel=old_el * 4, spsel=0, daif=0x140,
                     sp_el0=task_sp, sp_el1=exception_sp,
                     vbar_el1=vector, tpidr_el1=cpu)
        u.reg_write(model.UC_ARM64_REG_SP, task_sp)
        u.reg_write(model.UC_ARM64_REG_NZCV, 0xa0000000)
        for reg, value in zip(model.GPR, initial):
            u.reg_write(reg, value)
        machine.exception(cpu, 1, resume, esr=esr, irq=is_irq)
        if slot is not None:
            u.reg_write(model.UC_ARM64_REG_PC, vector + slot * 128)
        branches = 0
        instructions = 0
        for _ in range(400):
            pc = u.reg_read(model.UC_ARM64_REG_PC)
            if pc == target:
                break
            ins = next(machine.decoder.disasm(machine.read(pc, 4), pc))
            branches += int(ins.mnemonic == 'subs' and ins.op_str == 'x16, x16, #1')
            u.emu_start(pc, 0, count=1)
            instructions += 1
        else:
            raise AssertionError((name, 'instruction bound', hex(pc)))
        expected = initial.copy()
        if returns:
            expected[0] = cpu
            assert state['currentel'] == old_el * 4 and state['spsel'] == 0
            assert state['daif'] == 0x140
            assert u.reg_read(model.UC_ARM64_REG_NZCV) == 0xa0000000
            assert state['sp_el1'] == exception_sp
            assert u.reg_read(model.UC_ARM64_REG_SP) == task_sp
        else:
            assert state['currentel'] == 4 and state['spsel'] == 1
            assert state['spsr_el1'] & 15 == old_el * 4
            assert u.reg_read(model.UC_ARM64_REG_SP) == exception_sp
        assert state['sp_el0'] == task_sp
        assert [u.reg_read(reg) for reg in model.GPR] == expected, name
        assert branches == (0 if slot is not None else 24), (name, branches)
        cases.append({'case': name, 'instructions': instructions,
                      'BHB_iterations': branches, 'destination': hex(target)})

    run('EL1t core-ID SVC returns on SP_EL0', 1, (0x15 << 26) | 109, resume, returns=True)
    run('EL1t IRQ reaches existing FreeRTOS handler', 1, 0,
        machine.symbols['FreeRTOS_IRQ_Handler'], is_irq=True)
    run('EL1t non-SVC synchronous exception faults', 1, 0x25 << 26, cold)
    run('EL1t exit SVC reaches existing exit handler', 1, (0x15 << 26) | 119,
        machine.symbols['a733_smp_exit_privileged'])
    run('EL1t FIQ faults', 1, 0, cold, slot=2)
    run('EL1t SError faults', 1, 0, cold, slot=3)
    run('EL0 core-ID SVC returns without privilege elevation', 0,
        (0x15 << 26) | 109, resume, returns=True)
    return {'passed': True, 'cases': cases,
            'limits': 'Modeled exception entry and system registers; no boot, scheduler, '
                      'IRQ handler body, physical interrupt delivery or DMA execution.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    evidence = args.evidence.resolve()
    evidence.mkdir(parents=True, exist_ok=False)
    inputs = {'image.elf': args.image.resolve(), 'executed-check.py': Path(__file__),
              'cached-model.py': ROOT / 'checks/a733-smp-cached.py',
              'continue.patch': ROOT / 'apps/dma-copy/continue.patch'}
    for name, path in inputs.items():
        shutil.copy2(path, evidence / name)
    (evidence / 'inputs.json').write_text(json.dumps({name: {'path': str(path),
        'sha256': sha(evidence / name)} for name, path in inputs.items()}, indent=2) + '\n')
    try:
        result = execute(evidence / 'image.elf', evidence / 'cached-model.py')
    except Exception as error:
        (evidence / 'failure.txt').write_text(repr(error) + '\n')
        raise
    (evidence / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS private DMA EL1t exception routes and EL0 return privilege')


if __name__ == '__main__':
    main()
