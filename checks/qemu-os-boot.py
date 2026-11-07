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
GATE_RUNS = [['--output', '{output}']]
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

DEFINES = ['-DSUNSTONE_A733_OS=1', '-DSUNSTONE_A733_SMP_DIAGNOSTIC=1', '-DSUNSTONE_BOARD_APPLICATION=1',
           '-DSUNSTONE_BOARD_SUPERVISED=1', '-DSUNSTONE_BOARD_USB2_DUAL=1', '-DSUNSTONE_TASK_DEADLINES=1',
           '-DSUNSTONE_A733_IRQ_TABLE=1', '-DSUNSTONE_CRUMBS=1']
CFLAGS = ['--target=aarch64-none-elf', '-march=armv8-a', '-mstrict-align', '-mgeneral-regs-only', '-ffreestanding',
          '-fno-builtin', '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
          '-ffunction-sections', '-fdata-sections', '-O2', '-g', '-Wall', '-Wextra', '-include', 'stdint.h']

# OS and board sources that run unmodified.
OS_SOURCES = ['apps/os/application.c', 'apps/os/large.c', 'apps/os/large_load.c', 'drivers/storage/fat32_ro.c', 'kernel/appmem/app_region.c', 'kernel/appmem/large_admit.c', 'kernel/umem/umem.c', 'apps/os/services.c', 'apps/os/capture.c', 'apps/os/session.c', 'apps/os/grants.c', 'apps/os/start.c',
              'apps/os/thermal.c', 'apps/os/cpu.c', 'apps/os/perf.c', 'drivers/clock/a733_cpu_clock.c', 'drivers/power/a733_perf_mode.c', 'apps/os/admission.c', 'apps/board/main.c', 'apps/os/sealed.c',
              'runtime/deadline.c', 'runtime/deadline-a733.c', 'platform/a733-smp-cached/irq_table.c',
              # The I2S capture service is in every image; an ungranted session never opens its stream.
              'apps/os/audio.c', 'platform/a733/dma_pool_session.c', 'platform/a733/dma_startup.c', 'platform/a733/dma_native.c',
              'drivers/dma/dma_memory.c', 'drivers/dma/a733_dma.c', 'drivers/dma/a733_dma_controller.c',
              'drivers/dma/a733_dma_platform.c', 'drivers/dma/a733_dma_platform_native.c', 'drivers/dma/dma_service.c',
              'drivers/dma/dma_service_i2s.c', 'drivers/dma/dma_service_i2s_stream.c', 'drivers/audio/a733_i2s.c',
              'drivers/audio/a733_i2s_stream.c', 'drivers/audio/a733_i2s_platform.c', 'drivers/audio/a733_i2s_platform_native.c',
              'drivers/audio/a733_audio_leases.c', 'drivers/audio/dma_service_i2s_irq.c', 'runtime/device_memory_pool.c',
              # The radio station is in every image; an ungranted session reads neither its profile nor its credential.
              'apps/os/radio.c', 'apps/os/wifi_credential.c',
              # The NPU service is in every image; an ungranted session binds no task, interrupt or register.
              'apps/os/npu.c', 'apps/os/npu-declaration.c', 'apps/os/npu-graph-index.c', 'apps/os/npu-client.c', 'apps/os/npu-irq.c',
              'platform/a733/npu_native.c', 'runtime/counter_clock.c', 'drivers/npu/a733_npu_runtime.c',
              'drivers/npu/a733_npu_runtime_interim.c', 'drivers/npu/npu_pool.c', 'drivers/npu/npu_declaration.c',
              'drivers/npu/npu_service.c', 'drivers/npu/npu_queue.c', 'drivers/npu/npu_runtime_queue.c', 'drivers/npu/npu_idle.c',
              'drivers/npu/a733_npu_sequence.c', 'drivers/npu/a733_npu_controller.c', 'drivers/npu/a733_npu_memory.c',
              'drivers/npu/a733_npu_mmu.c', 'drivers/npu/nbg_model.c', 'drivers/npu/nbg_assembly.c', 'drivers/npu/nbg_checksum.c',
              'drivers/npu/nbg_relocation.c', 'drivers/npu/a733_npu_startup.c', 'drivers/npu/a733_npu_platform.c',
              'drivers/npu/a733_npu_platform_native.c', 'runtime/npu_app.c',
              # and its EL0 ring, which a declaration of more than one job in flight selects.
              'apps/os/npu-queue.c', 'runtime/npu_app_queue.c']
# Driver cores and their adapters, unmodified. The adapters reach registers
# through a733_mmio.h, which the virtual board replaces (platform/qemu-os).
DRIVER_SOURCES = ['drivers/thermal/a733_thermal.c', 'drivers/thermal/a733_thermal_native.c',
                  'drivers/clock/a733_clock_leases.c', 'drivers/power/a733_rail_leases.c',
                  'drivers/power/a733_pmic.c', 'drivers/power/a733_pmic_native.c',
                  'drivers/power/a733_pmic_consumer.c', 'drivers/power/a733_power_domains.c',
                  'drivers/power/a733_power_domains_native.c', 'drivers/pinctrl/a733_pins.c',
                  'drivers/pinctrl/a733_pin_tables.c', 'drivers/pinctrl/a733_pins_native.c',
                  'drivers/gpu/a733_gpu_resources.c', 'drivers/i2c/a733_i2c.c', 'drivers/usb/a733_usb_platform.c',
                  'drivers/storage/storage_digest.c', 'drivers/storage/storage_digest_extensions.c', 'drivers/storage/storage_digest_ce.S', 'platform/a733/shared_locks.c', 'platform/a733/native_usb.c',
                  'platform/a733/counter_clock.c', 'arch/aarch64/device_memory.c', 'runtime/memory.c',
                  'runtime/memmove.c', 'runtime/gpu_call.S']
# Units that include the A733 platform's boot.h.
QUOTE_A733 = {'drivers/gpu/a733_gpu_resources_native.c', 'drivers/i2c/a733_i2c.c'}
# The virtual board.
BOARD_SOURCES = ['platform/qemu-os/start.S', 'platform/qemu-os/platform.c', 'platform/qemu-os/board.c', 'platform/qemu-os/lifetime.c',
                 'platform/qemu-os/mmio.c', 'platform/qemu-os/absent.c', 'platform/qemu-os/snprintf.c', 'drivers/gpu/a733_gpu_resources_native.c',
                 'apps/os/cpu-return-el0.S']


class Image:
    """Compile and link one image of the virtual board.

    Objects are shared between the images of one run when their source and
    flags are the same."""

    def __init__(self, work, services_source=None, extra_defines=(), extra_sources=(), latency=False, partitioned=False):
        self.work = Path(work)
        # False, True, or the directory holding a replacement kernel/scheduler/freertos.c.
        self.partitioned = partitioned
        # Every image admits both lifetimes; the scenario's lifetime picks one at launch. The resident supervisor
        # renews WDT0 while every service reports.
        self.extra_sources = list(extra_sources) + ['apps/os/watchdog.c', 'platform/a733/maintenance_watchdog.c']
        self.latency = latency
        self.services_source = Path(services_source) if services_source else ROOT / 'apps/os/services.c'
        self.defines = DEFINES + list(extra_defines)
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

    def build(self, application, services_source=None, define=(), cache=None, wraps=()):
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
        done = run([LLD, '--error-limit=0', '-nostdlib', '-static', '--gc-sections', wrap, *['--wrap=' + w for w in wraps],
                    *(['--wrap=vApplicationIRQHandler'] if self.latency else []),
                    '-T', ROOT / 'platform/qemu-os/link.ld', '-Map', self.work / 'os.map',
                    '-o', elf, *objects], self.log)
        assert done.returncode == 0, 'link failed:\n' + done.stderr
        return elf


# ---------------------------------------------------------------------------
# What the stage-2 loader seals: the session, the launch contract and the scenario

host = load('os_host_records', ROOT / 'apps/os/host.py')
hw_facts = load('hw_facts', ROOT / 'tools/hw_facts.py')
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


def thermal_service(total, cpu_critical=0):
    """The thermal owner's record (struct os_session after the board prefix)."""
    h = lambda n: hashlib.sha256(n.encode()).digest()
    record = struct.pack('<5Q', OS_SERVICE_MAGIC, 1, total, 0x7e40a1, 0x7e40a2)
    record += h('thermal-initial') + h('thermal-electrical') + h('thermal-clocks') + h('thermal-supervision')
    record += struct.pack('<2I4B', 0, 24000000, 0, 31, 31, 0)
    for hot, hysteresis, critical in ((1500, 1600, 1400),) * 5:
        record += struct.pack('<3H', hot, hysteresis, critical)
    record += bytes(2) + struct.pack('<I', cpu_critical)       # cpu_critical_millicelsius; zero keeps the 110 C default
    assert len(record) == host.SERVICE_BYTES[host.THERMAL], len(record)
    return record


def session_for(elf, input_bytes, cpu_critical=0):
    """The complete sealed session of the image's application and its launch contract."""
    info = host.image_manifest(elf)
    identity = elf_bytes(elf, 'a733_smp_build_id', 32)
    services = {host.THERMAL: thermal_service(host.PREFIX_BYTES + host.SERVICE_BYTES[host.THERMAL], cpu_critical)}
    given = input_bytes if isinstance(input_bytes, list) else [input_bytes] if input_bytes is not None else []
    raw = host.session_bytes(board_prefix(identity), services, info['manifest'], info['sha256'], 0, host.BIND_SID, given)
    epoch, generation, lease = struct.unpack_from('<3Q', raw, 24)
    # The supervised launch contract (apps/board/prepare.py request_bytes): the loader
    # stamps the times and the magic when it launches.
    h = hashlib.sha256(raw).digest()
    contract = struct.pack('<6Q', 0, 2, 224, epoch, generation, generation ^ (2 ** 64 - 1)) + identity
    contract += struct.pack('<14Q', 0, 0, 0, 0, lease, 0, 0, 0, 0, 0, 0, 0, SESSION_ADDRESS, len(raw)) + h
    return raw, contract + bytes(4096 - len(contract)), info


SCENARIO = struct.Struct('<3Q2I5Q5i5iQQQQQQ3Q')


def scenario(fault=0, first_ms=(2, 2, 2, 2, 2), temperature_c=(52, 60, 48, 55, 50), slope_mc=(0,) * 5,
             period_us=25000, usb_ready_ms=30, linger_us=0, limit_ms=8000, trace=0, hold_ms=0, lifetime=1):
    """The board's behavior in one run; see platform/qemu-os/qemu_os.h."""
    first = [INFINITY if t is None else t * 1000 for t in first_ms]
    raw = SCENARIO.pack(0x514f535343454e31, 1, SCENARIO.size, fault, period_us, *first,
                        *[int(t * 1000) for t in temperature_c], *slope_mc,
                        usb_ready_ms * 1000, linger_us, limit_ms * 1000, trace, hold_ms * 1000, lifetime, *([0] * 3))
    return raw


# ---------------------------------------------------------------------------
# Running the image

def adler32(data):
    a, b = 1, 0
    for byte in data:
        a = (a + byte) % 65521
        b = (b + a) % 65521
    return b << 16 | a


# Large regions: the file the loader reads is the first 4 MiB of the region test stream, on a RAM disk.
region_stream = load('region_test_file', ROOT / 'tools/region_test_file.py')
fat_writer = load('fat32_ro_check', ROOT / 'checks/fat32-ro.py')
LARGE_FILE = b''.join(region_stream.stream(4 << 20))[:4 << 20]
LARGE_DIGEST = hashlib.sha256(LARGE_FILE).hexdigest()
LARGE_FOLD = 0
for _word in struct.iter_unpack('<Q', LARGE_FILE):
    LARGE_FOLD ^= _word[0]
RAMDISK_ADDRESS = 0x48000000


def large_manifest_path(path, digest, file, with_digest=True):
    """manifest.json of the large-region test application: one 1 GiB region loaded from a 4 MiB file."""
    manifest = {"name": "large-test", "devices": {"required": [], "optional": []},
                "cpu": {"cluster": "a55", "workers": 1, "priority": 2, "stack_bytes": 8192},
                "regions": [{"name": "input", "kind": "input", "access": ["read"], "bytes": 4096}],
                "large_regions": [{"name": "weights", "bytes": 1 << 30, "source_bytes": len(file), "access": ["read"],
                                   "file": "/sunstone-test/sunstone-region-test.bin", **({"sha256": digest} if with_digest else {})}],
                "deadlines": {"run_us": 2000000, "return_reserve_us": 5000000, "progress_window_us": 1000000},
                "output_limit": 64}
    Path(path).write_text(json.dumps(manifest, indent=2) + '\n')
    return path


def ramdisk(work, tag, tree):
    """A 40 MiB FAT32 volume with the tree, written by the independent writer of checks/fat32-ro.py."""
    part = Path(work) / (tag + '.disk')
    run(['truncate', '-s', str(81920 * 512), part])
    done = run(['/usr/sbin/mkfs.fat', '-F', '32', '-s', '1', '-n', 'SUNSTONE', '-i', '5a733004', part])
    assert done.returncode == 0, done.stderr
    image = bytearray(part.read_bytes())
    volume = fat_writer.Fat32(image, 0)
    volume.populate(tree, {})
    return bytes(image)


def boot(elf, raw_session, contract, scenario_bytes, inputs, log, timeout=60, memory='512M'):
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
               '-cpu', 'cortex-a76', '-smp', '8', '-m', memory, '-nographic', '-monitor', 'none',
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


def reverted_owner(work):
    """apps/os/sd_owner.c as the fourth board submission had it: start() sleeps once on the caller's default notification index,
    so any hint on that index ends the wait before the owner has opened the card."""
    source = (ROOT / 'apps/os/sd_owner.c').read_text()
    begin = source.index('    /* The owner gives the notification once the card is open or has failed.')
    end = source.index('    ready_waiter = NULL;')
    old = ('    if (atomic_load(&ready) == 0) (void)ulTaskNotifyTake(pdTRUE, 8000u);\n')
    source = source[:begin] + old + source[end:]
    marker = '    if (atomic_load(&ready) != 1 && !os_sd_diag[0]) {'
    stop = source.index('    return atomic_load(&ready) == 1;\n}', source.index(marker))
    source = source[:source.index(marker)] + '    if (atomic_load(&ready) != 1 && !os_sd_diag[0]) os_sd_diag[0] = 11;\n' + source[stop:]
    source = source.replace('xTaskNotifyGiveIndexed(ready_waiter, WAIT_INDEX)', 'xTaskNotifyGive(ready_waiter)')
    path = Path(work) / 'sd_owner_reverted.c'
    path.write_text(source)
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
        image = Image(out / 'build-latency', extra_defines=['-DSUNSTONE_LATENCY=1'],
                      extra_sources=['runtime/latency.c',
                                     'runtime/latency_irq.c'], latency=True)
        application = image.application('latency', ['apps/latency/native.c', 'apps/latency/workload.c'],
                                         ROOT / 'apps/latency/manifest.json')
        return image.build(application)

    def build_partitioned(name, latency=False, scheduler=True):
        """The same board composition on the private owner scheduler (configSS_PARTITIONED_SCHEDULER)."""
        if latency:
            image = Image(out / name, extra_defines=['-DSUNSTONE_LATENCY=1'],
                          extra_sources=['runtime/latency.c',
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

    def build_large(name, manifest):
        """An application that declares large regions, on the board with 4 GiB of RAM and the upper map."""
        image = Image(out / name, extra_defines=['-DQEMU_OS_UPPER=1'])
        application = image.application('large-test', ['checks/qemu-os/large-main.c'], manifest)
        return image.build(application)

    def build_6g(name, manifest):
        """The 6 GiB test application (apps/large-6g) with a region of the emulator's size."""
        image = Image(out / name, extra_defines=['-DQEMU_OS_UPPER=1'])
        application = image.application('large-6g', ['apps/large-6g/main.c'], manifest)
        return image.build(application)

    def build_storage_write(owner_source=None):
        """The storage-write application on the write-capable owner (apps/os/sd_owner.c) over the board's RAM-disk card."""
        image = Image(out / ('build-storage-write' + ('-reverted' if owner_source else '')),
                      extra_sources=[str(owner_source or 'apps/os/sd_owner.c'), 'platform/qemu-os/sd_hw.c',
                                                                   'drivers/storage/a733_storage_bounds.c'])
        application = image.application('storage-write', ['apps/storage-write-test/main.c', 'drivers/storage/fat32_rw.c',
                                                           'apps/storage-write-test/sha256_ce.S'],
                                        ROOT / 'apps/storage-write-test/manifest.json')
        return image.build(application)

    def build_storage_write_space():
        """The storage-write image with the upper memory mapped, so that the application has an address space and its
        main task a tag; the owner is started by that task's call."""
        image = Image(out / 'build-storage-write-space', extra_defines=['-DQEMU_OS_UPPER=1', '-DQEMU_OS_SD_OWNER=1'],
                      extra_sources=['apps/os/sd_owner.c', 'platform/qemu-os/sd_hw.c', 'drivers/storage/a733_storage_bounds.c'])
        application = image.application('storage-write', ['apps/storage-write-test/main.c', 'drivers/storage/fat32_rw.c',
                                                           'apps/storage-write-test/sha256_ce.S'],
                                        ROOT / 'apps/storage-write-test/manifest.json')
        return image.build(application)

    def build_notify():
        """An application that is given the OS's notifications while it waits (docs/notifications.md)."""
        image = Image(out / 'build-notify')
        application = image.application('notify-test', ['checks/qemu-os/notify-main.c'], ROOT / 'checks/qemu-os/notify-manifest.json')
        return image.build(application)

    def build_tlbi(name, form, period):
        """The invalidation task of apps/os/tlbi-issuer.c, on core 5."""
        image = Image(out / name, extra_defines=['-DSUNSTONE_TLBI_ISSUER=%d' % form, '-DSUNSTONE_TLBI_PERIOD_TICKS=%d' % period,
                                                 '-DSUNSTONE_TLBI_CORE=5'], extra_sources=['apps/os/tlbi-issuer.c'])
        application = image.application('tlbi-test', ['checks/qemu-os/tlbi-main.c'], ROOT / 'checks/qemu-os/tlbi-manifest.json')
        return image.build(application)

    def build_wake_race():
        """An application whose waits are given their event between the test and the registration."""
        image = Image(out / 'build-wake-race', extra_sources=['checks/qemu-os/wake-race.c'])
        application = image.application('wake-race', ['checks/qemu-os/wake-race-main.c'], ROOT / 'checks/qemu-os/wake-race-manifest.json')
        return image.build(application, wraps=['board_service_blocked_until'])

    def build_heap(name, manifest):
        """An application whose heap is above the linked limit, so that the ledger serves it (step 1 of
        docs/designs/application-memory-layout.md). The C library's sbrk is linked in; its allocator is not."""
        image = Image(out / name, extra_defines=['-DQEMU_OS_UPPER=1'], extra_sources=['runtime/newlib.c'])
        application = image.application('heap-test', ['checks/qemu-os/heap-main.c'], manifest)
        return image.build(application)

    def build_heap_ctor(name, manifest):
        """The heap application whose static constructor takes a block from sbrk before the main task runs."""
        image = Image(out / name, extra_defines=['-DQEMU_OS_UPPER=1', '-DSUNSTONE_NEWLIB=1'], extra_sources=['runtime/newlib.c'])
        application = image.application('heap-test', ['checks/qemu-os/heap-ctor-main.c'], manifest)
        return image.build(application)

    def build_heap_early(name, manifest):
        """The same application, with a call of sbrk before the admission, as a static constructor would make."""
        image = Image(out / name, extra_defines=['-DQEMU_OS_UPPER=1'],
                      extra_sources=['runtime/newlib.c', 'checks/qemu-os/heap-early.c'])
        application = image.application('heap-test', ['checks/qemu-os/heap-main.c'], manifest)
        return image.build(application, wraps=['os_large_prepare'])

    def build_usb_race():
        """An application that is started while the transport becomes ready between its liveness test and its wait."""
        image = Image(out / 'build-usb-race', extra_sources=['checks/qemu-os/usb-race.c'])
        application = image.application('hello-compute', ['apps/hello-compute/main.c'], ROOT / 'apps/hello-compute/manifest.json')
        return image.build(application, wraps=['board_service_blocked_until'])

    def build_slot1():
        """An application with a manifest region in level-1 slot 1 above the EL0 window."""
        image = Image(out / 'build-slot1', extra_defines=['-DQEMU_OS_UPPER=1'])
        application = image.application('slot1-test', ['checks/qemu-os/slot1-main.c'], ROOT / 'checks/qemu-os/slot1-manifest.json')
        return image.build(application)

    def build_upper():
        """The same application with its region in level-1 slot 2, above the unmapped hole (step 6b)."""
        image = Image(out / 'build-upper', extra_defines=['-DQEMU_OS_UPPER=1'])
        application = image.application('slot1-test', ['checks/qemu-os/slot1-main.c'], ROOT / 'checks/qemu-os/upper-manifest.json')
        return image.build(application)

    def build_facts():
        image = Image(out / 'build-hw-facts', extra_defines=['-DSUNSTONE_A733_HW_FACTS=1'],
                      extra_sources=['platform/a733/hw_facts.c'])
        application = image.application('hw-facts', ['apps/hw-facts/main.c'], ROOT / 'apps/hw-facts/manifest.json')
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
        el0_elf = pool.submit(build_el0_abort)
        facts_elf = pool.submit(build_facts)
        large_elf = pool.submit(build_large, 'build-large', large_manifest_path(out / 'large-manifest.json', LARGE_DIGEST, LARGE_FILE))
        large_nodigest_elf = pool.submit(build_large, 'build-large-nodigest', large_manifest_path(out / 'large-nodigest-manifest.json', LARGE_DIGEST, LARGE_FILE, False))
        large_big_elf = pool.submit(build_large, 'build-large-too-big', ROOT / 'checks/qemu-os/large-too-big-manifest.json')
        manifest_6g = json.loads((ROOT / 'apps/large-6g/manifest.json').read_text())
        manifest_6g['large_regions'][0]['bytes'] = 256 << 20
        (out / 'large-6g-manifest.json').write_text(json.dumps(manifest_6g))
        large_6g_elf = pool.submit(build_6g, 'build-large-6g', out / 'large-6g-manifest.json')
        storage_write_elf = pool.submit(build_storage_write)
        heap_elf = pool.submit(build_heap, 'build-heap', ROOT / 'checks/qemu-os/heap-manifest.json')
        heap_big_elf = pool.submit(build_heap, 'build-heap-too-big', ROOT / 'checks/qemu-os/heap-too-big-manifest.json')
        heap_early = pool.submit(build_heap_early, 'build-heap-early', ROOT / 'checks/qemu-os/heap-manifest.json')
        heap_ctor = pool.submit(build_heap_ctor, 'build-heap-ctor', ROOT / 'checks/qemu-os/heap-manifest.json')
        slot1_elf = pool.submit(build_slot1).result()
        heap_elf, heap_big_elf, heap_early_elf = heap_elf.result(), heap_big_elf.result(), heap_early.result()
        notify_elf = pool.submit(build_notify).result()
        tlbi_elfs = {k: pool.submit(build_tlbi, 'build-tlbi-%d-%d' % k, *k).result() for k in ((0, 1), (1, 1), (2, 10), (3, 1))}
        upper_elf = pool.submit(build_upper).result()
        usb_race_elf = pool.submit(build_usb_race).result()
        wake_race_elf = pool.submit(build_wake_race).result()
        storage_write_space_elf = pool.submit(build_storage_write_space).result()
        (out / 'build-reverted-owner').mkdir()
        reverted_owner_elf = pool.submit(build_storage_write, reverted_owner(out / 'build-reverted-owner'))
        large_elf, large_big_elf, large_6g_elf, large_nodigest_elf = large_elf.result(), large_big_elf.result(), large_6g_elf.result(), large_nodigest_elf.result()
        storage_write_elf = storage_write_elf.result()
        reverted_owner_elf = reverted_owner_elf.result()
        os_elf, control_elf, latency_elf, el0_elf, partitioned_elf, partitioned_latency_elf, prestart_elf, facts_elf = (
            os_elf.result(), control_elf.result(), latency_elf.result(), el0_elf.result(),
            partitioned_elf.result(), partitioned_latency_elf.result(), prestart_elf.result(), facts_elf.result())
    built = time.monotonic() - started

    def run_case(name, elf, scenario_bytes, input_bytes=data, memory='512M', extra_inputs=(), cpu_critical=0):
        raw, contract, info = session_for(elf, input_bytes, cpu_critical)
        given = input_bytes if isinstance(input_bytes, list) else [input_bytes] if input_bytes is not None else []
        inputs = [(info['manifest']['regions'][i]['address'], b) for i, b in enumerate(given)]
        inputs += list(extra_inputs)
        output, status, seconds = boot(elf, raw, contract, scenario_bytes, inputs, out / (name + '.log'), memory=memory)
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

    # 4b. A sensor that reads above the critical limit: the stop record carries the reading that caused it (a stop returns no
    # report; the runner reads this record after the reset). Channel 3 at 62 C against a session limit of 60 C (the board model's sensors top out near 65 C).
    c = run_case('sensor-critical-reading', os_elf, scenario(temperature_c=(52, 60, 48, 62, 50), usb_ready_ms=usb_delay_ms, limit_ms=9000), cpu_critical=60000)
    c.failed_with('THERMAL')
    word = int(c.failure['value'], 16)
    c.require(word >> 31 == 1 and not word >> 30 & 1 and (word >> 24 & 7) == 3 and 61_000 <= (word & 0xffffff) <= 63_000,
              'the stop record holds %#x, not channel 3 near 62000 mC' % word)

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
    c = run_case('resident-normal-start', os_elf, scenario(usb_ready_ms=usb_delay_ms, hold_ms=5000, lifetime=2))
    c.clean()
    c.require(c.finished is not None and bytes.fromhex(c.finished['result']) == expected
              and int(c.finished['t_us']) >= 5_000_000, 'the resident session did not outlive the first watchdog expiry')
    c = run_case('resident-service-stall', os_elf, scenario(fault=5, usb_ready_ms=usb_delay_ms, hold_ms=4000, lifetime=2))
    c.failed_with('BOARD')
    c.require('STOP status=5 detail=2' in c.output, 'the supervisor did not name the stalled transport')
    c.require(1_000_000 <= int(c.failure['t_us']) < 2_500_000, 'the stall was reported at %s us' % c.failure['t_us'])
    c = run_case('resident-service-silent', os_elf, scenario(fault=4, usb_ready_ms=usb_delay_ms, limit_ms=9000, lifetime=2))
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

    # 10. The hardware facts probe (docs/hardware/a733-facts-probe.md): every core records its registers at EL1 before
    # the scheduler, the application returns the table, and the host decoder reads it. QEMU's GICv3 is not the
    # A733's, so the table names the virtual board and can never be merged into the hardware facts.
    c = run_case('hw-facts', facts_elf, scenario(usb_ready_ms=usb_delay_ms), None)
    c.clean()
    c.require(c.finished is not None and c.finished['payload_bytes'] == str(hw_facts.TABLE_BYTES),
              'the application did not return the facts table')
    chunks = sorted((int(l['offset']), l['hex']) for l in lines(c.output, 'APP-RESULT'))
    c.require([o for o, _ in chunks] == list(range(0, hw_facts.TABLE_BYTES, 96)), 'the facts table was printed in pieces that do not join')
    table = bytes.fromhex(''.join(h for _, h in chunks))
    (out / 'hw-facts-table.bin').write_bytes(table)
    header, facts = hw_facts.decode(table)
    c.require(header['platform'] == 'qemu-os-board', 'the table names %s' % header['platform'])
    by = {(f['name'], f['core']): f for f in facts}
    c.require(all(by[('cpu.midr_el1.partnum', k)]['value'] == 0xd0b and by[('cpu.cntfrq_el0', k)]['value'] == 24000000
                  for k in range(8)), 'a core did not report the Cortex-A76 and the declared 24 MHz counter')
    c.require(len({by[('cpu.mpidr_el1', k)]['value'] for k in range(8)}) == 8, 'the cores do not report eight different MPIDR values')
    c.require(by[('gic.dist.gicd_ctlr.ds1_ds', 0)]['value'] == 1 and by[('gic.dist.gicd_ctlr.ds1_are', 0)]['value'] == 1, 'GICD_CTLR does not have DS and ARE_S set as the virtual board')
    c.require(by[('gic.redist.intid27.priority_after_write', 0)]['value'] == by[('gic.redist.intid27.priority_after_write', 0)]['written'],
              'the PPI 27 write test did not read back what it wrote')
    c.require(by[('gic.dist.spi97.route_requested', 0)]['value'] == 0x100d8, 'the probe image routes INTID 97 at 0xd8 on core 0')
    c.require(by[('mmio.0x07064000', 0)]['status'] == 'unmeasured', 'the PPU word was read although its gate is closed')

    # 11. Large regions. The OS takes them from the ledger before the scheduler starts and a loader task fills them from
    # a RAM disk through the FAT32 reader and two buffers. The application waits for the region, reads it at EL0 in its
    # own address space and reports.
    good = ramdisk(out, 'disk-good', {'sunstone-test': {'sunstone-region-test.bin': LARGE_FILE}})
    wrong = bytearray(LARGE_FILE)
    wrong[1 << 20] ^= 1
    corrupt = ramdisk(out, 'disk-corrupt', {'sunstone-test': {'sunstone-region-test.bin': bytes(wrong)}})
    empty = ramdisk(out, 'disk-empty', {'unrelated.txt': b'no test file here\n'})
    mode0 = struct.pack('<QQ', 0, LARGE_FOLD) + bytes(4080)
    mode1 = struct.pack('<QQ', 1, LARGE_FOLD) + bytes(4080)
    c = run_case('large-region-loaded', large_elf, scenario(usb_ready_ms=usb_delay_ms), mode0, '4G',
                 [(RAMDISK_ADDRESS, good)])
    c.clean()
    admitted = lines(c.output, 'LARGE-ADMITTED')
    c.require(len(admitted) == 1 and (int(admitted[0]['base'], 16), int(admitted[0]['bytes'], 16)) == (0x80000000, 1 << 30)
              and admitted[0]['state'] == '0' and admitted[0]['asid'] == '1',
              'admission differs: %s' % [a['line'] for a in admitted])
    c.require(c.finished is not None and c.finished['status'] == '1', 'the application did not finish')
    tag, fold = struct.unpack('<QQ', bytes.fromhex(c.finished['result']))
    c.require(tag == 0x4c4152474f4b and fold == LARGE_FOLD, 'result %#x %#x: the region did not read back as the file' % (tag, fold))
    # The file is absent: the loader reports it, the application sees a failed region and returns normally.
    c = run_case('large-region-file-absent', large_elf, scenario(usb_ready_ms=usb_delay_ms), mode0, '4G', [(RAMDISK_ADDRESS, empty)])
    c.clean()
    tag, error = struct.unpack('<QQ', bytes.fromhex(c.finished['result']))
    c.require(c.finished['status'] == '1' and tag == 0x4c415247464c and error == 0xfffffffa, 'absent file: result %#x %#x' % (tag, error))
    # The file differs from the declared digest in one bit: the region never becomes ready.
    c = run_case('large-region-digest-mismatch', large_elf, scenario(usb_ready_ms=usb_delay_ms), mode0, '4G', [(RAMDISK_ADDRESS, corrupt)])
    c.clean()
    tag, error = struct.unpack('<QQ', bytes.fromhex(c.finished['result']))
    c.require(tag == 0x4c415247464c and error == 0xfffffff5, 'digest mismatch: result %#x %#x' % (tag, error))
    # A region declared without a digest is not hashed: the corrupted file loads, the loader reports READY, and the
    # application (which compares the fold itself) sees the corruption.
    c = run_case('large-region-no-digest', large_nodigest_elf, scenario(usb_ready_ms=usb_delay_ms), mode0, '4G', [(RAMDISK_ADDRESS, corrupt)])
    c.clean()
    c.require(c.finished is not None and c.finished['status'] == '1', 'the application did not finish')
    tag, fold = struct.unpack('<QQ', bytes.fromhex(c.finished['result']))
    c.require(tag == 0x4c4152474d4d, 'no-digest load: result tag %#x, not a loaded region with a differing fold' % tag)
    # A region with no source is ready at once, above 4 GiB it would not fit the emulator, so the emulator's region is 256 MiB: The emulator's level-1 slot 1 starts at 0x50000000, where the ledger places it.
    # the application times its passes, writes and reads back the address pattern, and reports no mismatch.
    c = run_case('large-region-unfilled', large_6g_elf, scenario(usb_ready_ms=usb_delay_ms), None, '4G')
    c.clean()
    c.require(c.finished is not None and c.finished['status'] == '1', 'the application did not finish')
    parts = {int(o): h for o, h in re.findall(r'APP-RESULT offset=(\d+) hex=([0-9a-f]+)', c.output)}
    words = struct.unpack('<64Q', bytes.fromhex(''.join(parts[o] for o in sorted(parts))))
    c.require(words[0] == 0x36474f4e45 and words[1] == 2 and words[2] == 0, 'tag/state/error %#x %d %d' % words[:3])
    c.require(words[3] == 256 << 20 and words[4] == 0x50000000, 'region %#x at %#x' % (words[3], words[4]))
    c.require(all(t > 0 for t in words[8:8 + 24 + 6]), 'a pass has no time: %s' % (words[8:38],))
    c.require(words[38] == (256 << 20) // 8 and words[39] == 0, 'integrity: %d words, %d mismatches' % (words[38], words[39]))
    # A second EL0 task that runs in no application space reads the region: a permission fault, reported with the region's address.
    c = run_case('large-region-other-space-faults', large_elf, scenario(usb_ready_ms=usb_delay_ms), mode1, '4G', [(RAMDISK_ADDRESS, good)])
    c.failed_with('APPLICATION', 0xe24)
    c.require(int(c.failure['value'], 16) == 0x80000000, 'the fault address is %s' % c.failure['value'])
    c.require(int(c.failure['site'], 16) == 0x800d4e24, 'the record holds the site %s: not a level 1 permission fault on a read' % c.failure['site'])
    # The provider is present but the device fails: at the metadata, at the start of a request, in the middle of the
    # stream, or there is no device. The loader fails the region with its status; the application is told and returns.
    # The file is larger than five requests (4 MiB = 32 requests of 128 KiB), so the mid-stream failure falls inside it.
    for fault, name, status in ((6, 'mount', 0xfffffffe), (7, 'submit', 0xffffffff), (8, 'midway', 0xffffffff), (9, 'absent', 0xffffffff)):
        c = run_case('large-device-%s' % name, large_elf, scenario(fault=fault, usb_ready_ms=usb_delay_ms), mode0, '4G',
                     [(RAMDISK_ADDRESS, good)])
        c.clean()
        tag, error = struct.unpack('<QQ', bytes.fromhex(c.finished['result']))
        c.require(c.finished['status'] == '1' and tag == 0x4c415247464c and error == status,
                  'device fault %s: result %#x %#x, expected status %#x' % (name, tag, error, status))

    # The task switch: a round trip between two tasks on one core, with and without a TTBR0 write at each switch.
    mode2 = struct.pack('<QQ', 2, LARGE_FOLD) + bytes(4080)
    c = run_case('large-region-switch-cost', large_elf, scenario(usb_ready_ms=usb_delay_ms), mode2, '4G', [(RAMDISK_ADDRESS, good)])
    c.clean()
    tag, same, different, rounds = struct.unpack('<4Q', bytes.fromhex(c.finished['result']))
    c.require(tag == 0x4c4152474245 and rounds == 20000, 'switch benchmark result %#x' % tag)
    switch_cost = {'rounds': rounds, 'same_space_ticks': same, 'different_space_ticks': different,
                   'note': 'QEMU instruction time (-icount shift=0), not hardware cycles'}

    # Regions that exceed the board's upper memory stop the start before any task runs.
    c = run_case('large-regions-too-big', large_big_elf, scenario(usb_ready_ms=usb_delay_ms), memory='4G')
    c.require(c.main is None and not lines(c.output, 'LARGE-ADMITTED'), 'the application started although the ledger refused its regions')
    c.require(c.failure is not None, 'the refusal left no failure record')

    # A heap above the linked limit is a ledger region (step 1 of docs/designs/application-memory-layout.md). The
    # memory behind it holds a pattern before the boot; the heap must reach the application zeroed.
    stale = bytes([0xa5]) * (8 << 20)
    heap_input = lambda mode: struct.pack('<Q', mode) + bytes(4088)
    c = run_case('heap-from-ledger', heap_elf, scenario(usb_ready_ms=usb_delay_ms), heap_input(0), '4G', [(0x50000000, stale)])
    c.clean()
    admitted = lines(c.output, 'LARGE-ADMITTED')
    c.require(len(admitted) == 1 and (int(admitted[0]['base'], 16), int(admitted[0]['bytes'], 16)) == (0x50000000, 8 << 20)
              and admitted[0]['state'] == '2' and admitted[0]['asid'] == '1',
              'heap admission differs: %s' % [a['line'] for a in admitted])
    c.require(c.finished is not None and c.finished['status'] == '1', 'the heap application did not finish')
    tag, base, size, peak, error = struct.unpack('<5Q', bytes.fromhex(c.finished['result']))
    c.require(tag == 0x48454150204f4b and base == 0x50000000 and size == 8 << 20 and peak == 8 << 20 and error == 12,
              'heap result tag %#x base %#x bytes %#x peak %#x errno %d' % (tag, base, size, peak, error))
    # A task that is in no application space creates a task: the child stays out, and its read of the heap is a
    # permission fault at the first byte.
    c = run_case('heap-other-space-faults', heap_elf, scenario(usb_ready_ms=usb_delay_ms), heap_input(1), '4G', [(0x50000000, stale)])
    c.failed_with('APPLICATION', 0xe24)
    c.require(int(c.failure['value'], 16) == 0x50000000 and int(c.failure['site'], 16) == 0x800e4e24,
              'heap fault address %s, site %s' % (c.failure['value'], c.failure['site']))
    # The application's own space ends at the heap: the next byte is a kernel block, a permission fault.
    c = run_case('heap-end-faults', heap_elf, scenario(usb_ready_ms=usb_delay_ms), heap_input(2), '4G', [(0x50000000, stale)])
    c.failed_with('APPLICATION', 0xe24)
    c.require(int(c.failure['value'], 16) == 0x50800000, 'the byte after the heap faults at %s' % c.failure['value'])
    # A worker attached to the application's space writes the heap and the main task reads it.
    c = run_case('heap-worker-shares', heap_elf, scenario(usb_ready_ms=usb_delay_ms), heap_input(3), '4G', [(0x50000000, stale)])
    c.clean()
    tag, base, size, peak, fold = struct.unpack('<5Q', bytes.fromhex(c.finished['result']))
    expected_fold = 0
    for i in range(4096):
        expected_fold ^= (0x9e3779b97f4a7c15 * (i + 1)) & (2 ** 64 - 1)
    c.require(tag == 0x48454150204f4b and fold == expected_fold and peak == 1, 'worker result %#x fold %#x, expected %#x; writer shares the main task tag: %d' % (tag, fold, expected_fold, peak))
    # A task of a task of the application creates a task that writes the heap: no call puts any of them in the space.
    c = run_case('heap-grandchild-shares', heap_elf, scenario(usb_ready_ms=usb_delay_ms), heap_input(4), '4G', [(0x50000000, stale)])
    c.clean()
    tag, base, size, peak, fold = struct.unpack('<5Q', bytes.fromhex(c.finished['result']))
    c.require(tag == 0x48454150204f4b and fold == expected_fold and peak == 1, 'grandchild result %#x fold %#x, expected %#x; writer shares the main task tag: %d' % (tag, fold, expected_fold, peak))
    # The linked heap has handed out a byte before the admission, as a static constructor that calls malloc would
    # make it: the heap of the ledger cannot replace it, so the application does not start.
    c = run_case('heap-after-early-sbrk', heap_early_elf, scenario(usb_ready_ms=usb_delay_ms), heap_input(0), '4G', [(0x50000000, stale)])
    c.require(c.main is None and c.failure is not None, 'the application started although the linked heap was already in use')
    # A static constructor that takes memory from sbrk runs after the admission attached the region as the heap: its block is
    # the first 4096 bytes of the region, zero although the memory held a pattern, and the main task's block follows it.
    c = run_case('heap-constructor-allocates', heap_ctor.result(), scenario(usb_ready_ms=usb_delay_ms), heap_input(0), '4G', [(0x50000000, stale)])
    c.clean()
    tag, block, dirty, own, peak = struct.unpack('<5Q', bytes.fromhex(c.finished['result']))
    c.require(tag == 0x4354524f48454150 and block == 0x50000000 and dirty == 0 and own == 0x50001000 and peak == 8192,
              'constructor result tag %#x block %#x dirty %d main block %#x peak %d' % (tag, block, dirty, own, peak))
    # A manifest region above the 48 MiB window lies in level-1 slot 1 and is mapped for its application alone.
    c = run_case('region-in-slot-1', slot1_elf, scenario(usb_ready_ms=usb_delay_ms), heap_input(0), '4G')
    c.clean()
    admitted = lines(c.output, 'LARGE-ADMITTED')
    tag, address, size, fold = struct.unpack('<4Q', bytes.fromhex(c.finished['result']))
    expected_fold = 0xffffffffffffffff
    for i in range(0, (64 << 20) // 8, 512):
        expected_fold ^= (0x9e3779b97f4a7c15 * (i + 1)) & (2 ** 64 - 1)
    c.require(tag == 0x534c4f5431 and address == 0x51000000 and size == 64 << 20 and fold == expected_fold,
              'slot 1 region result %#x at %#x, %#x bytes, fold %#x' % (tag, address, size, fold))
    c = run_case('region-in-slot-1-end-faults', slot1_elf, scenario(usb_ready_ms=usb_delay_ms), heap_input(1), '4G')
    c.failed_with('APPLICATION', 0xe24)
    c.require(int(c.failure['value'], 16) == 0x55000000, 'the byte after the slot 1 region faults at %s' % c.failure['value'])

    # A manifest region from 0x80000000: level-1 slot 2, mapped for its application alone; the byte after it is a kernel block.
    c = run_case('region-in-upper', upper_elf, scenario(usb_ready_ms=usb_delay_ms), heap_input(0), '4G')
    c.clean()
    tag, address, size, fold = struct.unpack('<4Q', bytes.fromhex(c.finished['result']))
    expected_fold = 0xffffffffffffffff
    for i in range(0, (64 << 20) // 8, 512):
        expected_fold ^= (0x9e3779b97f4a7c15 * (i + 1)) & (2 ** 64 - 1)
    c.require(tag == 0x534c4f5431 and address == 0x88000000 and size == 64 << 20 and fold == expected_fold,
              'upper region result %#x at %#x, %#x bytes, fold %#x' % (tag, address, size, fold))
    c = run_case('region-in-upper-end-faults', upper_elf, scenario(usb_ready_ms=usb_delay_ms), heap_input(1), '4G')
    c.failed_with('APPLICATION', 0xe24)
    c.require(int(c.failure['value'], 16) == 0x8c000000, 'the byte after the upper region faults at %s' % c.failure['value'])

    # The main stack comes from upper memory with an unmapped guard block below it: an overflow is an EL0 fault in the guard.
    c = run_case('main-stack-overflow-hits-guard', slot1_elf, scenario(usb_ready_ms=usb_delay_ms), heap_input(2), '4G')
    c.failed_with('APPLICATION', 0xe24)
    guard = int(c.failure['value'], 16)
    c.require(0x50000000 <= guard < 0x50200000, 'the overflow faulted at %#x, not in the guard block 0x50000000..0x50200000' % guard)

    # A task that the application creates in its init hook, which the OS calls before the scheduler where no task creates
    # anything, runs in the application's space: it stores to the main task's ledger stack and to a region, and the main
    # task reads both. The child carries the main task's tag.
    c = run_case('init-hook-task-shares-space', slot1_elf, scenario(usb_ready_ms=usb_delay_ms), heap_input(3), '4G')
    c.clean()
    tag, stack_word, region_word, child_tag = struct.unpack('<4Q', bytes.fromhex(c.finished['result']))
    c.require(tag == 0x534c4f5433 and stack_word == 0x1111222233334444 and region_word == 0x5555666677778888 and child_tag != 0,
              'init-hook task result %#x stack %#x region %#x tag %#x' % (tag, stack_word, region_word, child_tag))

    # The transport becomes ready between the main task's liveness test and its wait: the wake-up finds no waiter, and the
    # task must still start, not sleep to the session deadline.
    c = run_case('usb-ready-between-test-and-wait', usb_race_elf, scenario(usb_ready_ms=60000, limit_ms=4000))
    c.clean()
    c.require(c.main is not None and c.main['live'] == '1' and int(c.main['t_us']) < 2_000_000,
              'the application did not start after the transport became ready between its test and its wait')
    c.require(c.finished is not None and c.finished['status'] == '1', 'the application did not finish')

    # The invalidation task of the scheduler soak (apps/os/tlbi-issuer.c): one wake per period on core 5, at the EL1 the TLBI needs,
    # an invalidation at each wake in the forms 1 to 3 and none in the control (form 0).
    for (form, period), elf in tlbi_elfs.items():
        c = run_case('tlbi-issuer-form-%d-period-%d' % (form, period), elf, scenario(usb_ready_ms=usb_delay_ms), bytes(4096))
        c.clean()
        tag, wakes, issued, core, largest = struct.unpack('<5Q', bytes.fromhex(c.finished['result']))
        expected = 200 // period
        c.require(tag == 0x54424c49 and core == 5 and expected * 0.6 <= wakes <= expected * 1.2,
                  'form %d: %d wakes on core %d in 200 ms, expected about %d on core 5' % (form, wakes, core, expected))
        c.require(issued == (wakes if form else 0) and (largest > 0) == bool(form),
                  'form %d: %d invalidations for %d wakes, largest %d ticks' % (form, issued, wakes, largest))
    # An OS wake-up between the test and the registration of the wait is kept: a host request offered there, and a USB
    # state change there, end ss_host_recv and ss_usb_wait at the event and not at their two-second timeout.
    for mode, what in ((0, 'host-request'), (1, 'usb-change')):
        c = run_case('wake-between-test-and-wait-' + what, wake_race_elf, scenario(usb_ready_ms=usb_delay_ms),
                     struct.pack('<Q', mode) + bytes(4088))
        c.clean()
        tag, result, e = struct.unpack('<3Q', bytes.fromhex(c.finished['result']))
        c.require(tag == 0x57414b45 and result == 1 and e < 500000,
                  '%s between the test and the wait ended the call after %d us with result %d' % (what, e, result))

    # The OS gives the main task its usb-ready and live-state notifications while the main task sleeps in the deadline
    # wait and in ss_host_recv: each ends at its own condition (the deadline; the timeout), not at the OS's notification.
    c = run_case('os-notifications-do-not-end-waits', notify_elf, scenario(usb_ready_ms=usb_delay_ms), bytes(4096))
    c.clean()
    tag, wait, e1, recv, e2 = struct.unpack('<5q', bytes.fromhex(c.finished['result']))
    c.require(wait == 1 and e1 >= 30000, 'the deadline wait ended with status %d after %d us, expected expiry after 30000 us' % (wait, e1))
    c.require(recv == 0 and e2 >= 30000, 'ss_host_recv returned %d after %d us, expected the timeout (0) after 30000 us' % (recv, e2))
    # Index 0 belongs to the application: its own give during an OS wait, and during a deadline wait, is still there afterwards.
    c = run_case('application-give-survives-os-wait', notify_elf, scenario(usb_ready_ms=usb_delay_ms), struct.pack('<Q', 1) + bytes(4088))
    c.clean()
    tag, recv, e, take = struct.unpack('<4q', bytes.fromhex(c.finished['result']))
    c.require(recv == 0 and e >= 30000 and take == 1,
              'ss_host_recv returned %d after %d us and the application find %d notifications at index 0; expected the timeout and its own give' % (recv, e, take))
    c = run_case('application-give-does-not-end-deadline-wait', notify_elf, scenario(usb_ready_ms=usb_delay_ms), struct.pack('<Q', 2) + bytes(4088))
    c.clean()
    tag, wait, e, take = struct.unpack('<4q', bytes.fromhex(c.finished['result']))
    c.require(wait == 1 and e >= 30000 and take == 1,
              'the deadline wait ended with status %d after %d us, take %d; expected expiry after 30000 us and the give still pending' % (wait, e, take))
    # A device hint ends the deadline wait early; nothing of it may end the application's next wait on index 0.
    c = run_case('deadline-hint-leaves-index-0-alone', notify_elf, scenario(usb_ready_ms=usb_delay_ms), struct.pack('<Q', 3) + bytes(4088))
    c.clean()
    tag, wait, e, take, e3 = struct.unpack('<5q', bytes.fromhex(c.finished['result']))
    c.require(wait == 0 and e < 20000, 'the hint did not end the deadline wait early: status %d after %d us' % (wait, e))
    c.require(take == 0 and e3 >= 38000, 'the next wait on index 0 ended after %d us with %d notifications; expected the 40 ms timeout' % (e3, take))

    # 12. The storage-write application through ss_storage_enable on the write-capable owner (apps/os/sd_owner.c), over a RAM-disk
    # card that holds the real card's 395 backup sectors at their sector numbers and the runner's bounds blob. The software path
    # (owner start, mailbox, blob copy, parse, verify against the backup hash) must succeed; the native register binding is the one
    # step this board cannot model (platform/qemu-os/sd_hw.c), so the case stops at it and says so. A corrupted backup sector must
    # be refused at the verify stage, which also shows that the diagnostic names it.
    backup_dir = CACHE / 'sd-backup'
    backup = (backup_dir / 'backup-395-sectors.bin').read_bytes()
    # The recorded blob is version 2 (no card identity). Re-encode its ranges and backup hash with the current encoder
    # (tools/storage_bounds.py encode_input, version 3). The QEMU card cannot report a CID, so a fixed one is written; the CID
    # comparison happens at the native register binding, which this board does not model.
    sb = load('storage_bounds_encoder', ROOT / 'tools/storage_bounds.py')
    old = (backup_dir / 'bounds-input.bin').read_bytes()
    assert old[:8] == b'SSBW0002', 'the recorded bounds blob changed format'
    n_write, n_backup = struct.unpack_from('<II', old, 8)
    pairs = [struct.unpack_from('<QQ', old, 48 + 16 * k) for k in range(n_write + n_backup)]
    blob = sb.encode_input(pairs[:n_write], pairs[n_write:], old[16:48].hex(), '00112233445566778899aabbccddeeff')
    storage_host = load('storage_write_host', ROOT / 'apps/storage-write-test/host.py')

    def card_image(corrupt=None):
        disk = bytearray(65536 * 512)
        for at in range(0, len(backup), 520):
            sector = struct.unpack_from('<Q', backup, at)[0]
            disk[sector * 512:(sector + 1) * 512] = backup[at + 8:at + 520]
        if corrupt is not None:
            disk[corrupt * 512 + 100] ^= 1
        return bytes(disk)

    def decode_result(c):
        chunks = sorted((int(l['offset']), l['hex']) for l in lines(c.output, 'APP-RESULT'))
        c.require(chunks and [o for o, _ in chunks] == list(range(0, 512, 96)), 'the 512-byte result was not printed whole')
        return storage_host.decode(bytes.fromhex(''.join(h for _, h in chunks)))
    mode4 = storage_host.mode_input(4)
    c = run_case('storage-write-enable', storage_write_elf, scenario(usb_ready_ms=usb_delay_ms), [blob, mode4], '512M',
                 [(RAMDISK_ADDRESS, card_image())])
    c.clean()
    c.require(c.finished is not None, 'the application did not finish')
    d = decode_result(c)
    c.require(d['status'] == 0 and d['au_sectors'] == 8192, 'enable did not succeed: status %d, diagnostic %s' % (d['status'], [d['diag%d' % i] for i in range(4)]))
    # The SD owner is started by the application's own call, from a task that has an address space: the owner is an OS task
    # and must end up with no application tag, so that it runs in the kernel's space (include/os_space.h, traceTASK_CREATE
    # gives a new task its creator's tag; apps/os/sd_owner.c clears it). The main task, by contrast, is tagged.
    c = run_case('storage-write-owner-in-kernel-space', storage_write_space_elf, scenario(usb_ready_ms=usb_delay_ms), [blob, mode4], '4G',
                 [(RAMDISK_ADDRESS, card_image())])
    c.clean()
    d = decode_result(c)
    c.require(d['status'] == 0 and d['au_sectors'] == 8192, 'enable did not succeed with an address space: status %d' % d['status'])
    space = lines(c.output, 'TASK-SPACE')
    c.require(len(space) == 1 and space[0].get('main') == '1' and space[0].get('sdowner') == '0' and space[0].get('sdentry') == '0',
              'the application main task must be tagged and the SD owner untagged, and in the kernel\'s table at its first instruction: %s' % (space,))
    # A hint on the application's default notification index during the owner's open (a USB-change event, as application.c sends
    # them) must not end the wait: enable succeeds. The owner as the fourth board submission had it (one sleep on that index)
    # returns early with stage 11, which is the control.
    stray = scenario(usb_ready_ms=usb_delay_ms, fault=10)
    c = run_case('storage-write-enable-stray-notification', storage_write_elf, stray, [blob, mode4], '512M', [(RAMDISK_ADDRESS, card_image())])
    c.clean()
    d = decode_result(c)
    c.require(d['status'] == 0 and d['au_sectors'] == 8192, 'a stray notification ended the owner wait: status %d, diagnostic %s' % (d['status'], [d['diag%d' % i] for i in range(4)]))
    c = run_case('storage-write-enable-stray-reverted', reverted_owner_elf, stray, [blob, mode4], '512M', [(RAMDISK_ADDRESS, card_image())])
    c.clean()
    d = decode_result(c)
    c.require(d['status'] == 1 and d['diag0'] == 11, 'the earlier owner wait survived the stray notification: status %d, diagnostic %s' % (d['status'], [d['diag%d' % i] for i in range(4)]))
    # A retained platform register lock (what a failed controller transaction leaves): the thermal service cannot read its sensors
    # and, after the five seconds without a reading, ends the session as CRITICAL for lack of a reading, with the stale marker and
    # the channel in the record (nothing in the SD path is at fault in this record).
    c = run_case('storage-write-lock-retained', storage_write_elf, scenario(usb_ready_ms=usb_delay_ms, fault=11, limit_ms=14000),
                 [blob, mode4], '512M', [(RAMDISK_ADDRESS, card_image())])
    c.failed_with('THERMAL')
    word = int(c.failure['value'], 16)
    c.require(word >> 31 == 1 and word >> 30 & 1, 'the stop record %#x does not mark a channel lost for lack of a reading' % word)
    # A failed clock provider ends the thermal owner's liveness: the record names the term (bit 2) and not a reading.
    c = run_case('storage-write-clocks-failed', storage_write_elf, scenario(usb_ready_ms=usb_delay_ms, fault=12, limit_ms=8000),
                 [blob, mode4], '512M', [(RAMDISK_ADDRESS, card_image())])
    c.failed_with('THERMAL')
    word = int(c.failure['value'], 16)
    c.require(word >> 29 == 1 and word >> 2 & 1, 'the stop record %#x does not name the failed clock provider' % word)
    # A failed open leaves its stage as the first failure record, so a later thermal or watchdog stop cannot hide it.
    c = run_case('storage-write-open-fails', storage_write_elf, scenario(usb_ready_ms=usb_delay_ms, fault=13), [blob, mode4], '512M',
                 [(RAMDISK_ADDRESS, card_image())])
    c.require(c.failure is not None and c.failure['group'] == 'APPLICATION', 'a failed SD open left no application failure record')
    word = int(c.failure['value'], 16)
    c.require(word & 0xff == 7 and (word >> 8 & 0xffff) == 0x0903, 'the record %#x does not carry stage 7 and its detail' % word)
    # The read-back modes open the card for reading and verify no bounds: a card whose backup sectors have changed (every session changes them) opens.
    c = run_case('storage-write-open-read-backup-changed', storage_write_elf, scenario(usb_ready_ms=usb_delay_ms), [blob, storage_host.mode_input(6)], '512M',
                 [(RAMDISK_ADDRESS, card_image(corrupt=32770))])
    c.clean()
    d = decode_result(c)
    c.require(d['status'] == 0 and d['au_sectors'] == 8192, 'the read-only open did not succeed on a card with changed backup sectors: status %d' % d['status'])
    c = run_case('storage-write-enable-backup-changed', storage_write_elf, scenario(usb_ready_ms=usb_delay_ms), [blob, mode4], '512M',
                 [(RAMDISK_ADDRESS, card_image(corrupt=32770))])
    c.clean()
    d = decode_result(c)
    c.require(d['status'] == 1 and d['diag0'] == 23 and d['diag3'] == 395,
              'a changed backup sector was not refused at the verify stage: status %d, diagnostic %s' % (d['status'], [d['diag%d' % i] for i in range(4)]))

    elapsed = time.monotonic() - started
    summary = {'large_region_switch_cost': switch_cost, 'passed': True, 'physical_access': False, 'seconds': round(elapsed, 2), 'build_seconds': round(built, 2),
               'qemu_sha256': hashlib.sha256(QEMU.read_bytes()).hexdigest(),
               'images': {p.parent.name: hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in (os_elf, control_elf, latency_elf, el0_elf, partitioned_elf,
                                    partitioned_latency_elf, prestart_elf, facts_elf)},
               'cases': [{'name': r.name, 'status': r.status, 'wall_seconds': round(r.seconds, 3),
                          'failure': r.failure and r.failure['line'], 'main': r.main and r.main['line'],
                          'finished': r.finished and r.finished['line'][:80]} for r in results]}
    (out / 'result.json').write_text(json.dumps(summary, indent=2) + '\n')
    print('PASS qemu-os-boot: %d cases in %.1f s (build %.1f s)' % (len(results), elapsed, built))
    for r in results:
        print('  %-32s status %d  %.2f s' % (r.name, r.status, r.seconds))


if __name__ == '__main__':
    main()
