#!/usr/bin/env python3
"""Locate first-task entry and its preceding EL1 RAM writes in the linked image.

Each CPU stops before its first EL0 instruction. This is preparation for a
returning physical diagnostic, not physical scheduling or cache qualification.
"""
import argparse
import importlib.util
import json
from pathlib import Path
from unicorn import UC_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('scheduler', ROOT/'checks/a733-supervised-scheduler.py')
scheduler = importlib.util.module_from_spec(spec)
spec.loader.exec_module(scheduler)


class FirstEntry(scheduler.Scheduler):
    def __init__(self, image):
        self.entries = {}
        self.written_pages = [set() for _ in range(8)]
        super().__init__(image)

    def memory(self, u, access, address, size, value, core):
        if access == UC_MEM_WRITE and self.regs[core]['currentel'] == 4:
            if 0x41000000 <= address and address + size <= 0x42000000:
                self.written_pages[core].update(range(address & ~4095,
                    ((address + size - 1) & ~4095) + 4096, 4096))
        return super().memory(u, access, address, size, value, core)

    def instruction(self, u, address, size, core):
        if self.regs[core]['currentel'] == 0:
            assert core not in self.entries
            tcb = self.q(self.symbols['pxCurrentTCBs'] + core * 8)
            idle = self.q(self.symbols['xIdleTaskHandles'] + core * 8)
            # tasks.c assigns each CPU its idle TCB before port startup.
            # A pending interrupt can select a worker before its first ERET.
            task = ('prvIdleTask' if core == 0 else 'prvPassiveIdleTask') if tcb == idle else 'worker'
            assert address == self.symbols[task], ('Unexpected first task', core, hex(address), task)
            assert self.reg(core, 'x0') == (0 if tcb == idle else core), ('Task argument', core)
            self.entries[core] = {'pc': address, 'sp': u.reg_read(UC_ARM64_REG_SP),
                'task': task, 'tcb': tcb,
                'argument': self.reg(core, 'x0'), 'sctlr_el1': self.regs[core]['sctlr_el1'],
                'daif': self.regs[core]['daif']}
            self.active[core] = False
            u.emu_stop()
            return
        return super().instruction(u, address, size, core)

    def run_to_entries(self):
        for quantum in range(10000):
            for index in range(8):
                core = (index + quantum) % 8
                if not self.active[core]:
                    continue
                u = self.cpus[core]
                r = self.regs[core]
                if core == 0 and r['cntp_ctl_el0'] & 3 == 1 and self.clock >= r['cntp_cval_el0']:
                    self.pending[core].add(30)
                u.emu_start(u.reg_read(UC_ARM64_REG_PC), 0x42200000,
                    count=(211, 347, 509, 673, 887)[(quantum + core) % 5])
            if len(self.entries) == 8:
                return
            self.clock += 1000
        raise AssertionError(('First-task entry budget', self.entries, self.summary()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    machine = FirstEntry(args.image)
    machine.run_to_entries()
    pages = sorted(set().union(*machine.written_pages))
    result = {'image_sha256': scheduler.model.sha(args.image), 'physical_access': False,
        'entries': machine.entries, 'el1_written_pages': pages,
        'per_core_el1_written_pages': [sorted(p) for p in machine.written_pages],
        'sources': {str(p.relative_to(ROOT)): scheduler.model.sha(p) for p in
            (Path(__file__), ROOT/'checks/a733-supervised-scheduler.py', ROOT/'checks/a733-smp-cached.py')},
        'limits': __doc__ + ' Observed pages are a lower bound for other execution interleavings; derive cleanup coverage from linker-owned ranges and actual callers.'}
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print('PASS eight first-task entries; observed EL1-written pages:', len(pages))


if __name__ == '__main__':
    main()
