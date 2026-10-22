#!/usr/bin/env python3
"""Measure idle-core wake-ups and idle instructions in the cached eight-core image.

The check stages a source tree, replaces the cached image's diagnostic with
checks/cpu-power-states-idle.c, optionally applies a patch to the staged tree,
and builds the image with the build function of checks/a733-smp-cached.py. It
then executes the image in that check's instruction model with one change:
WFI stops a core until an interrupt is pending for it, and WFI at EL0 without
SCTLR_EL1.nTWI fails the check, because the core would trap to EL1 with
exception class 0x01.

Over a 64-tick window in which no task is ready, the check counts SGIs sent to
cores 1 to 7, timer wake-ups of core 0 and instructions executed per core.
After the window, a notification from core 0 and then a two-tick delay must
each wake the task of every secondary core on that core. The check also reports the instruction counts of
xTaskIncrementTick and both idle tasks from the ELF symbol table.

The model has no A733 power, cache or interconnect timing. It shows which
interrupts the scheduler sends and which instructions the cores execute; it
proves no physical wake-up from WFI.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
from elftools.elf.elffile import ELFFile
from unicorn.arm64_const import UC_ARM64_REG_PC

ROOT = Path(__file__).resolve().parents[1]
WINDOW_TICKS = 64
SECONDARIES = range(1, 8)


def load_cached_check():
    spec = importlib.util.spec_from_file_location('cached', ROOT/'checks/a733-smp-cached.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


cached = load_cached_check()


def stage(source, evidence, patch):
    tree = evidence/'tree'
    shutil.copytree(source, tree, ignore=shutil.ignore_patterns('.git'))
    shutil.copy2(ROOT/'checks/cpu-power-states-idle.c', tree/'platform/a733-smp-cached/diagnostic.c')
    if patch:
        subprocess.run(['patch', '-p1', '--silent', '-d', str(tree), '-i', str(patch.resolve())], check=True)
    return tree


def build(source, evidence, freertos, patch):
    cached.ROOT = stage(source, evidence, patch)
    try:
        return cached.build(evidence/'image', freertos)
    finally:
        cached.ROOT = ROOT


def function_sizes(image):
    with image.open('rb') as f:
        symbols = {s.name: s['st_size']//4 for s in ELFFile(f).get_section_by_name('.symtab').iter_symbols()
                   if s['st_info']['type'] == 'STT_FUNC'}
    return {name: symbols.get(name) for name in ('xTaskIncrementTick', 'prvIdleTask', 'prvPassiveIdleTask')}


class Pending(set):
    """Pending interrupts of one core; adding one ends that core's WFI."""
    def __init__(self, machine, core):
        super().__init__()
        self.machine, self.core = machine, core

    def add(self, interrupt):
        if interrupt == 0:
            self.machine.sgis[self.core] += 1
        self.machine.waiting[self.core] = False
        super().add(interrupt)

    def remove(self, interrupt):
        taken = self.machine.taken[self.core]
        taken[interrupt] = taken.get(interrupt, 0)+1
        super().remove(interrupt)


class IdleMachine(cached.Machine):
    def __init__(self, image):
        super().__init__(image)
        self.pending = [Pending(self, c) for c in range(8)]
        self.waiting = [False]*8
        self.sgis, self.taken, self.instructions = [0]*8, [{} for _ in range(8)], [0]*8
        self.wfi = [0]*8
        self.phase_address = self.symbols['a733_cpu_power_phase']
        self.phase, self.snapshots = 0, {}

    def instruction(self, u, a, size, c):
        self.instructions[c] += 1
        if a not in self.decoded:
            self.decoded[a] = next(self.decoder.disasm(bytes(u.mem_read(a, 4)), a))
        if self.decoded[a].mnemonic == 'wfi':
            m = self.regs[c]
            # DDI0616A.a E3.2: EL0 WFI traps to EL1 (EC=0x01) unless nTWI is set.
            assert m['currentel'] or m['sctlr_el1'] & (1 << 16), ('EL0 WFI traps', c, hex(a))
            self.wfi[c] += 1
            if not self.pending[c]:
                self.waiting[c] = True
        super().instruction(u, a, size, c)

    def snapshot(self):
        return {'clock': self.clock, 'sgis': list(self.sgis), 'instructions': list(self.instructions),
                'timer': self.taken[0].get(30, 0), 'wfi': list(self.wfi), 'waiting': list(self.waiting)}

    def run(self):
        for quantum in range(100000):
            for index in range(8):
                c = (index+quantum) % 8
                u, m = self.cpus[c], self.regs[c]
                if not self.active[c]:
                    continue
                if not c and m['cntp_ctl_el0'] & 3 == 1 and self.clock >= m['cntp_cval_el0']:
                    self.pending[c].add(30)
                if self.waiting[c]:
                    continue
                if m['currentel'] <= 4 and not m['daif'] & 0x80 and m['icc_igrpen1_el1'] & 1 and \
                   m['icc_pmr_el1'] > 0xe0 and self.pending[c] and self.irq_active[c] is None:
                    self.exception(c, 1, u.reg_read(UC_ARM64_REG_PC), irq=True)
                try:
                    u.emu_start(u.reg_read(UC_ARM64_REG_PC), 0x42200000, count=(211, 347, 509, 673, 887)[(quantum+c) % 5])
                except Exception as ex:
                    raise RuntimeError((c, hex(u.reg_read(UC_ARM64_REG_PC)), self.clock, str(ex))) from ex
                phase = self.q(self.phase_address)
                if phase != self.phase:
                    assert phase == self.phase+1, ('phase order', self.phase, phase)
                    self.phase = phase
                    self.snapshots[phase] = self.snapshot()
                if self.q(0x42000000+24) == 5:
                    return self.summary()
            self.clock += 1000
        raise AssertionError(('instruction budget', self.phase, [hex(u.reg_read(UC_ARM64_REG_PC)) for u in self.cpus]))


def measure(image):
    machine = IdleMachine(image)
    end = machine.run()
    assert end['report'][4] == 0, ('image failed', end['report'])
    assert machine.phase == 3, machine.phase
    start, stop, end = machine.snapshots[1], machine.snapshots[2], machine.snapshots[3]
    ticks = stop['timer']-start['timer']
    window = {
        'ticks': ticks,
        'sgis_to_secondaries': sum(stop['sgis'][c]-start['sgis'][c] for c in SECONDARIES),
        'sgis_per_tick': sum(stop['sgis'][c]-start['sgis'][c] for c in SECONDARIES)/ticks,
        'instructions': [stop['instructions'][c]-start['instructions'][c] for c in range(8)],
        'core0_wakeups_per_second': ticks*24000000/(stop['clock']-start['clock']),
    }
    wake = {
        'secondaries_in_wfi_at_window_end': [stop['waiting'][c] for c in SECONDARIES],
        'sgis_to_secondaries': sum(end['sgis'][c]-stop['sgis'][c] for c in SECONDARIES),
    }
    return {'window': window, 'wake': wake, 'wfi_per_core': machine.wfi, 'sctlr_el1': [hex(r['sctlr_el1']) for r in machine.regs],
            'function_instructions': function_sizes(image)}


def check_budgets(result):
    window = result['window']
    assert WINDOW_TICKS <= window['ticks'] <= WINDOW_TICKS+1, window
    assert window['sgis_to_secondaries'] == 0, window
    assert all(window['instructions'][c] == 0 for c in SECONDARIES), window
    assert all(result['wfi_per_core']), result['wfi_per_core']
    # The image exits only after each secondary task ran after its notification
    # and after its delay; both wake-ups therefore ended a WFI.
    assert all(result['wake']['secondaries_in_wfi_at_window_end']), result['wake']


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--evidence', type=Path, required=True, help='new directory for the build and result')
    p.add_argument('--freertos', type=Path, required=True, help='pinned FreeRTOS-Kernel source')
    p.add_argument('--source', type=Path, default=ROOT, help='source tree to build (default: this tree)')
    p.add_argument('--patch', type=Path, help='patch applied with -p1 to the staged tree')
    p.add_argument('--record-only', action='store_true', help='report the metrics without the budgets')
    a = p.parse_args()
    evidence = a.evidence.resolve()
    evidence.mkdir(parents=True, exist_ok=False)
    image = build(a.source.resolve(), evidence, a.freertos.resolve(), a.patch)
    result = measure(image)
    result['source'], result['patch'] = str(a.source.resolve()), str(a.patch.resolve()) if a.patch else None
    (evidence/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result['window'] | {'wake': result['wake'], 'function_instructions': result['function_instructions']}))
    if not a.record_only:
        check_budgets(result)
        print('PASS idle secondaries wait in WFI with no SGI or instruction for 64 ticks and wake for work')


if __name__ == '__main__':
    main()
