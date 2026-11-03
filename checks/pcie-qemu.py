#!/usr/bin/env python3
"""Run the PCIe provider, MSI and NVMe C models as AArch64 instructions in QEMU.

The same assertions and register/DMA/interrupt models run on the host under
sanitizers in a733-pcie-trace.py. QEMU supplies the CPU and RAM; it does not
supply an A733 PCIe controller or establish physical operation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from harness import CACHE, CLANG, LLD, environment

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--output', '{output}']]
UNITS = ['drivers/pci/a733_pcie.c', 'drivers/pci/a733_pcie_phy.c',
         'drivers/power/a733_pmic.c', 'drivers/power/a733_rail_leases.c',
         'drivers/clock/a733_clock_leases.c', 'drivers/pinctrl/a733_pins.c',
         'drivers/pinctrl/a733_pin_tables.c', 'drivers/gpu/a733_gpu_resources.c',
         'runtime/device_memory_pool.c', 'drivers/pci/a733_pcie_epf_test.c',
         'drivers/pci/a733_pcie_function.c', 'drivers/pci/a733_pcie_nvme.c',
         'arch/aarch64/device_memory.c']
SUITES = {'core': ['core'],
          'epf': ['gen3', 'gen3-fallback', 'gen3-retrain', 'link-loss', 'missing-msi', 'guard-error'],
          'nvme': ['gen3', 'gen3-poll', 'gen3-intx', 'gen3-fallback', 'suspend', 'suspend-keep',
                   'timeout', 'extra-vector', 'shutdown-stuck', 'stop-link-stuck']}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--suite', action='append', choices=tuple(SUITES), help='Run only an affected or unexecuted suite')
    p.add_argument('--qemu', type=Path, default=CACHE / 'host/usr/bin/qemu-system-aarch64')
    p.add_argument('--newlib', type=Path, default=CACHE / 'host/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/aarch64-none-elf')
    a = p.parse_args()
    out = a.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    commands = []

    def run(label, command):
        command = list(map(str, command))
        r = subprocess.run(command, capture_output=True, text=True, timeout=60, env=environment())
        (out / (label + '.log')).write_text(r.stdout + r.stderr)
        commands.append(dict(label=label, command=command, exit=r.returncode))
        (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        assert r.returncode == 0, (label, r.stdout[-2000:], r.stderr[-2000:])
        return r.stdout + r.stderr

    # Host mmap is replaced only by admission of QEMU's already present RAM.
    shim = out / 'sys'; shim.mkdir()
    (shim / 'mman.h').write_text('''#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 1
#define MAP_ANONYMOUS 2
#define MAP_FIXED_NOREPLACE 4
#define MAP_FAILED ((void *)-1)
static inline void *mmap(void *a, size_t n, int p, int f, int fd, long off) {
 assert((uintptr_t)a == 0x41c00000 && n == 2*1024*1024 && p == 3 && f == 7 && fd == -1 && !off);
 return a;
}
''')
    (out / 'boot.S').write_text('''.section .text.boot,"ax"
.global _start
_start:
 ldr x0, =0x47fe0000
 mov sp, x0
 mov x0, #(3 << 20)
 msr cpacr_el1, x0
 isb
 ldr x0, =__bss_start
 ldr x1, =__bss_end
1: cmp x0, x1
 b.hs 2f
 str xzr, [x0], #8
 b 1b
2: bl qemu_main
3: wfe
 b 3b
''')
    # Reuse the native aperture predicate without the RTOS/MMIO callbacks.
    source = (ROOT / 'drivers/pci/a733_pcie_native.c').read_text()
    start = source.index('int a733_pcie_native_address(')
    end = source.index('\nstatic int allowed(', start)
    (out / 'address.c').write_text('#include "a733_pcie_native.h"\n' + source[start:end])
    flags = ['--target=aarch64-none-elf', '-std=gnu11', '-march=armv8-a', '-mstrict-align', '-O2', '-g',
             '-ffreestanding', '-fno-builtin', '-ffunction-sections', '-fdata-sections',
             '-DDEVICE_MEMORY_POOL_SINGLE_THREAD=1',
             '-fno-stack-protector', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
             '-I' + str(out), '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'checks'),
             '-idirafter', str(a.newlib / 'include')]
    common = []
    for i, source in enumerate([ROOT / u for u in UNITS] + [out / 'address.c', out / 'boot.S']):
        target = out / f'common-{i}.o'
        run(f'compile-{i}', [CLANG, *flags, '-c', source, '-o', target]); common.append(target)
    results = {}
    for suite, cases in SUITES.items():
        if a.suite and suite not in a.suite:
            continue
        filename = 'a733-pcie' + ('' if suite == 'core' else '-' + suite) + '.c'
        source = (ROOT / 'checks' / filename).read_text()
        assert source.count('int main(int argc,char **argv)') == 1
        (out / 'model.c').write_text(source.replace('int main(int argc,char **argv)', 'int model_main(int argc,char **argv)'))
        modes = ','.join(json.dumps(c) for c in cases)
        (out / 'entry.c').write_text(f'''#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
extern int model_main(int, char **);
extern void initialise_monitor_handles(void);
void _init(void) {{}}
void _fini(void) {{}}
static uint64_t table[512] __attribute__((aligned(4096)));
void qemu_main(void) {{
 /* Newlib uses unaligned Normal-memory accesses. MMU-off RAM is Device. */
 table[0] = (UINT64_C(1)<<54) | (UINT64_C(1)<<53) | (1<<10) | 1;
 table[1] = UINT64_C(0x40000000) | (1<<10) | (3<<8) | (1<<2) | 1;
 uint64_t mair=0xff00, tcr=25 | (1<<8) | (1<<10) | (3<<12) | (1<<23);
 __asm__ volatile("dsb sy; msr mair_el1,%0; msr tcr_el1,%1; msr ttbr0_el1,%2; isb; tlbi vmalle1; dsb sy; isb" :: "r"(mair),"r"(tcr),"r"(table) : "memory");
 uint64_t sctlr;
 __asm__ volatile("mrs %0,sctlr_el1" : "=r"(sctlr));
 sctlr |= 1 | (1<<2) | (1<<12);
 __asm__ volatile("msr sctlr_el1,%0; isb" :: "r"(sctlr) : "memory");
 initialise_monitor_handles();
 static char *modes[] = {{{modes}}};
 unsigned i = *(volatile uint32_t *)0x47fff000;
 assert(i < sizeof(modes)/sizeof(modes[0]));
 char *argv[] = {{"pcie-model", modes[i], NULL}};
 int result = model_main({1 if suite == 'core' else 2}, argv);
 puts("PASS QEMU PCIe assertions");
 exit(result);
}}
''')
        objects = []
        for filename in ('model', 'entry'):
            target = out / (suite + '-' + filename + '.o')
            run(suite + '-compile-' + filename, [CLANG, *flags, '-c', out / (filename + '.c'), '-o', target]); objects.append(target)
        image = out / (suite + '.elf')
        run(suite + '-link', [LLD, '-static', '--gc-sections', '-T', ROOT / 'platform/qemu-virt/link.ld',
                             '--defsym=end=__bss_end',
                             *common, *objects, '--start-group', a.newlib / 'lib/libc.a',
                             a.newlib / 'lib/librdimon.a',
                             a.newlib.parent / 'lib/gcc/aarch64-none-elf/14.3.1/libgcc.a',
                             '--end-group', '-o', image])
        for index, case in enumerate(cases):
            label = suite + '-' + case
            log = run(label, [a.qemu, '-machine', 'virt,gic-version=3,secure=off,virtualization=off',
                              '-icount', 'shift=0,sleep=off', '-cpu', 'cortex-a55', '-smp', '1', '-m', '128M', '-nographic', '-monitor', 'none',
                              '-semihosting-config', 'enable=on,target=native', '-kernel', image,
                              '-device', f'loader,addr=0x47fff000,data={index},data-len=4'])
            assert 'PASS QEMU PCIe assertions' in log, label
            results[label] = dict(passed=True, image_sha256=hashlib.sha256(image.read_bytes()).hexdigest())
            (out / 'results.json').write_text(json.dumps(dict(physical_access=False, cases=results), indent=2) + '\n')
            print(label, 'PASS', flush=True)


if __name__ == '__main__':
    main()
