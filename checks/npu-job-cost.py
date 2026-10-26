#!/usr/bin/env python3
"""Count the CPU work of one NPU job in a linked finite image, without hardware.

Runs the image's own constructor, bootstrap publication, SUBMIT send, job
publication and job completion callbacks in Unicorn and counts, per phase,
the instructions executed, the bytes hashed, the address translations and
the cache lines maintained. It also counts the instructions, translations
and barriers of one native register read. The scheduler, the application's
worker check, translations (AT), cache maintenance (DC) and the counter are
modeled at their function boundaries; their instructions are not counted.
Instruction counts are not board times.
"""
import argparse
import hashlib
import importlib.util
import json
import struct
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_BLOCK
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('harness', ROOT / 'checks/harness.py')
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)
STOP = 0x43fff000
TASK = 0x43f00100
REGISTERS = 0x03600000
DSB_SY, DMB = 0xd5033f9f, 0xd50330bf


class Machine:
    def __init__(self, path):
        self.image = harness.Image(path, layout_names=(
            'a733_npu_reserved', 'npu_convolution', 'a733_npu_native', 'a733_npu_memory',
            'a733_npu_command', 'a733_npu_startup'))
        u = self.u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        u.mem_map(0x03000000, 0x01000000)
        u.mem_map(0x40000000, 0x04000000)
        for at, data in self.image.parts:
            if data:
                u.mem_write(at, data)
        self.counting = False
        self.reset()
        self.scheduler = 1
        self.ticks = 1000
        hooks = {'aarch64_device_translate': self.translate, 'aarch64_device_cache': self.cache,
                 'aarch64_device_cache_stride': lambda a: self.ret(64),
                 'xTaskGetSchedulerState': lambda a: self.ret(self.scheduler),
                 'xTaskGetCurrentTaskHandle': lambda a: self.ret(TASK),
                 'vPortEnterCritical': lambda a: self.ret(), 'vPortExitCritical': lambda a: self.ret(),
                 'npu_service_send': self.send}
        for name, fn in hooks.items():
            if self.image.symbols.get(name):
                at = self.image.address(name)
                u.hook_add(UC_HOOK_CODE, self.wrap(fn), begin=at, end=at)
        # The application's per-callback worker check is unchanged by the
        # measured work; it returns admitted here.
        at = self.image.address('worker', 'apps/npu-convolution/native.c')
        u.hook_add(UC_HOOK_CODE, self.wrap(lambda a: self.ret(1)), begin=at, end=at)
        at = self.image.address('digest', 'drivers/npu/npu_convolution.c')
        u.hook_add(UC_HOOK_CODE, self.digested, begin=at, end=at)
        for base, data in self.image.parts:
            for offset in range(0, len(data) - 3, 4):
                word = int.from_bytes(data[offset:offset + 4], 'little')
                if word & 0xffffffe0 in (0xd53be000, 0xd53be020):  # CNTFRQ_EL0, CNTPCT_EL0
                    u.hook_add(UC_HOOK_CODE, self.counter, begin=base + offset, end=base + offset)
                if word in (DSB_SY,) or word & 0xfffff0ff == 0xd50330bf:
                    u.hook_add(UC_HOOK_CODE, self.barrier, begin=base + offset, end=base + offset)
        u.hook_add(UC_HOOK_BLOCK, self.block)

    def reset(self):
        self.stats = {'instructions': 0, 'hashed_bytes': 0, 'translations': 0, 'cache_lines': 0,
                      'cache_calls': [], 'dsb_sy': 0, 'dmb': 0}

    def get(self, at, n=8):
        return int.from_bytes(self.u.mem_read(at, n), 'little')

    def put(self, at, value, n=8):
        self.u.mem_write(at, int(value).to_bytes(n, 'little'))

    def ret(self, value=0):
        self.u.reg_write(UC_ARM64_REG_X0, value)
        self.u.reg_write(UC_ARM64_REG_PC, self.u.reg_read(UC_ARM64_REG_LR))

    def wrap(self, fn):
        def hook(cpu, at, size, cookie):
            fn([self.u.reg_read(UC_ARM64_REG_X0 + i) for i in range(4)])
        return hook

    def block(self, cpu, at, size, cookie):
        if self.counting:
            self.stats['instructions'] += size // 4

    def translate(self, a):
        self.u.mem_write(a[2], struct.pack('<QII', a[0], 0 if REGISTERS <= a[0] < REGISTERS + 4096 else 255, 3))
        self.stats['translations'] += 1
        self.ret(1)

    def cache(self, a):
        self.stats['cache_lines'] += (a[2] + 63) // 64
        self.stats['cache_calls'].append([a[0], hex(a[1]), a[2]])
        self.ret(1)

    def send(self, a):
        ticket = 7
        self.put(a[3], ticket)
        self.ret(1)

    def digested(self, cpu, at, size, cookie):
        self.stats['hashed_bytes'] += self.u.reg_read(UC_ARM64_REG_X0 + 1)

    def counter(self, cpu, at, size, cookie):
        word = self.get(at, 4)
        value = 24000000 if word & 0xffffffe0 == 0xd53be000 else self.ticks
        self.ticks += 24
        self.u.reg_write(UC_ARM64_REG_X0 + (word & 31), value)
        self.u.reg_write(UC_ARM64_REG_PC, at + 4)

    def barrier(self, cpu, at, size, cookie):
        if self.counting:
            self.stats['dsb_sy' if self.get(at, 4) == DSB_SY else 'dmb'] += 1

    def call(self, name, *args, unit=None, count=None):
        self.u.reg_write(UC_ARM64_REG_SP, 0x43fd0000)
        self.u.reg_write(UC_ARM64_REG_LR, STOP)
        for i, value in enumerate(args):
            self.u.reg_write(UC_ARM64_REG_X0 + i, value)
        self.counting = count is not None
        if count is not None:
            self.reset()
        self.u.emu_start(self.image.address(name, unit), STOP, count=400000000)
        self.counting = False
        assert self.u.reg_read(UC_ARM64_REG_PC) == STOP, name
        result = self.u.reg_read(UC_ARM64_REG_X0)
        if count is not None:
            stats = dict(self.stats)
            stats.pop('cache_calls') if len(stats['cache_calls']) > 16 else None
            count[name if unit is None else name + ' (' + unit + ')'] = stats
        return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    m = Machine(a.image)
    image = m.image
    phases = {}
    if image.symbols.get('a733_memory_reservations_prepare'):
        assert m.call('a733_memory_reservations_prepare') == 1
    # The application object holds the reserved NPU allocation; resolve its
    # private type from DWARF, as the existing NN-pool check does.
    from elftools.elf.elffile import ELFFile
    with a.image.open('rb') as stream:
        offsets = []
        for cu in ELFFile(stream).get_dwarf_info().iter_CUs():
            if not cu.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith('/apps/npu-convolution/native.c'):
                continue
            for die in cu.iter_DIEs():
                if (die.tag == 'DW_TAG_variable' and die.attributes.get('DW_AT_name') and
                        die.attributes['DW_AT_name'].value == b'app' and 'DW_AT_location' in die.attributes):
                    kind = die.get_DIE_from_attribute('DW_AT_type')
                    offsets.extend(d.attributes['DW_AT_data_member_location'].value for d in kind.iter_children()
                                   if d.tag == 'DW_TAG_member' and d.attributes['DW_AT_name'].value == b'reserved')
    assert len(offsets) == 1
    reserved = image.address('app', 'apps/npu-convolution/native.c') + offsets[0]
    rl, cl = image.layouts['a733_npu_reserved'], image.layouts['npu_convolution']
    nl, ml = image.layouts['a733_npu_native'], image.layouts['a733_npu_memory']
    model = image.address('a733_npu_application_model')
    model_bytes = m.get(image.address('a733_npu_application_model_bytes'))
    c = m.call('a733_npu_reserved_prepare', reserved, 1, model, model_bytes, REGISTERS, STOP, 0, count=phases)
    assert c == reserved + rl['convolution']
    native = c + cl['native']
    memory = native + nl['memory']
    image_at = memory + ml['image']
    commands = c + cl['commands']
    stride = 24
    startup = 0x43e00000
    m.put(startup + image.layouts['a733_npu_startup']['window_open'], 1, 4)
    m.scheduler = 2
    for field, value, width in (('startup', startup, 8), ('client', TASK, 8), ('bound', 1, 4),
                                ('opened', 1, 4), ('ready', 1, 4)):
        m.put(c + cl[field], value, width)
    publish, finish = ('publish', 'drivers/npu/npu_convolution.c'), ('finish', 'drivers/npu/npu_convolution.c')
    boot = 0x43e01000
    m.u.mem_write(boot, bytes(m.u.mem_read(commands, stride)))
    phases['bootstrap'] = {}
    assert m.call(*publish[:1], c, image_at, boot, 1, unit=publish[1], count=phases['bootstrap']) == 1
    assert m.call(*finish[:1], c, boot, 0, unit=finish[1], count=phases['bootstrap']) == 1
    input_at, input_bytes = image.address('a733_npu_application_input'), m.get(image.address('a733_npu_application_input_bytes'))
    jobs = []
    for job in range(2):
        counts = {}
        # The service's READY reply sets ready; REAP clears job.
        m.put(c + cl['job'], 0)
        m.put(c + cl['ready'], 1, 4)
        sent = m.call('npu_convolution_send', c, 2, input_at, input_bytes, 1000000, 5, 0x43e02000, count=counts)
        assert sent == 1, {k: m.get(c + cl[k], 4) for k in ('failed', 'closed', 'ready', 'opened', 'bound', 'prepared')} | {
            'memory_state': m.get(memory + ml['state'], 4), 'job': m.get(c + cl['job']), 'pending': m.get(c + cl['pending'])}
        m.put(c + cl['pending'], 0)
        command = 0x43e01100
        m.u.mem_write(command, bytes(m.u.mem_read(commands + stride, stride)))
        assert m.call(*publish[:1], c, image_at, command, 0, unit=publish[1], count=counts) == 1
        assert m.call(*finish[:1], c, command, 0, unit=finish[1], count=counts) == 1
        jobs.append(counts)
    access = {}
    value_at = 0x43e03000
    m.call('read32', native, 4, value_at, unit='platform/a733/npu_native.c', count=access)
    result = {'image_sha256': hashlib.sha256(a.image.read_bytes()).hexdigest(), 'physical_access': False,
              'constructor_and_bootstrap': phases, 'jobs': jobs, 'register_read': access,
              'scope': __doc__.strip()}
    a.output.mkdir(parents=True, exist_ok=True)
    (a.output / 'job-cost.json').write_text(json.dumps(result, indent=1) + '\n')
    for i, counts in enumerate(jobs):
        for name, stats in counts.items():
            print(f'job {i + 1} {name}: {stats["instructions"]} instructions, {stats["hashed_bytes"]} bytes hashed, '
                  f'{stats["translations"]} translations, {stats["cache_lines"]} cache lines, '
                  f'{stats["dsb_sy"]} DSB SY')
    for name, stats in access.items():
        print(f'register read: {stats["instructions"]} instructions, {stats["translations"]} translations, '
              f'{stats["dsb_sy"]} DSB SY, {stats["dmb"]} DMB')


if __name__ == '__main__':
    main()
