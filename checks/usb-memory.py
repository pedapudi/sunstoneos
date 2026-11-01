#!/usr/bin/env python3
"""Run actual AArch64 USB memory table construction and cold checksum offline."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'checks'))
import harness  # Assertion guard and ELF reader.

spec = importlib.util.spec_from_file_location('table_walk_fixture', ROOT/'checks/cpu-table-walks.py')
walk_fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(walk_fixture)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    steps = []

    def run(label, command):
        command = list(map(str, command))
        result = subprocess.run(command, text=True, capture_output=True)
        (out/(label+'.log')).write_text(result.stdout+result.stderr)
        steps.append(dict(label=label, command=command, exit=result.returncode))
        (out/'steps.json').write_text(json.dumps(steps, indent=2)+'\n')
        assert result.returncode == 0, (label, result.stderr)

    clang = '/usr/bin/clang-21'
    common = [clang, '-std=c11', '-g', '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT/'include'), '-DSUNSTONE_USB_HOST_GENERIC=1']
    run('pool-build', common+['-O1', *harness.SANITIZE, '-fno-pie', '-no-pie', ROOT/'checks/usb-memory-pool.c',
        ROOT/'platform/a733/memory_uncached.c', ROOT/'runtime/device_memory_pool.c',
        '-Wl,--defsym=__uncached_pool_start=0x47000000', '-Wl,--defsym=__uncached_pool_end=0x48000000',
        '-Wl,--defsym=__device_pool_end=0x41800000', '-o', out/'pool'])
    run('pool', [out/'pool'])
    for fault in (1, 8):
        run('pool-fault-'+str(fault), [out/'pool', str(fault)])
    native = common+['--target=aarch64-none-elf', '-march=armv8-a', '-mgeneral-regs-only', '-mstrict-align',
        '-O2', '-ffreestanding', '-fno-builtin', '-ffunction-sections', '-fdata-sections', '-I'+str(ROOT/'runtime/include')]
    units = ['platform/a733-smp-cached/memory.c', 'platform/a733/cpu_coherence.c', 'platform/a733/cpu_coherence.S',
        'checks/usb-memory-native.c']
    objects = []
    for i, unit in enumerate(units):
        target = out/('native-'+str(i)+'.o')
        run('native-'+str(i), native+['-c', ROOT/unit, '-o', target])
        objects.append(target)
    # The native allocator branch also compiles; the host pool check executes it
    # with modeled AT results and the real allocator.
    run('native-pool', native+['-DDEVICE_MEMORY_POOL_SINGLE_THREAD=1', '-c', ROOT/'platform/a733/memory_uncached.c', '-o', out/'native-pool.o'])
    ld = Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld'
    image_path = out/'memory.elf'
    run('link', [ld, '-T', ROOT/'checks/usb-memory.ld', '--gc-sections', '--undefined=a733_cpu_coherence_check_el1',
        *objects, '-o', image_path])
    image = harness.Image(image_path)

    def machine(admitted=1, decode=1):
        m = walk_fixture.Machine(image)
        m.uc.mem_write(image.address('fixture_admitted'), admitted.to_bytes(4, 'little'))
        m.uc.mem_write(image.address('fixture_decode'), decode.to_bytes(4, 'little'))
        return m

    def walk(m, address, el):
        table = image.address('a733_cached_el'+str(el)+'_l1')
        for level, shift in ((1, 30), (2, 21), (3, 12)):
            entry = m.read(table+((address >> shift) & 511)*8)
            if not entry & 1:
                return None
            if level == 3 or entry & 3 == 1:
                mask = (1 << shift)-1
                physical = (entry & 0x0000fffffffff000 & ~mask)+(address & mask)
                return physical, (entry >> 6) & 3, (entry >> 2) & 7, (entry >> 8) & 3, entry
            table = entry & 0x0000fffffffff000
        raise AssertionError('invalid walk')

    refused = machine(admitted=0)
    assert refused.call('a733_cached_memory_prepare') == 0
    assert not any(t[0] == 'write' for t in refused.trace), 'Refusal must precede table writes'
    cases = []
    for decode in (0, 1):
        m = machine(decode=decode)
        assert m.call('a733_cached_memory_prepare') == 1
        for address in range(0x47000000, 0x48000000, 4096):
            for el in (1, 3):
                translated = walk(m, address, el)
                assert translated and translated[:4] == (address, 0 if el == 1 else 1, 2, 3)
                assert translated[4] >> 54 & 1
                if el == 1:
                    assert translated[4] >> 53 & 1
                else:
                    assert translated[4] >> 5 & 1  # EL3 Nonsecure output.
        for address in range(0x7a000000, 0x7a600000, 4096):
            assert walk(m, address, 3) is None
            translated = walk(m, address, 1)
            if 0x7a040000 <= address < 0x7a440000:
                assert translated and translated[:4] == (address-0x33000000, 3, 2, 3)
                assert translated[4] >> 53 & 1 and translated[4] >> 54 & 1
                # Page offsets must preserve identity to the same physical byte.
                assert walk(m, address+4095, 1)[0] == address-0x33000000+4095
            else:
                assert translated is None
        alias_tables = image.address('a733_cached_usb_el1_l3')
        for table in range(3):
            assert walk(m, alias_tables+table*4096, 1) is None  # Tables inaccessible at EL1/EL0.
            mapped = walk(m, alias_tables+table*4096, 3)
            assert mapped and mapped[2:4] == (2, 3) and mapped[4] >> 5 & 1
        root = image.address('a733_cached_el1_l1')
        # Every core runs the actual assembly cold check. System registers and
        # DC instructions are modeled; table loads, sums and branches execute.
        for core in range(8):
            m.el1()
            m.model['mpidr_el1'] = 0x81000000 | core << 8
            assert m.call('a733_cpu_coherence_check_el1', root, 1000000) == 1
            flushed = [t[2] for t in m.trace if t[0] == 'dc' and t[1] == 'civac']
            expected = set()
            for base in [root, image.address('a733_cached_el1_l2'), *[alias_tables+i*4096 for i in range(3)]]:
                expected.update(range(base, base+4096, 64))
            assert len(flushed) == 320 and set(flushed) == expected
        # Each added page is bound by the sum, including an invalid guard PTE.
        for table in range(3):
            address = alias_tables+table*4096
            previous = m.read(address)
            m.write(address, previous ^ 0x1000)
            assert m.call('a733_cpu_coherence_check_el1', root, 1000000) == 0xffffffff
            m.write(address, previous)
        cases.append(dict(dram_decode=decode, physical_pages_checked=4096, alias_pages_checked=1536,
            shared_pages=1024, cores_checked=8, maintenance_lines_per_core=320, corrupt_l3_pages_refused=3))
    result = {'result': 'PASS', 'hardware_access': False, 'cases': cases,
        'image_sha256': hashlib.sha256(image_path.read_bytes()).hexdigest(),
        'table_bytes': image.address('__tables_end')-image.address('__tables_start'),
        'scope': 'Actual AArch64 table builder and per-core cold checksum; software page walks verify physical identity, AP and NC attributes. Registers, cache effects and ownership evidence are modeled; no physical MMU, DMA or board qualification.'}
    inputs = [ROOT/p for p in units]+[ROOT/'platform/a733/memory_uncached.c', ROOT/'include/a733_usb_memory.h',
        ROOT/'include/a733_cpu_coherence.h', ROOT/'checks/usb-memory.py', ROOT/'checks/usb-memory.ld',
        ROOT/'checks/usb-memory-pool.c']
    result['inputs'] = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    (out/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({k: result[k] for k in ('result', 'cases', 'table_bytes')}, indent=2))


if __name__ == '__main__':
    main()
