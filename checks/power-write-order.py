#!/usr/bin/env python3
"""Execute native PMIC and legacy USB completion barriers with posted stores."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from harness import CACHE, CLANG, Image, LLD, environment, instruction_sites, refused
from register_latency import PostedWrites, RegisterLatency
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0
from elftools.elf.elffile import ELFFile
GATE_RUNS = [['--output', '{output}']]

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('--reuse-build', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
commands, inputs, results = [], {}, []
machines = []
fr = CACHE/'sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2'
tiny = CACHE/'sources/tinyusb-d668b0abb7219b9ce324fa88d46604bb9e869973'


def keep(path):
    path = Path(path).resolve()
    data = path.read_bytes()
    dest = out/'inputs'/str(path).lstrip('/')
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)
    inputs[str(path)] = dict(sha256=hashlib.sha256(data).hexdigest(), snapshot=str(dest))


def run(command):
    p = subprocess.run([str(x) for x in command], capture_output=True, text=True, env=environment())
    commands.append(dict(command=[str(x) for x in command], returncode=p.returncode,
                         stdout=p.stdout, stderr=p.stderr))
    (out/'commands.json').write_text(json.dumps(commands, indent=2)+'\n')
    assert p.returncode == 0, p.stderr


flags = ['--target=aarch64-none-elf', '-std=c11', '-O2', '-g', '-ffreestanding',
         '-fno-builtin', '-fno-stack-protector', '-ffunction-sections', '-fdata-sections',
         '-mno-outline-atomics', '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror',
         '-DINCLUDE_xTaskGetSchedulerState=1', '-DINCLUDE_xTaskGetCurrentTaskHandle=1',
         '-DINCLUDE_xSemaphoreGetMutexHolder=1', '-DSUNSTONE_A733_USB=1']
incs = [root/'include', root/'runtime/include', root/'platform/a733', root/'drivers/usb',
        fr/'include', fr/'portable/GCC/ARM_AARCH64_SRE', tiny/'src']
support = out/'support.c'
support.write_text('#include "FreeRTOS.h"\n#include "task.h"\n'
                   '#include "sunstone_deadline.h"\n'
                   'void vTaskDelay(TickType_t ticks){(void)ticks;}\n'
                   'int sunstone_deadline_wait_until(uint64_t until){(void)until;return 0;}\n'
                   'void platform_puts(const char *text){(void)text;}\n'
                   '_Noreturn void platform_panic(const char *text,unsigned line)'
                   '{(void)text;(void)line;for(;;)__asm__ volatile("brk #0");}\n')
pmic = out/'pmic.c'
pmic.write_text('#include "'+str(root/'drivers/power/a733_pmic_native.c')+'"\n'
                'static struct a733_platform_locks locks;\n'
                'static struct a733_pmic_native native={.locks=&locks,.holding=1};\n'
                'uint32_t observed;\n'
                'int issue(uintptr_t address,uint32_t value){return write32(&native,address,value);}\n'
                'int observe(uintptr_t address){return read32(&native,address,&observed);}\n')


def load(elf):
    image = Image(elf)
    with elf.open('rb') as stream:
        table = ELFFile(stream).get_section_by_name('.symtab')
        image.sizes = {s.name: int(s['st_size']) for s in table.iter_symbols()}
    return image


def build(name, source, entries):
    if args.reuse_build and (args.reuse_build/(name+'.elf')).exists():
        base = args.reuse_build.resolve()
        elf = base/(name+'.elf')
        for dep in base.glob(name+'-*.d'):
            for path in dep.read_text().replace('\\\n', ' ').split(':', 1)[1].split():
                path = Path(path).resolve()
                assert path.read_bytes() == (base/'inputs'/str(path).lstrip('/')).read_bytes(), path
                keep(path)
        return load(elf), elf
    objects = []
    for index, unit in enumerate([source, support]):
        obj = out/f'{name}-{index}.o'
        dep = obj.with_suffix('.d')
        run([CLANG, *flags, *('-I'+str(p) for p in incs), '-MD', '-MF', dep, '-c', unit, '-o', obj])
        objects.append(obj)
        for path in dep.read_text().replace('\\\n', ' ').split(':', 1)[1].split():
            keep(path)
    script = out/(name+'.ld')
    script.write_text('ENTRY('+entries[0]+')\nSECTIONS { . = 0x40000000; '
                      '.text : { *(.text*) } .rodata : { *(.rodata*) } '
                      '.data : { *(.data*) } .bss : { *(.bss*) *(COMMON) } }\n')
    elf = out/(name+'.elf')
    run([LLD, '--gc-sections', *('-u'+n for n in entries), '-T', script, *objects, '-o', elf])
    keep(script)
    return load(elf), elf


pmic_image, pmic_elf = build('pmic', pmic, ['issue', 'observe'])
usb_image, usb_elf = build('usb', root/'drivers/usb/a733_usb.c', ['a733_usb_prepare'])
mutant = out/'usb-no-release-hold.c'
original = (root/'drivers/usb/a733_usb.c').read_text()
assert original.count('hold_us(40);') == 1
mutant.write_text(original.replace('hold_us(40);', 'hold_us(0);'))
mutant_image, mutant_elf = build('usb-no-release-hold', mutant, ['a733_usb_prepare'])


class Machine:
    def __init__(self, image, variant='control'):
        machines.append(self)
        self.image, self.variant = image, variant
        self.cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        for address, size in [(0x40000000, 0x800000), (0x02000000, 0x200000),
                              (0x04100000, 0x1000), (0x07083000, 0x1000)]:
            self.cpu.mem_map(address, size)
        for address, data in image.parts:
            self.cpu.mem_write(address, data)
        self.cpu.reg_write(UC_ARM64_REG_SP, 0x407ff000)
        hooks = set(instruction_sites(image.parts, lambda op:
                    op == 0xd5033f9f or op & ~31 == 0xd53be020))
        for name in ('platform_panic', 'vTaskDelay','sunstone_deadline_wait_until'):
            hooks.update(image.symbols.get(name, set()))
        for site in hooks:
            self.cpu.hook_add(UC_HOOK_CODE, self.code, begin=site, end=site)
        self.cpu.hook_add(UC_HOOK_MEM_WRITE, self.write)
        self.cpu.hook_add(UC_HOOK_MEM_READ, self.read)
        # Units are 1/24 ns: a raw 24 MHz counter tick is 1000 units.
        self.time = 100023*24
        self.words = PostedWrites({0x07083008: 0, 0x0708300c: 0x40, 0x07083010: 0xf8,
                                   0x04100040: 1 << 6, 0x02003300: 0,
                                   0x02003304: 1 << 24, 0x04100420: 0, 0x04100410: 8})
        self.barriers, self.writes, self.samples = 0, [], 0
        self.origins, self.panic, self.event = {}, False, None
        self.last_store, self.mutations = None, 0
        self.fault_store_count = None
        # Mutate native opcodes before execution. Every case uses the same
        # unmodified DSB decoder/completion hook below.
        sites = sorted(instruction_sites(image.parts, lambda op: op == 0xd5033f9f))
        if variant == 'no-data-dsb':
            start = image.address('issue')
            sites = [s for s in sites if start <= s < start+image.sizes['issue']]
            assert len(sites) == 1
            self.cpu.mem_write(sites[0], bytes.fromhex('1f2003d5'))  # NOP
            self.mutations = 1
        elif variant in ('no-entry-dsb', 'no-release-dsb'):
            start = image.address('a733_usb_prepare')
            sites = [s for s in sites if start <= s < start+image.sizes['a733_usb_prepare']]
            assert len(sites) == 6, sites
            selected = 0 if variant == 'no-entry-dsb' else 3
            self.cpu.mem_write(sites[selected], bytes.fromhex('1f2003d5'))
            self.mutations = 1

    def ret(self, value=0):
        self.cpu.reg_write(UC_ARM64_REG_X0, value)
        self.cpu.reg_write(UC_ARM64_REG_PC, self.cpu.reg_read(UC_ARM64_REG_LR))

    def complete(self):
        self.barriers += 1
        for address, value in self.words.complete():
            if address == 0x04100040:
                self.origins['detach'] = self.time
            if address == 0x02003304:
                self.origins['release' if value & (1 << 24) else 'assert'] = self.time
            if address == 0x0708300c:
                self.event = RegisterLatency(self.time, 500000*24, 255, 0x18)

    def code(self, cpu, address, size, _):
        op = int.from_bytes(cpu.mem_read(address, 4), 'little')
        if address == self.image.symbols.get('platform_panic', {None}).copy().pop():
            self.panic = True
            self.fault_store_count = len(self.writes)
            cpu.emu_stop()
            return
        if address == self.image.symbols.get('vTaskDelay', {None}).copy().pop():
            ticks = cpu.reg_read(UC_ARM64_REG_X0)
            period = 1000000*24
            if self.variant != 'frozen':
                self.time = (self.time//period+ticks)*period
            self.ret()
            return
        if address == self.image.symbols.get('sunstone_deadline_wait_until', {None}).copy().pop():
            assert not self.words.pending, 'hold timestamp before completed store'
            if self.variant=='deadline-error':self.ret((1<<64)-1)
            else:
                self.time=max(self.time,cpu.reg_read(UC_ARM64_REG_X0)*1000*24)
                self.ret(1)
            return
        if op & ~31 == 0xd53be020:  # MRS CNTPCT_EL0
            assert not self.words.pending, 'hold timestamp before completed store'
            self.samples += 1
            if self.samples == 1:
                self.first_counter = self.time
            if self.variant == 'backward' and self.samples == 2:
                self.time = self.first_counter-1000
            value = self.time//1000
            if self.variant != 'frozen':
                self.time += 1000
            if self.variant == 'overflow':
                value = (1 << 64)-2
            cpu.reg_write(UC_ARM64_REG_X0+(op & 31), value)
            cpu.reg_write(UC_ARM64_REG_PC, address+4)
            return
        if op == 0xd5033f9f:  # DSB SY; identical decoder for control and mutants
            self.complete()
            cpu.reg_write(UC_ARM64_REG_PC, address+4)

    def write(self, cpu, access, address, size, value, _):
        if address not in self.words.words:
            return
        assert not self.words.pending, 'dependent write before completed store'
        if address == 0x0708300c:
            assert self.words.read(0x07083008) == 0x6c, 'CNTR before DATA'
        if address == 0x02003300:
            assert self.time-self.origins['detach'] >= 150000000*24, 'short detach hold'
        if address == 0x02003304 and value & (1 << 24):
            assert self.time-self.origins['assert'] >= 1000000*24, 'short reset hold'
        if address in (0x04100420, 0x04100410):
            assert self.time-self.origins['release'] >= 40000*24, 'short release hold'
        self.writes.append((address, value))
        self.last_store = address
        # The callback has a costly fractional completion; endpoint application
        # is separate from this architectural posted transaction.
        self.time += 25023*24
        self.words.write(address, value)

    def read(self, cpu, access, address, size, value, _):
        if address not in self.words.words:
            return
        assert not self.words.pending, 'dependent read before completed store'
        if self.event and address == 0x07083010:
            self.words.words[address] = self.event.apply(self.time, self.words.read(address))
        cpu.mem_write(address, self.words.read(address).to_bytes(size, 'little'))

    def call(self, name, *arguments):
        self.cpu.reg_write(UC_ARM64_REG_LR, 0x407fe000)
        for index, value in enumerate(arguments):
            self.cpu.reg_write(UC_ARM64_REG_X0+index, value)
        self.cpu.emu_start(self.image.address(name), 0x407fe000, count=50000000)
        assert self.panic or self.cpu.reg_read(UC_ARM64_REG_PC) == 0x407fe000, 'instruction budget exhausted'
        return self.cpu.reg_read(UC_ARM64_REG_X0)


def pmic_case(variant):
    m = Machine(pmic_image, variant)
    assert m.call('issue', 0x07083008, 0x6c) == 1
    assert m.call('issue', 0x0708300c, 0x48) == 1
    assert m.call('observe', 0x07083010) == 1
    observed = pmic_image.address('observed')
    assert int.from_bytes(m.cpu.mem_read(observed, 4), 'little') == 0xf8
    assert m.event.pending, 'DSB incorrectly completed the bus event'
    m.time += 500000*24
    assert m.call('observe', 0x07083010) == 1
    assert int.from_bytes(m.cpu.mem_read(observed, 4), 'little') == 0x18
    assert m.barriers == 2 and len(m.writes) == 2
    return m


control = pmic_case('control')
results.append(dict(case='pmic-control', result='PASS', barriers=control.barriers))
reason = refused(lambda: pmic_case('no-data-dsb'), 'dependent write before completed store', AssertionError)
assert machines[-1].mutations == 1
results.append(dict(case='pmic-missing-data-dsb', result='PASS', refused=reason))


def usb_case(variant):
    m = Machine(mutant_image if variant == 'no-release-hold' else usb_image, variant)
    # The actual TinyUSB caller's preceding SoftConn-clear byte store remains
    # pending on entry; the real entry DSB must complete it before detach time.
    m.words.write(0x04100040, 0)
    m.call('a733_usb_prepare')
    if variant in ('deadline-error', 'backward', 'overflow'):
        assert m.panic and not m.writes and m.fault_store_count == 0
    else:
        assert not m.panic and len(m.writes) == 5 and m.barriers == 6
        assert not m.words.pending
    return m


for variant in ['control', 'deadline-error', 'backward', 'overflow']:
    m = usb_case(variant)
    results.append(dict(case='usb-'+variant, result='PASS', barriers=m.barriers,
                        counter_samples=m.samples, writes=len(m.writes), panic=m.panic))
for variant, reason in [('no-entry-dsb', 'hold timestamp before completed store'),
                        ('no-release-dsb', 'hold timestamp before completed store'),
                        ('no-release-hold', 'short release hold')]:
    message = refused(lambda: usb_case(variant), reason, AssertionError)
    results.append(dict(case='usb-'+variant, result='PASS', refused=message))

for path in [__file__, root/'checks/harness.py', root/'checks/register_latency.py', support, pmic]:
    keep(path)
(out/'inputs.json').write_text(json.dumps(inputs, indent=2)+'\n')
(out/'results.json').write_text(json.dumps(dict(result='PASS', cases=results,
    artifacts={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in [pmic_elf, usb_elf, mutant_elf]},
    limitations='Host AArch64 execution with modeled completion, precise waits and separate bus latency; no physical access or stopped-counter recovery qualification.'), indent=2)+'\n')
print(json.dumps(results, indent=2))
