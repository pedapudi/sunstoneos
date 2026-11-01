#!/usr/bin/env python3
"""Execute the native EL0 NPU doorbell exception path and adjacent SVCs.

Relink current queue and port instructions into an existing OS build. Unicorn
models exception entry/return, task identity, the admitted session and NPU MMIO.
The real EL0 vector, FreeRTOS dispatch, OS queue bridge and native IRQ refill
execute. This does not qualify EL0 memory isolation or physical NPU behavior.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn import arm64_const as reg
from harness import Image, environment, skip_crumbs

ROOT = Path(__file__).resolve().parents[1]
TYPES = {'ss_npu_queue', 'ss_npu_queue_backend', 'ss_npu_queue_handle', 'ss_npu_queue_entry',
         'npu_runtime_queue', 'npu_runtime', 'npu_runtime_slot', 'npu_queue', 'npu_queue_ops',
         'npu_queue_command', 'npu_queue_job', 'npu_service', 'a733_npu_native', 'os_session',
         'os_npu_session', 'a733_npu_command','a733_npu_startup','ss_npu_queue_group'}
UNITS = ('apps/os/npu-queue.c', 'runtime/npu_app_queue.c', 'drivers/npu/npu_runtime_queue.c',
         'drivers/npu/npu_queue.c')
X = [getattr(reg, f'UC_ARM64_REG_X{n}') for n in range(31)]
MASK = (1 << 64) - 1
METADATA = {}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def build_image(build, output):
    cache = (build / 'CMakeCache.txt').read_text()
    old = Path(re.search(r'^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$', cache, re.M)[1])
    cc = re.search(r'^CMAKE_C_COMPILER:(?:STRING|FILEPATH)=(.*)$', cache, re.M)[1]
    link = shlex.split((build / 'CMakeFiles/sunstone-a733-os.dir/link.txt').read_text())
    inputs, commands = {}, []
    for token in link:
        token = token.removeprefix('-Wl,-T,').removeprefix('--ld-path=')
        path = Path(token) if Path(token).is_absolute() else build / token
        if path.is_file():
            inputs[str(path)] = sha(path)

    def run(command, name):
        commands.append(list(map(str, command)))
        (output / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        result = subprocess.run(command, cwd=build, env={**environment(), 'TMPDIR': str(output / 'scratch')}, capture_output=True)
        (output / (name + '.log')).write_bytes(result.stdout + result.stderr)
        assert result.returncode == 0, ('build failed', name)

    def flags(target, kind):
        path = build / f'CMakeFiles/{target}.dir/flags.make'
        inputs[str(path)] = sha(path)
        text = path.read_text()
        result = []
        for name in ('DEFINES', 'INCLUDES', 'FLAGS'):
            values = shlex.split(re.search(r'^' + kind + '_' + name + r' = (.*)$', text, re.M)[1])
            for value in values:
                # Generated build includes stay in the accepted build tree.
                if str(build) not in value:
                    value = value.replace(str(old), str(ROOT))
                result.append(value)
        return result

    options = flags('sunstone-a733-os-npu', 'C')
    replacements = {}
    for unit in UNITS:
        obj = output / (Path(unit).stem + '.o')
        dep = obj.with_suffix('.d')
        run([cc, '--target=aarch64-none-elf', *options, '-Werror', '-MD', '-MF', str(dep),
             '-c', str(ROOT / unit), '-o', str(obj)], obj.stem)
        replacements[unit + '.obj'] = obj
        for token in shlex.split(dep.read_text().replace('\\\n', ' ').split(':', 1)[1]):
            if Path(token).is_file():
                inputs[token] = sha(token)
    freertos = Path(re.search(r'^FREERTOS_DIR:(?:PATH|STRING)=(.*)$', cache, re.M)[1])
    original = freertos / 'portable/GCC/ARM_CR82/portASM.S'
    patched = output / 'portASM.S'
    patched.write_bytes(original.read_bytes())
    patch = ROOT / 'patches/freertos-smp-asm.patch'
    run(['patch', '--batch', '--forward', str(patched), str(patch)], 'patch-port')
    inputs[str(original)], inputs[str(patch)] = sha(original), sha(patch)
    asm_flags = flags('sunstone-a733-os-core', 'ASM')
    asm = output / 'portASM.o'
    dep = asm.with_suffix('.d')
    run([cc, '--target=aarch64-none-elf', *asm_flags, '-MD', '-MF', str(dep), '-c', str(patched), '-o', str(asm)], 'portASM')
    for token in shlex.split(dep.read_text().replace('\\\n', ' ').split(':', 1)[1]):
        if Path(token).is_file():
            inputs[token] = sha(token)
    replacements['freertos-a733-smp-cached/portASM.S.obj'] = asm
    for index, token in enumerate(link):
        for suffix, obj in replacements.items():
            if token.endswith('/' + suffix):
                link[index] = str(obj)
                break
        if token.startswith('-Wl,-Map,'):
            link[index] = '-Wl,-Map,' + str(output / 'fixture.map')
    assert all(str(obj) in link for obj in replacements.values())
    elf = output / 'fixture.elf'
    link[link.index('-o') + 1] = str(elf)
    link += ['-Wl,--undefined=ss_npu_queue_submit']
    run(link, 'link')
    inputs[str(Path(__file__).resolve())] = sha(__file__)
    return elf, inputs


class Native:
    BASE, END, MMIO = 0x90000000, 0x903ff000, 0x03600000

    def __init__(self, path):
        cached = METADATA.get(path)
        if cached is not None:
            self.image,self.sizes,self.states,self.objects,memory = cached
        else:
            self.image = Image(path, TYPES)
            self.sizes, self.states, self.objects = {}, {}, {}
            with path.open('rb') as stream:
                elf = ELFFile(stream)
                for symbol in elf.get_section_by_name('.symtab').iter_symbols():
                    if symbol['st_info']['type']=='STT_OBJECT' and int(symbol['st_value']):
                        self.objects.setdefault(symbol.name,set()).add(int(symbol['st_value']))
                memory = [(int(p['p_vaddr']), int(p['p_memsz'])) for p in elf.iter_segments() if p['p_type'] == 'PT_LOAD']
                for cu in elf.get_dwarf_info().iter_CUs():
                    unit = cu.get_top_DIE().attributes['DW_AT_name'].value.decode()
                    for die in cu.iter_DIEs():
                        name = die.attributes.get('DW_AT_name')
                        if not name:
                            continue
                        name = name.value.decode()
                        if die.tag == 'DW_TAG_structure_type' and name in TYPES and 'DW_AT_byte_size' in die.attributes:
                            self.sizes[name] = die.attributes['DW_AT_byte_size'].value
                        if unit.endswith('runtime/npu_app_queue.c') and die.tag == 'DW_TAG_enumerator':
                            self.states[name] = die.attributes['DW_AT_const_value'].value
            METADATA[path] = self.image,self.sizes,self.states,self.objects,memory
        self.u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        pages = set()
        for start, size in [*memory, (self.BASE, 0x400000), (self.MMIO, 4096)]:
            pages.update(range(start & ~4095, (start + size + 4095) & ~4095, 4096))
        ordered=sorted(pages);start=previous=ordered[0]
        for page in ordered[1:]+[ordered[-1]+8192]:
            if page!=previous+4096:
                self.u.mem_map(start,previous+4096-start);start=page
            previous=page
        for address, data in self.image.parts:
            self.u.mem_write(address, data)
        self.ring = self.address('ring', '/apps/os/npu-queue.c')
        self.direct = self.address('direct', '/apps/os/npu-queue.c')
        self.rt, self.session, self.task = self.BASE + 0x10000, self.BASE + 0x80000, self.BASE + 0xa0000
        self.core, self.identity, self.svc_count = 1, self.task, 0
        self.context, self.reason, self.esr, self.elr, self.spsr = 0, None, 0, self.END, 0xa0000000
        self.daif, self.tpidr, self.sgi, self.calls, self.writes, self.returns = 0, 1, [], [], [], []
        self.stopping,self.cache_ops = False,[]
        self.app_sp = None
        if hasattr(self.image,'native_system_instructions'):
            self.instructions=self.image.native_system_instructions
        else:
            self.instructions = {}
            disassembly = subprocess.check_output(['/usr/bin/llvm-objdump-21', '-d', '--no-show-raw-insn', str(path)], text=True)
            for line in disassembly.splitlines():
                match = re.match(r'\s*([0-9a-f]+):\s+(mrs|msr|svc|eret|dc)\s*(.*)', line)
                if match:
                    self.instructions[int(match[1], 16)] = match[2], match[3].strip().lower()
            self.image.native_system_instructions=self.instructions
        self.hooks = {self.address(name): name for name in ('xTaskGetCurrentTaskHandle', 'os_session',
            'os_npu_stopping', 'runtime_current_reent', 'a733_smp_exit_privileged',
            'xTaskGenericNotify', 'vTaskGenericNotifyGiveFromISR', 'npu_service_send',
            'ss_npu_queue_svc', 'ss_npu_queue_service_progress')}
        self.crumbs, self.failures = [], []
        skip_crumbs(self.u, self.image, self.crumbs, self.failures)
        self.u.hook_add(UC_HOOK_CODE, self.code)
        self.u.hook_add(UC_HOOK_MEM_WRITE, self.mmio, begin=self.MMIO, end=self.MMIO + 4095)
        assert self.call('os_npu_queue_sdk_bind') == 1
        self.put(self.address('opened', '/apps/os/npu-queue.c'), 1, 1)
        self.set(self.ring, 'ss_npu_queue', 'state', self.states['READY'])
        for name, value, width in [('producer', self.task, 8), ('depth', 4, 4), ('handles', 1, 4), ('job_limit', 100, 8)]:
            self.set(self.ring, 'ss_npu_queue', name, value, width)
        group=self.field(self.ring,'ss_npu_queue','group')
        self.set(group,'ss_npu_queue_group','members',1)
        self.set(self.session, 'os_session', 'npu', 0)  # Remaining session fields are zero.
        self.set(self.field(self.session, 'os_session', 'npu'), 'os_npu_session', 'jobs', 100, 8)
        self.set(self.direct, 'npu_runtime_queue', 'runtime', self.rt, 8)
        self.set(self.rt, 'npu_runtime', 'startup', self.BASE + 0xb0000, 8)
        self.set(self.BASE + 0xb0000,'a733_npu_startup','window_open',1)
        self.set(self.field(self.rt, 'npu_runtime', 'native'), 'a733_npu_native', 'registers', self.MMIO, 8)
        self.set(self.direct, 'npu_runtime_queue', 'frequency', 24000000, 8)
        self.queue = self.field(self.direct, 'npu_runtime_queue', 'queue')
        self.set(self.queue, 'npu_queue', 'initialized', 1)
        self.set(self.queue, 'npu_queue', 'context', self.direct, 8)
        ops = self.field(self.queue, 'npu_queue', 'ops')
        for name in ('start', 'complete'):
            self.set(ops, 'npu_queue_ops', name, self.address(name, '/drivers/npu/npu_runtime_queue.c'), 8)
        self.set(ops, 'npu_queue_ops', 'notify_from_isr', self.address('notify', '/drivers/npu/npu_runtime_queue.c'), 8)
        command = self.address('commands', '/apps/os/npu-queue.c')
        self.set(command, 'npu_queue_command', 'owner', self.queue, 8)
        self.set(self.field(command, 'npu_queue_command', 'hardware'), 'a733_npu_command', 'device', 0x10000000)
        self.set(self.direct, 'npu_runtime_queue', 'claimed', 1)
        service = self.field(self.rt, 'npu_runtime', 'service')
        self.set(service, 'npu_service', 'direct', self.field(self.direct, 'npu_runtime_queue', 'direct'), 8)
        self.calls.clear()

    def address(self, name, unit=None):
        if unit and not any(key[1]==name and key[0].endswith(unit) for key in self.image.variables) and name in self.objects:
            assert len(self.objects[name])==1,(name,self.objects[name])
            return next(iter(self.objects[name]))
        return self.image.address(name, unit=unit)

    def field(self, base, kind, name):
        return base + self.image.layouts[kind][name]

    def put(self, address, value, size=4):
        self.u.mem_write(address, (value & ((1 << (8 * size)) - 1)).to_bytes(size, 'little'))

    def get(self, address, size=4):
        return int.from_bytes(self.u.mem_read(address, size), 'little')

    def set(self, base, kind, name, value, size=4):
        self.put(self.field(base, kind, name), value, size)

    def read(self, base, kind, name, size=4):
        return self.get(self.field(base, kind, name), size)

    def ret(self, value):
        self.u.reg_write(X[0], value & MASK)
        self.u.reg_write(reg.UC_ARM64_REG_PC, self.u.reg_read(X[30]))

    def mmio(self, cpu, access, address, size, value, unused):
        self.writes.append([address - self.MMIO, size, value])

    def code(self, cpu, address, size, unused):
        if address == self.END:
            self.reason = 'returned';cpu.emu_stop();return
        name = self.hooks.get(address)
        if name:
            self.calls.append(name)
            if name == 'xTaskGetCurrentTaskHandle':self.ret(self.identity);return
            if name == 'os_session':
                for n in range(1,19):cpu.reg_write(X[n],0xcafe0000+n)
                self.ret(self.session);return
            if name == 'os_npu_stopping':self.ret(self.stopping);return
            if name == 'runtime_current_reent':
                for n in range(19):cpu.reg_write(X[n], 0xdead0000 + n)
                self.ret(0x90800100);return
            if name == 'a733_smp_exit_privileged':self.reason = 'os-exit';cpu.emu_stop();return
            if name in ('xTaskGenericNotify', 'vTaskGenericNotifyGiveFromISR', 'npu_service_send'):
                raise AssertionError('unexpected service hop or notification: ' + name)
        instruction = self.instructions.get(address)
        if not instruction:
            return
        op, operands = instruction
        if op == 'mrs':
            target, system = operands.split(', ')
            values = {'esr_el1': self.esr, 'elr_el1': self.elr, 'spsr_el1': self.spsr,
                      'tpidr_el1': self.tpidr, 'mpidr_el1': self.core << 8,
                      'tpidrro_el0': self.task | self.core << 48,
                      'currentel': self.context << 2, 'cntpct_el0': 1000000,
                      'daif': self.daif}
            assert system in values, ('unexpected system read', system)
            cpu.reg_write(X[int(target[1:])], values[system])
        elif op == 'msr':
            system, value = operands.split(', ')
            number = int(value[1:], 0) if value.startswith('#') else cpu.reg_read(X[int(value[1:])])
            if system == 'elr_el1':self.elr = number
            elif system == 'spsr_el1':self.spsr = number
            elif system == 'tpidr_el1':self.tpidr = number
            elif system == 'icc_sgi1r_el1':self.sgi.append(number)
            elif system == 'daifset':self.daif |= number << 6
            else:raise AssertionError(('unexpected system write', system))
        elif op == 'svc':
            assert int(operands[1:], 0) == 120 and self.context == 0
            self.svc_count += 1;self.esr = (0x15 << 26) | 120
            self.elr = address + 4;self.context = 1;self.daif = 0x80
            self.app_sp = cpu.reg_read(reg.UC_ARM64_REG_SP)
            cpu.reg_write(reg.UC_ARM64_REG_SP, self.BASE + 0x3d0000)
            cpu.reg_write(reg.UC_ARM64_REG_PC, self.address('a733_cached_el1_vectors') + 0x400)
            return
        elif op == 'dc':
            assert operands.startswith('civac, '),'EL0 cache path used privileged IVAC'
            self.cache_ops.append(operands.split(', ')[0])
        elif op == 'eret':
            self.returns.append({'elr': self.elr, 'spsr': self.spsr, 'sp': cpu.reg_read(reg.UC_ARM64_REG_SP)})
            if self.app_sp is not None:
                assert cpu.reg_read(reg.UC_ARM64_REG_SP) == self.BASE + 0x3d0000
                cpu.reg_write(reg.UC_ARM64_REG_SP, self.app_sp);self.app_sp = None
            self.context = 0;self.daif = 0
            cpu.reg_write(reg.UC_ARM64_REG_PC, self.elr);return
        cpu.reg_write(reg.UC_ARM64_REG_PC, address + 4)

    def call(self, name, *args):
        self.reason = None
        self.u.reg_write(reg.UC_ARM64_REG_SP, self.BASE + 0x380000)
        self.u.reg_write(X[30], self.END)
        for n, value in enumerate(args):self.u.reg_write(X[n], value)
        self.u.emu_start(name if isinstance(name, int) else self.address(name), 0, count=100000)
        assert self.reason is not None, ('instruction budget', hex(self.u.reg_read(reg.UC_ARM64_REG_PC)))
        value = self.u.reg_read(X[0]) & 0xffffffff
        return value - (1 << 32) if value >> 31 else value

    def exception(self, number):
        original = [0x1122334400000000 + i for i in range(31)]
        for register, value in zip(X, original):self.u.reg_write(register, value)
        self.context = 1;self.daif = 0x80;self.esr = (0x15 << 26) | number
        self.elr = self.END;self.spsr = 0xa0000000
        self.reason = None
        stack = self.BASE + 0x3d0000
        self.u.reg_write(reg.UC_ARM64_REG_SP, stack)
        self.u.emu_start(self.address('a733_cached_el1_vectors') + 0x400, 0, count=100000)
        assert self.reason in ('returned', 'os-exit'), self.reason
        restored = [self.u.reg_read(register) for register in X]
        assert restored[1:] == original[1:], ('register clobber', number, [(i, a, b) for i, (a, b) in enumerate(zip(original, restored)) if a != b])
        assert self.u.reg_read(reg.UC_ARM64_REG_SP) == stack
        assert self.elr == self.END and self.spsr == 0xa0000000
        return restored[0], original[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve();out.mkdir(parents=True, exist_ok=False);(out / 'scratch').mkdir()
    elf, inputs = build_image(args.build_dir.resolve(), out)
    rows = []
    for number in (117, 118, 119):
        m = Native(elf);value, original = m.exception(number)
        if number == 117:assert m.sgi == [original] and value == original
        if number == 118:assert value == 0x90800100 and m.calls == ['runtime_current_reent']
        if number == 119:assert value == original and m.reason == 'os-exit'
        assert 'ss_npu_queue_svc' not in m.calls
        rows.append({'case': f'adjacent-svc-{number}-preserves-context', 'passed': True})
        print('PASS',rows[-1]['case'],flush=True)
    for failure in ('none', 'unopened', 'closed', 'wrong-core', 'wrong-producer'):
        m = Native(elf)
        if failure == 'unopened':m.put(m.address('opened', '/apps/os/npu-queue.c'), 0, 1)
        if failure == 'closed':m.put(m.address('closed', '/apps/os/npu-queue.c'), 1, 1)
        if failure == 'wrong-core':m.core = 2
        if failure == 'wrong-producer':m.identity += 8
        value, unused = m.exception(120)
        assert value & 0xffffffff == (1 if failure == 'none' else 0xffffffff), (failure, value)
        assert m.calls.count('ss_npu_queue_svc') == 1
        assert m.calls.count('ss_npu_queue_service_progress') == (failure == 'none')
        assert not m.writes
        rows.append({'case': 'svc120-' + failure + '-preserves-context', 'passed': True})
        print('PASS',rows[-1]['case'],flush=True)
    for failure in ('stop', 'lost-window'):
        m = Native(elf)
        m.stopping = failure == 'stop'
        if failure == 'lost-window':m.set(m.BASE + 0xb0000,'a733_npu_startup','window_open',0)
        m.set(m.ring,'ss_npu_queue','published',1,8)
        m.set(m.ring,'ss_npu_queue','active',1)
        entry=m.field(m.ring,'ss_npu_queue','entry')
        m.set(entry,'ss_npu_queue_entry','handle',1)
        m.set(entry,'ss_npu_queue_entry','timeout',1000000,8)
        value,unused=m.exception(120)
        assert value&0xffffffff==0xffffffff and not m.writes
        assert m.read(m.ring,'ss_npu_queue','state')==m.states['FAILED']
        assert m.read(m.queue,'npu_queue','failed')==int(failure=='lost-window')
        assert m.calls.count('ss_npu_queue_svc')==1
        rows.append({'case':'svc120-'+failure+'-retains-without-task-notification','passed':True})
        print('PASS',rows[-1]['case'],flush=True)
    m=Native(elf)
    m.put(m.address('cache_stride','/apps/os/npu-queue.c'),64)
    m.call(m.address('cache','/apps/os/npu-queue.c'),0,m.BASE+0x1000,128)
    assert m.cache_ops==['civac','civac']
    rows.append({'case':'el0-cache-uses-civac-for-each-line','passed':True})
    m = Native(elf)
    fence = m.BASE + 0x1000
    assert m.call('ss_npu_queue_submit', m.ring, 1, 0, 0, 1000000, fence) == 1
    assert m.svc_count == 1 and m.calls.count('ss_npu_queue_svc') == 1
    assert [w[0] for w in m.writes] == [0x654, 0x3a4]
    assert m.call('ss_npu_queue_submit', m.ring, 1, m.ring, 1, 1000000, fence) == 1
    assert m.svc_count == 1 and len(m.writes) == 2
    m.put(m.MMIO + 0x10, 1);m.put(m.MMIO + 4, 0x7fffffff)
    m.context = 1;m.daif = 0x80
    m.call(m.address('irq', '/drivers/npu/npu_runtime_queue.c'), m.direct, m.BASE + 0x2000)
    assert m.svc_count == 1 and [w[0] for w in m.writes] == [0x654, 0x3a4, 0x654, 0x3a4]
    assert m.read(m.ring, 'ss_npu_queue', 'completed', 8) == 1
    assert m.read(m.ring, 'ss_npu_queue', 'consuming', 8) == 2
    m.call(m.address('irq', '/drivers/npu/npu_runtime_queue.c'), m.direct, m.BASE + 0x2000)
    assert m.read(m.ring,'ss_npu_queue','completed',8)==2
    assert m.read(m.ring,'ss_npu_queue','consuming',8)==0
    assert m.read(m.ring,'ss_npu_queue','active')==0
    m.context=0;m.daif=0
    assert m.call('ss_npu_queue_submit',m.ring,1,m.ring,2,1000000,fence)==1
    assert m.svc_count==2 and len(m.writes)==6
    rows.append({'case': 'one-el0-doorbell-and-native-irq-refill-without-service-hop', 'passed': True,
                 'svc_entries': m.svc_count, 'native_mmio_writes': m.writes, 'calls': m.calls})
    (out / 'result.json').write_text(json.dumps({'passed': True, 'physical_access': False,
        'cases': rows, 'inputs': inputs, 'image_sha256': sha(elf), 'scope': __doc__}, indent=2) + '\n')
    print('PASS', len(rows), 'native EL0 SVC and IRQ integration cases')


if __name__ == '__main__':
    main()
