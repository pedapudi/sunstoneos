#!/usr/bin/env python3
"""Boot the SunstoneOS image on a virtual eight-core board in QEMU, through startup to the first application task.

The image is the OS as it is built for the A733 (apps/os, apps/board, the
thermal, power-domain, clock-lease and rail-lease drivers, the precise-wait
runtime and a real application) on the FreeRTOS SMP port, with a virtual board
under platform/qemu-os in place of the A733 hardware. The board description
supplies the sealed session and launch contract that the stage-2 loader seals,
a model of the thermal sensors with scripted temperatures and timing, the PMIC
and power-domain providers, the clock and CPU-operating-point providers, a
fixed-state USB device transport and absent NPU and GPU services. No OS
source is edited; the board's hooks sit in platform/qemu-os. No board is used.

Cases: normal start to the first application and its normal finish; the zero
sealed deadline that refused every application wait with failure 0x757
(negative control: the check must fail with the earlier apps/os/services.c);
sensors not yet read at application start; an OS service that fails to start.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

from harness import CACHE, CLANG, LLD, environment

ROOT = Path(__file__).resolve().parents[1]
QEMU = CACHE / 'host/usr/bin/qemu-system-aarch64'
PYTHON = sys.executable
OBJCOPY = '/usr/lib/llvm-21/bin/llvm-objcopy'


def freertos_dir():
    found = sorted((CACHE / 'sources').glob('FreeRTOS-Kernel-*'))
    assert len(found) == 1, 'the pinned FreeRTOS kernel source is missing from the source cache'
    return found[0]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(command, log=None, **kwargs):
    command = [str(c) for c in command]
    done = subprocess.run(command, capture_output=True, text=True, **kwargs)
    if log is not None:
        with open(log, 'a') as stream:
            stream.write(' '.join(command) + '\n' + done.stdout + done.stderr)
    return done


# ---------------------------------------------------------------------------
# Image construction

DEFINES = ['-DA733_SMP_CONTRACT_MAGIC=0x415337334f534331ULL', '-DA733_SMP_RESULT_MAGIC=0x415337334f535231ULL',
           '-DSUNSTONE_A733_OS=1', '-DSUNSTONE_A733_SMP_DIAGNOSTIC=1', '-DSUNSTONE_BOARD_APPLICATION=1',
           '-DSUNSTONE_BOARD_SUPERVISED=1', '-DSUNSTONE_BOARD_USB2_DUAL=1', '-DSUNSTONE_TASK_DEADLINES=1',
           '-DSUNSTONE_A733_IRQ_TABLE=1', '-DSUNSTONE_CRUMBS=1']
CFLAGS = ['--target=aarch64-none-elf', '-march=armv8-a', '-mstrict-align', '-mgeneral-regs-only', '-ffreestanding',
          '-fno-builtin', '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
          '-ffunction-sections', '-fdata-sections', '-O2', '-g', '-Wall', '-Wextra', '-include', 'stdint.h']

# OS and board sources that run unmodified.
OS_SOURCES = ['apps/os/application.c', 'apps/os/services.c', 'apps/os/session.c', 'apps/os/start.c',
              'apps/os/thermal.c', 'apps/os/admission.c', 'apps/board/main.c', 'apps/board/session.c',
              'runtime/deadline.c', 'runtime/deadline-a733.c', 'platform/a733-smp-cached/irq_table.c']
# Driver cores and their adapters, unmodified. The adapters reach registers
# through a733_mmio.h, which the virtual board replaces (platform/qemu-os).
DRIVER_SOURCES = ['drivers/thermal/a733_thermal.c', 'drivers/thermal/a733_thermal_native.c',
                  'drivers/clock/a733_clock_leases.c', 'drivers/power/a733_rail_leases.c',
                  'drivers/power/a733_pmic.c', 'drivers/power/a733_pmic_native.c',
                  'drivers/power/a733_pmic_consumer.c', 'drivers/power/a733_power_domains.c',
                  'drivers/power/a733_power_domains_native.c', 'drivers/pinctrl/a733_pins.c',
                  'drivers/pinctrl/a733_pin_tables.c', 'drivers/pinctrl/a733_pins_native.c',
                  'drivers/gpu/a733_gpu_resources.c', 'drivers/i2c/a733_i2c.c', 'drivers/usb/a733_usb_platform.c',
                  'drivers/storage/storage_digest.c', 'platform/a733/shared_locks.c', 'platform/a733/native_usb.c',
                  'platform/a733/counter_clock.c', 'arch/aarch64/device_memory.c', 'runtime/memory.c',
                  'runtime/memmove.c', 'runtime/gpu_call.S']
# Units that include the A733 platform's boot.h.
QUOTE_A733 = {'drivers/gpu/a733_gpu_resources_native.c', 'drivers/i2c/a733_i2c.c'}
# The virtual board.
BOARD_SOURCES = ['platform/qemu-os/start.S', 'platform/qemu-os/platform.c', 'platform/qemu-os/board.c',
                 'platform/qemu-os/mmio.c', 'platform/qemu-os/absent.c', 'platform/qemu-os/snprintf.c', 'drivers/gpu/a733_gpu_resources_native.c',
                 'apps/os/cpu-return-el0.S']


class Image:
    """Compile and link one image of the virtual board.

    Objects are shared between the images of one run when their source and
    flags are the same."""

    def __init__(self, work, services_source=None, extra_defines=(), extra_sources=(), latency=False, resident=False,
                 partitioned=False):
        self.work = Path(work)
        # False, True, or the directory holding a replacement kernel/scheduler/freertos.c.
        self.partitioned = partitioned
        self.extra_sources = list(extra_sources)
        self.latency = latency
        self.services_source = Path(services_source) if services_source else ROOT / 'apps/os/services.c'
        self.defines = DEFINES + list(extra_defines)
        if resident:
            # The resident lifetime: its own contract and result magic numbers, the supervisor
            # that renews WDT0 while every service reports (cmake/a733-os.cmake).
            self.defines = [d for d in self.defines if 'MAGIC' not in d] + [
                '-DA733_SMP_CONTRACT_MAGIC=0x415337334f524331ULL', '-DA733_SMP_RESULT_MAGIC=0x415337334f525231ULL',
                '-DSUNSTONE_BOARD_RESIDENT=1']
            self.extra_sources += ['apps/os/watchdog.c', 'platform/a733/maintenance_watchdog.c']
        self.kernel = freertos_dir()
        self.log = self.work / 'build.log'

    def patch(self, output, source, patch):
        done = run(['patch', '--fuzz=0', '--reject-file=-', '--output', output, source, ROOT / 'patches' / patch], self.log)
        assert done.returncode == 0, 'patch failed: ' + patch + '\n' + done.stdout + done.stderr

    def prepare_kernel(self):
        k, port = self.work / 'freertos', self.kernel / 'portable/GCC/ARM_CR82'
        k.mkdir(parents=True, exist_ok=True)
        self.patch(k / 'port-base.c', port / 'port.c', 'freertos-smp-c.patch')
        self.patch(k / 'port-priv.c', k / 'port-base.c', 'freertos-smp-privileged.patch')
        self.patch(k / 'portASM-base.S', port / 'portASM.S', 'freertos-smp-asm.patch')
        self.patch(k / 'portASM-gpu.S', k / 'portASM-base.S', 'freertos-gpu-call.patch')
        self.patch(k / 'portmacro-base.h', port / 'portmacro.h', 'freertos-smp-macro.patch')
        if self.latency:
            # The latency diagnostic's instrumented port (cmake/latency-instrumentation.cmake).
            self.patch(k / 'port.c', k / 'port-priv.c', 'freertos-latency-c.patch')
            self.patch(k / 'portASM.S', k / 'portASM-gpu.S', 'freertos-latency-asm.patch')
            self.patch(k / 'portmacro.h', k / 'portmacro-base.h', 'freertos-latency-macro.patch')
        else:
            for name, source in (('port.c', 'port-priv.c'), ('portASM.S', 'portASM-gpu.S'), ('portmacro.h', 'portmacro-base.h')):
                shutil.copy(k / source, k / name)
        if not self.partitioned:
            self.patch(k / 'tasks.c', self.kernel / 'tasks.c', 'freertos-smp-tasks.patch')
            return k
        # The private owner scheduler (cmake/scheduler-partitioned.cmake, sunstone_scheduler_partitioned_prepare).
        self.patch(k / 'tasks-smp.c', self.kernel / 'tasks.c', 'freertos-smp-tasks.patch')
        include = k / 'include'
        include.mkdir(exist_ok=True)
        for header in (self.kernel / 'include').glob('*.h'):
            shutil.copy(header, include / header.name)
        self.patch(include / 'FreeRTOS.h', self.kernel / 'include/FreeRTOS.h', 'freertos-partitioned-layout.patch')
        self.patch(k / 'tasks-layout.c', k / 'tasks-smp.c', 'freertos-partitioned-tasks.patch')
        self.patch(k / 'tasks.c', k / 'tasks-layout.c', 'freertos-partitioned-api.patch')
        self.patch(k / 'portASM-owner.S', k / 'portASM.S', 'freertos-partitioned-port-asm.patch')
        shutil.copy(k / 'portASM-owner.S', k / 'portASM.S')
        for name in ('queue', 'event_groups', 'stream_buffer', 'timers'):
            shutil.copy(self.kernel / (name + '.c'), k / (name + '.c'))
        done = run(['patch', '--fuzz=0', '--reject-file=-', '-d', k, '-p0', '-i',
                    ROOT / 'patches/freertos-partitioned-objects.patch'], self.log)
        assert done.returncode == 0, 'patch failed: freertos-partitioned-objects.patch\n' + done.stdout + done.stderr
        return k

    def includes(self, generated):
        sources = CACHE / 'sources/radio/stacks'
        tcp = [d for d in sorted(sources.glob('FreeRTOS-Plus-TCP-*')) if d.is_dir()][0]
        rsn = [d for d in sorted(sources.glob('wpa_supplicant-*')) if d.is_dir()][0]
        first = [*([Path(self.partitioned)] if self.partitioned is not True else []), ROOT / 'kernel/scheduler', self.work / 'freertos/include'] if self.partitioned else []
        return ['-I' + str(p) for p in (
            *first, generated, ROOT / 'platform/qemu-os', ROOT / 'apps/os', ROOT / 'apps/board',
            ROOT / 'platform/a733-smp-cached', ROOT / 'include', ROOT / 'runtime/include',
            self.kernel / 'include', self.work / 'freertos', ROOT / 'network/include',
            tcp / 'source/include', tcp / 'source/portable/Compiler/GCC', rsn / 'src', rsn / 'src/utils')] + [
            '-isystem', str(next((CACHE / 'host').glob('arm-gnu-toolchain-*/aarch64-none-elf/include')))]

    def compile_all(self, units, generated):
        self.work.mkdir(parents=True, exist_ok=True)
        flags = CFLAGS + self.defines + self.includes(generated)
        if self.partitioned:
            flags += ['-DconfigSS_PARTITIONED_SCHEDULER=1', '-march=armv8.1-a+lse']
        objects, jobs = [], []
        for source, extra in units:
            source = Path(source)
            unit_flags = flags if source.suffix != '.S' else [f for f in flags if f not in ('-mgeneral-regs-only',)]
            if source.suffix == '.S':
                i = unit_flags.index('-include')
                unit_flags = unit_flags[:i] + unit_flags[i + 2:]
            obj = self.work / 'obj' / (hashlib.sha1(str(source).encode()).hexdigest()[:8] + '-' + source.stem + '.o')
            obj.parent.mkdir(exist_ok=True)
            objects.append(obj)
            jobs.append(([CLANG, *unit_flags, *extra, '-c', source, '-o', obj], source))
        def one(job):
            command, source = job
            done = run(command, self.log)
            assert done.returncode == 0, 'compile failed: %s\n%s' % (source, done.stderr)
        with ThreadPoolExecutor(max_workers=min(12, os.cpu_count() or 4)) as pool:
            list(pool.map(one, jobs))
        return objects

    def object(self, source, extra=()):
        return (Path(source), tuple(extra))

    def application(self, name, sources, manifest):
        """The generated image binding of one application (tools/sunstone-manifest.py) and
        the wrapper that reports its first instruction."""
        cname = name.replace('-', '_')
        generated = self.work / 'app' / name
        generated.mkdir(parents=True, exist_ok=True)
        shutil.copy(manifest, generated / 'manifest-input.json')
        done = run([PYTHON, '-B', ROOT / 'tools/sunstone-manifest.py', 'emit', '--manifest',
                    generated / 'manifest-input.json', '--output', generated], self.log)
        assert done.returncode == 0, done.stderr
        (generated / 'wrap.c').write_text(
            '#include "sunstone_app.h"\n#include "qemu_os.h"\n'
            'extern void __real_%s_main(const struct ss_grant *);\n'
            'void __wrap_%s_main(const struct ss_grant *grant)\n'
            '{ qemu_os_app_entry("%s", grant); __real_%s_main(grant); }\n' % (cname, cname, name, cname))
        units = [(generated / 'sunstone-tables.c', ()), (generated / ('%s-manifest.c' % cname), ()),
                 (generated / 'wrap.c', ())] + [(ROOT / s, ()) for s in sources]
        return generated, units, '--wrap=%s_main' % cname

    def build(self, application, services_source=None, define=(), cache=None):
        """Link the image; returns the ELF path."""
        generated, app_units, wrap = application
        k = self.prepare_kernel()
        units = [(ROOT / s, ('-iquote', str(ROOT / 'platform/a733')) if s in QUOTE_A733 else ())
                 for s in OS_SOURCES + DRIVER_SOURCES + BOARD_SOURCES + self.extra_sources]
        units += app_units
        units += [(k / 'tasks.c', ()), (k / 'port.c', ('-Dnpu_service_task_privileged=a733_storage_native_task_privileged',)),
                  (k / 'portASM.S', ()), (k / 'queue.c' if self.partitioned else self.kernel / 'queue.c', ()),
                  (self.kernel / 'list.c', ())]
        if self.partitioned:
            units += [(k / name, ()) for name in ('event_groups.c', 'stream_buffer.c', 'timers.c')]
            units += [(ROOT / 'kernel/scheduler' / name, ()) for name in
                      ('owner.c', 'notification.c', 'deadline.c', 'freertos_registry.c', 'freertos_control.c')]
        units = [(self.services_source if u[0] == ROOT / 'apps/os/services.c' else u[0], u[1]) for u in units]
        objects = self.compile_all(units, generated)
        elf = self.work / 'os.elf'
        done = run([LLD, '--error-limit=0', '-nostdlib', '-static', '--gc-sections', wrap,
                    *(['--wrap=vApplicationIRQHandler'] if self.latency else []),
                    '-T', ROOT / 'platform/qemu-os/link.ld', '-Map', self.work / 'os.map',
                    '-o', elf, *objects], self.log)
        assert done.returncode == 0, 'link failed:\n' + done.stderr
        return elf


# ---------------------------------------------------------------------------
# What the stage-2 loader seals: the session, the launch contract and the scenario

host = load('os_host_records', ROOT / 'apps/os/host.py')
SESSION_ADDRESS, CONTRACT_ADDRESS, SCENARIO_ADDRESS = 0x41f00000, 0x41e01000, 0x41e00000
BOARD_SESSION_MAGIC, OS_SERVICE_MAGIC = 0x4153373342535332, 0x415337334f534131
PL_PULL, PL_DRIVE = 1, 2  # platform/qemu-os/qemu_os.h
INFINITY = 2 ** 64 - 1
# apps/os/os.h struct os_session and include/a733_board_application.h struct a733_board_session.
BOARD = struct.Struct('<6Q32s32s4Q4IQQQ4Q32s32s32sQ32s2Q32s2I2Q2I2Q2I4Q3I3I5I3I4II6B12B6B4x')
assert BOARD.size == 592


def elf_bytes(elf, symbol, size):
    """The initialized bytes of one object in a linked ELF."""
    info = host.ELFFile(open(elf, 'rb'))
    found = [s for s in info.get_section_by_name('.symtab').iter_symbols() if s.name == symbol]
    assert len(found) == 1, symbol
    section = info.get_section(found[0]['st_shndx'])
    offset = found[0]['st_value'] - section['sh_addr']
    return section.data()[offset:offset + size]


def board_prefix(identity, epoch=0x1101, generation=0x2202, lease=0x3303):
    """A supervised board session for workload 2: no USB host, no radio.

    The values are the loader's claims; the OS validates their form
    (apps/board/admission.c) and the board's registers are seeded to agree
    with the ones that name inherited state (the PMIC bus pins)."""
    digest = lambda n: hashlib.sha256(n.encode()).digest()
    values = [BOARD_SESSION_MAGIC, 2, 592, epoch, generation, lease, identity, digest('qualification'),
              0, 0, 0, 0, 0, 0, 0, 0,              # reload and recovery times, RTC fields
              0, 0, 0,                             # deadline_us, session_start_ticks, reload_after_ticks
              1, 2, 3, 4, digest('wiring'), digest('memory'), digest('qualification'),
              0, bytes(32), 0, 0, bytes(32),       # SID authority and evidence, radio profile
              2, 0,                                # workload, USB topology
              0, 0, 0, 0, 0, 0, 0, 0,              # latches
              0x70c1c, 0x75b0, 0, 0,               # PMIC and USB0 owners, no Type-C or USB2 owner
              0, 0x80000000, 0x80000000, 24000000, 24000000, 24000000, 3300000, 3300000, 1800000, 1200000, 0,
              1, 0, 0,                             # USB0 cold reference, USB2 revision, PCK client
              0, 0, 0, 0, 0]
    pins = [2, PL_PULL, PL_DRIVE] * 2
    raw = BOARD.pack(*values, *pins, *([0] * 12), *([0] * 6))
    assert struct.unpack_from('<I', raw, 400)[0] == 2
    return raw


def thermal_service(total):
    """The thermal owner's record (struct os_session after the board prefix)."""
    h = lambda n: hashlib.sha256(n.encode()).digest()
    record = struct.pack('<5Q', OS_SERVICE_MAGIC, 1, total, 0x7e40a1, 0x7e40a2)
    record += h('thermal-initial') + h('thermal-electrical') + h('thermal-clocks') + h('thermal-supervision')
    record += struct.pack('<2I4B', 0, 24000000, 0, 31, 31, 0)
    for hot, hysteresis, critical in ((1500, 1600, 1400),) * 5:
        record += struct.pack('<3H', hot, hysteresis, critical)
    record += bytes(2) + struct.pack('<I', 0)
    assert len(record) == host.SERVICE_BYTES[host.THERMAL], len(record)
    return record


def session_for(elf, input_bytes):
    """The complete sealed session of the image's application and its launch contract."""
    info = host.image_manifest(elf)
    identity = elf_bytes(elf, 'a733_smp_build_id', 32)
    services = {host.THERMAL: thermal_service(host.PREFIX_BYTES + host.SERVICE_BYTES[host.THERMAL])}
    raw = host.session_bytes(board_prefix(identity), services, info['manifest'], info['sha256'], 0,
                             host.BIND_SID, [input_bytes] if input_bytes is not None else [])
    epoch, generation, lease = struct.unpack_from('<3Q', raw, 24)
    # The supervised launch contract (apps/board/prepare.py request_bytes): the loader
    # stamps the times and the magic when it launches.
    h = hashlib.sha256(raw).digest()
    contract = struct.pack('<6Q', 0, 2, 224, epoch, generation, generation ^ (2 ** 64 - 1)) + identity
    contract += struct.pack('<14Q', 0, 0, 0, 0, lease, 0, 0, 0, 0, 0, 0, 0, SESSION_ADDRESS, len(raw)) + h
    return raw, contract + bytes(4096 - len(contract)), info


SCENARIO = struct.Struct('<3Q2I5Q5i5iQQQQQ4Q')


def scenario(fault=0, first_ms=(2, 2, 2, 2, 2), temperature_c=(52, 60, 48, 55, 50), slope_mc=(0,) * 5,
             period_us=25000, usb_ready_ms=30, linger_us=0, limit_ms=8000, trace=0, hold_ms=0):
    """The board's behavior in one run; see platform/qemu-os/qemu_os.h."""
    first = [INFINITY if t is None else t * 1000 for t in first_ms]
    raw = SCENARIO.pack(0x514f535343454e31, 1, SCENARIO.size, fault, period_us, *first,
                        *[int(t * 1000) for t in temperature_c], *slope_mc,
                        usb_ready_ms * 1000, linger_us, limit_ms * 1000, trace, hold_ms * 1000, *([0] * 4))
    return raw


# ---------------------------------------------------------------------------
# Running the image

def adler32(data):
    a, b = 1, 0
    for byte in data:
        a = (a + byte) % 65521
        b = (b + a) % 65521
    return b << 16 | a


def boot(elf, raw_session, contract, scenario_bytes, inputs, log, timeout=60):
    """One QEMU run. Returns the console text, the exit status and the wall seconds."""
    work = Path(elf).parent
    files = {'session.bin': raw_session, 'contract.bin': contract, 'scenario.bin': scenario_bytes}
    loaders = []
    for (name, data), address in zip(files.items(), (SESSION_ADDRESS, CONTRACT_ADDRESS, SCENARIO_ADDRESS)):
        path = work / (Path(log).stem + '-' + name)
        path.write_bytes(data)
        loaders += ['-device', 'loader,file=%s,addr=%#x,force-raw=on' % (path, address)]
    for address, data in inputs:
        path = work / (Path(log).stem + '-input-%#x.bin' % address)
        path.write_bytes(data)
        loaders += ['-device', 'loader,file=%s,addr=%#x,force-raw=on' % (path, address)]
    # Guest time counts instructions, not the wall time of a loaded host: a service window or a
    # sensor deadline of the board then cannot expire because a vCPU thread was descheduled.
    command = [QEMU, '-machine', 'virt,gic-version=3,secure=off,virtualization=off',
               '-accel', 'tcg,thread=single', '-icount', 'shift=0,sleep=off',
               '-cpu', 'cortex-a76', '-smp', '8', '-m', '512M', '-nographic', '-monitor', 'none',
               '-semihosting-config', 'enable=on,target=native', '-kernel', elf, *loaders]
    start = time.monotonic()
    try:
        done = subprocess.run([str(c) for c in command], capture_output=True, text=True, timeout=timeout)
        output, status = done.stdout + done.stderr, done.returncode
    except subprocess.TimeoutExpired as error:
        output = ((error.stdout or b'') + (error.stderr or b'')).decode(errors='replace')
        status = 124
    seconds = time.monotonic() - start
    Path(log).write_text(output)
    return output, status, seconds


def fields(line):
    return dict(re.findall(r'(\w+)=(\S+)', line))


def lines(output, tag):
    return [fields(l) | {'line': l} for l in output.splitlines() if l.startswith(tag)]


# ---------------------------------------------------------------------------
# Cases

FAILURES = ('PANIC', 'EXCEPTION', 'TIMEOUT', 'FAILURE-RECORD', 'STOP', 'SP-EL0-VIOLATION')


class Case:
    """The result of one boot: what the console said and what the cases may ask of it."""

    def __init__(self, name, output, status, seconds, log):
        self.name, self.output, self.status, self.seconds, self.log = name, output, status, seconds, log
        self.failure = (lines(output, 'FAILURE-RECORD') or [None])[0]
        self.main = (lines(output, 'APP-MAIN') or [None])[0]
        self.finished = (lines(output, 'APP-FINISHED') or [None])[0]
        self.cleared = (lines(output, 'THROTTLE-CLEARED') or [None])[0]

    def require(self, condition, message):
        assert condition, '%s: %s\n--- console (%s) ---\n%s' % (self.name, message, self.log, self.output[-3000:])

    def clean(self):
        """The run ended without a failure record, panic, exception, stop or timeout."""
        bad = [l for l in self.output.splitlines() if l.startswith(FAILURES)]
        self.require(not bad and self.status == 0, 'unexpected end: status %d, %s' % (self.status, bad[:3]))

    def failed_with(self, group, detail=None):
        self.require(self.failure is not None and self.status == 3, 'no failure record (status %d)' % self.status)
        self.require(self.failure['group'] == group, 'failure names %s, expected %s' % (self.failure['group'], group))
        if detail is not None:
            self.require(int(self.failure['detail'], 16) == detail, 'failure detail %s, expected %#x' % (self.failure['detail'], detail))


def check_mmio_header():
    """platform/qemu-os/a733_mmio.h differs from include/a733_mmio.h only in its eight plain accessors."""
    real = (ROOT / 'include/a733_mmio.h').read_text()
    for width, ctype in ((8, 'uint8_t'), (16, 'uint16_t'), (32, 'uint32_t'), (64, 'uint64_t')):
        real = real.replace('{ return *(volatile const %s *)address; }' % ctype,
                            '{ return (%s)qemu_os_mmio_read(address, %d); }' % (ctype, width // 8))
        real = real.replace('{ *(volatile %s *)address = value; }' % ctype,
                            '{ qemu_os_mmio_write(address, %d, value); }' % (width // 8))
    marker = 'static inline uint8_t a733_mmio_read8('
    shadow = (ROOT / 'platform/qemu-os/a733_mmio.h').read_text()
    assert real[real.index(marker):].strip() == shadow[shadow.index(marker):].strip(), \
        'platform/qemu-os/a733_mmio.h no longer matches include/a733_mmio.h'


def reverted_services(work):
    """apps/os/services.c as it was before the application's waits were bounded by the
    absolute runtime deadline: the wait certificate compares with the sealed session's
    deadline field, which is zero (include/a733_board_application.h, supervised session)."""
    source = (ROOT / 'apps/os/services.c').read_text()
    fixed = 'deadline>board_runtime_deadline_us()||'
    assert source.count(fixed) == 1, 'apps/os/services.c no longer holds the runtime deadline comparison'
    path = Path(work) / 'services-reverted.c'
    path.write_text(source.replace(fixed, 'deadline>session->board.deadline_us||'))
    return path


def unguarded_prestart_scheduler(work):
    """The directory of a kernel/scheduler/freertos.c from before the owner scheduler refused to
    yield or wait for an owner while the scheduler is not running (round 10 of the series).
    The board's first task changes its core affinity before the start; the old file then marks
    its own core for rescheduling and yields before any task stack is installed in SP_EL0."""
    source = (ROOT / 'kernel/scheduler/freertos.c').read_text()
    guard = '    if (!xSchedulerRunning) return false;\n'
    assert source.count(guard) == 1, 'ssKernelCanPreempt no longer refuses to yield before the start'
    source = source.replace(guard, '')
    begin = source.index('    if (!xSchedulerRunning) {\n        /* Boot has no running owner')
    end = source.index('        return changed;\n    }\n', begin) + len('        return changed;\n    }\n')
    source = source[:begin] + source[end:]
    directory = Path(work)
    directory.mkdir(parents=True, exist_ok=True)
    (directory / 'freertos.c').write_text(source)
    return directory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--services-source', type=Path,
                        help='build the images from this apps/os/services.c; the earlier file must fail the normal start')
    args = parser.parse_args()
    out = args.output
    out.mkdir(parents=True, exist_ok=False)
    assert QEMU.exists(), 'the cached QEMU is missing: ' + str(QEMU)
    started = time.monotonic()
    check_mmio_header()
    data = bytes((i * 7 + 3) & 255 for i in range(65536))
    expected = struct.pack('<IIQ', adler32(data), 0, len(data))
    results = []

    def build_hello(name, services=None):
        image = Image(out / name, services_source=services)
        application = image.application('hello-compute', ['apps/hello-compute/main.c'],
                                         ROOT / 'apps/hello-compute/manifest.json')
        return image.build(application)

    def build_latency():
        image = Image(out / 'build-latency', extra_defines=['-DA733_A76_ADMITTED=1', '-DSUNSTONE_LATENCY=1'],
                      extra_sources=['apps/os/cpu.c', 'drivers/clock/a733_cpu_clock.c', 'runtime/latency.c',
                                     'runtime/latency_irq.c'], latency=True)
        application = image.application('latency', ['apps/latency/native.c', 'apps/latency/workload.c'],
                                         ROOT / 'apps/latency/manifest.json')
        return image.build(application)

    def build_partitioned(name, latency=False, scheduler=True):
        """The same board composition on the private owner scheduler (configSS_PARTITIONED_SCHEDULER)."""
        if latency:
            image = Image(out / name, extra_defines=['-DA733_A76_ADMITTED=1', '-DSUNSTONE_LATENCY=1'],
                          extra_sources=['apps/os/cpu.c', 'drivers/clock/a733_cpu_clock.c', 'runtime/latency.c',
                                         'runtime/latency_irq.c'], latency=True, partitioned=scheduler)
            application = image.application('latency', ['apps/latency/native.c', 'apps/latency/workload.c'],
                                             ROOT / 'apps/latency/manifest.json')
        else:
            image = Image(out / name, partitioned=scheduler)
            application = image.application('hello-compute', ['apps/hello-compute/main.c'],
                                             ROOT / 'apps/hello-compute/manifest.json')
        return image.build(application)

    def build_el0_abort():
        image = Image(out / 'build-el0-abort')
        application = image.application('el0-abort', ['checks/qemu-os/el0-abort-main.c'],
                                         ROOT / 'checks/qemu-os/el0-abort-manifest.json')
        return image.build(application)

    def build_resident():
        image = Image(out / 'build-resident', services_source=args.services_source, resident=True)
        application = image.application('hello-compute', ['apps/hello-compute/main.c'],
                                         ROOT / 'apps/hello-compute/manifest.json')
        return image.build(application)

    old_scheduler = unguarded_prestart_scheduler(out / 'prestart-yield-scheduler')
    with ThreadPoolExecutor(max_workers=8) as pool:
        partitioned_elf = pool.submit(build_partitioned, 'build-partitioned')
        partitioned_latency_elf = pool.submit(build_partitioned, 'build-partitioned-latency', True)
        prestart_elf = pool.submit(build_partitioned, 'build-prestart-yield', False, old_scheduler)
        os_elf = pool.submit(build_hello, 'build-os', args.services_source)
        (out / 'build-reverted').mkdir()
        control_elf = pool.submit(build_hello, 'build-reverted-services', reverted_services(out / 'build-reverted'))
        latency_elf = pool.submit(build_latency)
        resident_elf = pool.submit(build_resident)
        el0_elf = pool.submit(build_el0_abort)
        os_elf, control_elf, latency_elf, resident_elf, el0_elf, partitioned_elf, partitioned_latency_elf, prestart_elf = (
            os_elf.result(), control_elf.result(), latency_elf.result(), resident_elf.result(), el0_elf.result(),
            partitioned_elf.result(), partitioned_latency_elf.result(), prestart_elf.result())
    built = time.monotonic() - started

    def run_case(name, elf, scenario_bytes, input_bytes=data):
        raw, contract, info = session_for(elf, input_bytes)
        inputs = [(info['manifest']['regions'][0]['address'], input_bytes)] if input_bytes is not None else []
        output, status, seconds = boot(elf, raw, contract, scenario_bytes, inputs, out / (name + '.log'))
        case = Case(name, output, status, seconds, out / (name + '.log'))
        results.append(case)
        return case

    usb_delay_ms = 30
    # 1. Normal start: the OS reports live, the application's main entry runs, and it finishes.
    c = run_case('normal-start', os_elf, scenario(usb_ready_ms=usb_delay_ms))
    c.clean()
    c.require(c.main is not None and c.main['name'] == 'hello-compute' and c.main['live'] == '1',
              'the application did not reach its main entry with the OS live')
    c.require(int(c.main['t_us']) >= usb_delay_ms * 1000,
              'the application started before the USB transport was ready, so its wait path never ran')
    c.require(c.main['core'] == '2' and c.main['regions'] == '1', 'application placement or grant differs')
    c.require(c.finished is not None and c.finished['status'] == '1', 'the application did not finish')
    c.require(bytes.fromhex(c.finished['result']) == expected, 'the result differs from the Adler-32 of the input')
    c.require(int(c.main['t_us']) <= int(c.finished['t_us']), 'the application finished before it started')
    c.require(c.cleared is not None, 'the OS never cleared the thermal throttle')
    boot_line = lines(c.output, 'BOOT')[0]
    c.require(boot_line['cores'] == '8' and boot_line['workload'] == '2', 'board description differs')

    # 2. The zero sealed deadline. The earlier services.c refuses the application's first wait.
    c = run_case('zero-sealed-deadline-reverted', control_elf, scenario(usb_ready_ms=usb_delay_ms))
    c.failed_with('APPLICATION', 0x757)

    # 3. Sensors not yet read when the application starts: the OS holds the throttle, the
    # application starts and finishes, and the throttle clears when the last sensor answers
    # (after the one-second freshness window, so the unread sensors also read as stale).
    c = run_case('sensors-not-yet-read', os_elf, scenario(first_ms=(2, 2, 2, 1500, 1500), usb_ready_ms=usb_delay_ms))
    c.clean()
    c.require(c.main is not None and c.main['throttled'] == '1' and int(c.main['t_us']) < 1_500_000,
              'the application did not start while sensors were unread')
    c.require(c.cleared is not None and int(c.cleared['t_us']) >= 1_500_000,
              'the throttle cleared before the last sensor was read')
    c.require(c.finished is not None and c.finished['status'] == '1', 'the application did not finish')

    # 4. A sensor that never answers: after five seconds without a reading the thermal owner
    # ends the session and names itself.
    c = run_case('sensor-lost', os_elf, scenario(first_ms=(2, 2, None, 2, 2), usb_ready_ms=usb_delay_ms, limit_ms=9000))
    c.failed_with('THERMAL')
    c.require(5_000_000 <= int(c.failure['t_us']) < 6_500_000, 'the lost sensor was reported at %s us' % c.failure['t_us'])

    # 5. An OS service that fails to start ends the session with a named record, not a hang.
    for fault, group in ((1, 'THERMAL'), (2, 'THERMAL'), (3, 'PMIC')):
        c = run_case('service-start-fault-%d' % fault, os_elf, scenario(fault=fault, usb_ready_ms=usb_delay_ms))
        c.failed_with(group)
        c.require(int(c.failure['t_us']) < 1_000_000, 'the record came late: %s us' % c.failure['t_us'])

    # 6. The latency CPU control reaches its main entry on the Cortex-A76 pair's first core.
    c = run_case('latency-main-entry', latency_elf, scenario(linger_us=1, usb_ready_ms=usb_delay_ms), None)
    c.clean()
    c.require(c.main is not None and c.main['name'] == 'latency' and c.main['core'] == '6' and c.main['live'] == '1',
              'latency did not reach its main entry on core 6')
    # Left to run, it stops at its first CPU-point check: the OS CPU clock owner is not modeled,
    # so the clock record never shows the top operating points (apps/latency/native.c, 0xb03).
    c = run_case('latency-clock-not-modeled', latency_elf, scenario(usb_ready_ms=usb_delay_ms), None)
    c.failed_with('APPLICATION', 0xb03)
    c.require(c.main is not None and c.main['name'] == 'latency', 'latency did not reach its main entry')

    # 7. The resident lifetime: a supervisor renews the four-second watchdog while every
    # service reports. A healthy session outlives the first expiry; a transport that stalls
    # after it was ready is named by the supervisor; a transport that never reports is
    # reset by the watchdog at the first expiry.
    c = run_case('resident-normal-start', resident_elf, scenario(usb_ready_ms=usb_delay_ms, hold_ms=5000))
    c.clean()
    c.require(c.finished is not None and bytes.fromhex(c.finished['result']) == expected
              and int(c.finished['t_us']) >= 5_000_000, 'the resident session did not outlive the first watchdog expiry')
    c = run_case('resident-service-stall', resident_elf, scenario(fault=5, usb_ready_ms=usb_delay_ms, hold_ms=4000))
    c.failed_with('BOARD')
    c.require('STOP status=5 detail=2' in c.output, 'the supervisor did not name the stalled transport')
    c.require(1_000_000 <= int(c.failure['t_us']) < 2_500_000, 'the stall was reported at %s us' % c.failure['t_us'])
    c = run_case('resident-service-silent', resident_elf, scenario(fault=4, usb_ready_ms=usb_delay_ms, limit_ms=9000))
    # At the loader's first expiry (4 s) the supervisor, which runs every 20 ms, refuses renewal because the
    # transport never reported, and the armed watchdog expires at the same instant. Which happens first
    # depends on the phase between the two, which the deterministic clock fixes for a given image. Both
    # are the designed stop: the board's reset, or the supervisor's named stop of the silent transport.
    reset = lines(c.output, 'WATCHDOG-RESET')
    if reset:
        c.require(c.status == 5 and 4_000_000 <= int(reset[0]['t_us']) < 4_300_000, 'the reset came at the wrong time')
    else:
        c.failed_with('BOARD')
        c.require('STOP status=5 detail=2' in c.output, 'the supervisor did not name the silent transport')
        c.require(4_000_000 <= int(c.failure['t_us']) < 4_300_000, 'the stop came at %s us' % c.failure['t_us'])
    c.require(c.main is None, 'the application started with the transport silent')

    # 8. A data abort at EL0 (the failure return's admission, apps/os/cpu-return-el0.S): the real
    # translation fault reaches the EL1 vector, which abandons the task on a reserved stack. The
    # board's function records the application failure with the exception class, the faulting
    # address and the faulting instruction, and suspends the task. The other cores go on until the
    # monitor ends the run, with no unhandled-exception report.
    c = run_case('el0-data-abort', el0_elf, scenario(usb_ready_ms=usb_delay_ms))
    c.failed_with('APPLICATION', 0xe24)
    c.require('EXCEPTION' not in c.output, 'the vector did not admit the EL0 exception')
    c.require(c.main is not None and c.main['name'] == 'el0-abort' and c.main['core'] == '2', 'the application did not start')
    c.require(int(c.failure['value'], 16) == 0x7a000000, 'the record holds the address %s' % c.failure['value'])
    # Bit 31 marks a return address, bits 30:16 hold ESR bits 14:0 (WnR and a level 2 translation fault: 0x46).
    c.require(int(c.failure['site'], 16) == 0x80464e24, 'the record holds the site %s' % c.failure['site'])
    symbol = [x for x in host.ELFFile(open(el0_elf, 'rb')).get_section_by_name('.symtab').iter_symbols()
              if x.name == 'el0_abort_main'][0]
    c.require(symbol['st_value'] <= int(c.failure['caller'], 16) < symbol['st_value'] + symbol['st_size'],
              'the faulting instruction %s lies outside the application main' % c.failure['caller'])
    c.require(c.failure['core'] == '2', 'the fault was taken on core %s' % c.failure['core'])
    state = lines(c.output, 'APP-TASK-STATE')
    c.require(state and state[0]['state'] == '3', 'the faulting task is not suspended: %s' % state)
    c.require(c.finished is None, 'the application went on past the fault')

    # 9. The private owner scheduler on the same board. The board's first task changes its core
    # affinity before the scheduler starts, and the cores enter with an SP_EL0 that lies in the
    # read-only code block (platform/qemu-os/start.S). The vectors check SP_EL0 at every exception
    # taken from a task, which is where the port saves a context: it must lie in a task stack.
    c = run_case('partitioned-normal-start', partitioned_elf, scenario(usb_ready_ms=usb_delay_ms))
    c.clean()
    c.require(c.main is not None and c.main['name'] == 'hello-compute' and c.main['live'] == '1'
              and c.main['core'] == '2', 'the application did not reach its main entry with the OS live')
    c.require(c.finished is not None and c.finished['status'] == '1'
              and bytes.fromhex(c.finished['result']) == expected, 'the application did not finish with the right result')
    c = run_case('partitioned-latency-main-entry', partitioned_latency_elf, scenario(linger_us=1, usb_ready_ms=usb_delay_ms), None)
    c.clean()
    c.require(c.main is not None and c.main['name'] == 'latency' and c.main['core'] == '6' and c.main['live'] == '1',
              'latency did not reach its main entry on core 6')
    # Negative control: the scheduler as it was when the board run failed. The first yield is taken
    # before any task has installed its stack, on core 0, with SP_EL0 in the code block.
    c = run_case('partitioned-prestart-yield-reverted', prestart_elf, scenario(usb_ready_ms=usb_delay_ms))
    c.require(c.status == 6 and lines(c.output, 'SP-EL0-VIOLATION'), 'the old scheduler passed the SP_EL0 check')
    violation = lines(c.output, 'SP-EL0-VIOLATION')[0]
    c.require(violation['core'] == '0' and 0x41000000 <= int(violation['SP_EL0'], 16) < 0x41200000,
              'the violation is not the code-block SP_EL0 on core 0: %s' % violation['line'])
    c.require(c.main is None, 'the application started after the violation')

    elapsed = time.monotonic() - started
    summary = {'passed': True, 'physical_access': False, 'seconds': round(elapsed, 2), 'build_seconds': round(built, 2),
               'qemu_sha256': hashlib.sha256(QEMU.read_bytes()).hexdigest(),
               'images': {p.parent.name: hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in (os_elf, control_elf, latency_elf, resident_elf, el0_elf, partitioned_elf,
                                    partitioned_latency_elf, prestart_elf)},
               'cases': [{'name': r.name, 'status': r.status, 'wall_seconds': round(r.seconds, 3),
                          'failure': r.failure and r.failure['line'], 'main': r.main and r.main['line'],
                          'finished': r.finished and r.finished['line'][:80]} for r in results]}
    (out / 'result.json').write_text(json.dumps(summary, indent=2) + '\n')
    assert elapsed < 120, 'the check took %.0f s; it must finish in under two minutes' % elapsed
    print('PASS qemu-os-boot: %d cases in %.1f s (build %.1f s)' % (len(results), elapsed, built))
    for r in results:
        print('  %-32s status %d  %.2f s' % (r.name, r.status, r.seconds))


if __name__ == '__main__':
    main()
