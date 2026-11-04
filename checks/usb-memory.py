#!/usr/bin/env python3
"""Run actual AArch64 USB memory table construction and cold checksum offline."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shlex
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
    parser.add_argument('--native-flags', type=Path, help='OS USB-class flags.make for session geometry check')
    parser.add_argument('--resume', action='store_true', help='resume only failed or unexecuted commands')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=args.resume)
    steps_path=out/'steps.json'
    steps = json.loads(steps_path.read_text()) if args.resume and steps_path.exists() else []

    def run(label, command):
        command = list(map(str, command))
        previous=[step for step in steps if step['label']==label]
        if previous and previous[-1]['exit']==0:
            assert previous[-1]['command']==command, 'passed command changed: '+label
            return
        result = subprocess.run(command, text=True, capture_output=True, env=harness.environment())
        log=label+('-attempt'+str(len(previous)+1) if previous else '')+'.log'
        (out/log).write_text(result.stdout+result.stderr)
        steps.append(dict(label=label, command=command, exit=result.returncode))
        (out/'steps.json').write_text(json.dumps(steps, indent=2)+'\n')
        assert result.returncode == 0, (label, result.stderr)

    clang = harness.CLANG
    common = [clang, '-std=c11', '-g', '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT/'include'), '-DSUNSTONE_USB_HOST_GENERIC=1']
    run('pool-build', common+['-O1', *harness.SANITIZE, '-fno-pie', '-no-pie', ROOT/'checks/usb-memory-pool.c',
        ROOT/'platform/a733/memory_uncached.c', ROOT/'runtime/device_memory_pool.c',
        '-Wl,--defsym=__uncached_pool_start=0x42200000', '-Wl,--defsym=__uncached_pool_end=0x42a00000',
        '-Wl,--defsym=__device_pool_end=0x41800000', '-o', out/'pool'])
    run('pool', [out/'pool'])
    if args.native_flags:
        flags=args.native_flags.read_text()
        def flag_value(name):
            return shlex.split(next(line.split('=',1)[1] for line in flags.splitlines() if line.startswith(name+' =')))
        selected=flag_value('C_DEFINES')+flag_value('C_INCLUDES')+flag_value('C_FLAGS')
        selected=[value for value in selected if not value.startswith(('-march=','-mstrict-align','-mgeneral-regs-only'))]
        run('session-build',[clang,*selected,'-O1',*harness.SANITIZE,'-ffunction-sections','-fdata-sections',
            ROOT/'checks/usb-memory-session.c','-Wl,--gc-sections','-o',out/'session'])
        run('session',[out/'session'])
    for fault in (1, 4):
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
    ld = harness.LLD
    image_path = out/'memory.elf'
    run('link', [ld, '-T', ROOT/'checks/usb-memory.ld', '--gc-sections', '--undefined=a733_cpu_coherence_check_el1',
        *objects, '-o', image_path])
    image = harness.Image(image_path)
    # Link the same allocator reservation as the OS: cached base, storage
    # session patch, then USB arena patch. Inspect allocated ELF bytes instead
    # of accepting symbols supplied by the table-only fixture.
    run('storage-link-patch', ['patch', '--fuzz=0', '-o', out/'storage.ld',
        ROOT/'platform/a733-smp-cached/link.ld', ROOT/'platform/a733-storage-maintenance/link.patch'])
    run('usb-link-patch', ['patch', '--fuzz=0', '-o', out/'usb.ld',
        out/'storage.ld', ROOT/'apps/os/usb-memory-link.patch'])
    run('production-link', [ld, '-T', out/'usb.ld', '--entry=a733_cached_memory_prepare',
        '--gc-sections', *objects, '-o', out/'production.elf'])
    # A pool sized from the declaration (22 MiB below the monitor interval) links with the same arena and sections, and both
    # pools lie in the write-back windows that the walks below prove (the application window and the legacy pool).
    run('production-link-sized', [ld, '-T', out/'usb.ld', '--entry=a733_cached_memory_prepare',
        '--gc-sections', *objects, '-o', out/'production-sized.elf'])
    from elftools.elf.elffile import ELFFile
    for name in ('production.elf', 'production-sized.elf'):
        with (out/name).open('rb') as stream:
            sized_arena = ELFFile(stream).get_section_by_name('.uncached_pool')
            assert (sized_arena['sh_addr'], sized_arena['sh_size']) == (0x42200000, 0x800000), name
    wb_windows = ((0x44000000, 0x47000000), (0x47400000, 0x47c00000))
    for pool in ((0x47400000, 0x47c00000), (0x45a00000, 0x47000000)):
        assert any(lo <= pool[0] and pool[1] <= hi for lo, hi in wb_windows), pool
    with (out/'production.elf').open('rb') as stream:
        elf=ELFFile(stream)
        arena=elf.get_section_by_name('.uncached_pool')
        assert (arena['sh_addr'], arena['sh_size'], arena['sh_type']) == (0x42200000, 0x800000, 'SHT_NOBITS')
        for section in elf.iter_sections():
            if section.name != '.uncached_pool' and section['sh_flags'] & 2 and section['sh_size']:
                assert section['sh_addr']+section['sh_size'] <= 0x42200000 or 0x42a00000 <= section['sh_addr']
        for segment in elf.iter_segments():
            if segment['p_type']=='PT_LOAD' and segment['p_vaddr']<=0x42200000<segment['p_vaddr']+segment['p_memsz']:
                assert segment['p_vaddr']==0x42200000 and segment['p_memsz']==0x800000 and segment['p_filesz']==0
    # Static owners and staging from the production OS and card path. The
    # arena must remain disjoint even when NPU and USB classes coexist.
    for low, high in [(0x41000000,0x42200000),(0x43000000,0x43100000),
                      (0x44000000,0x47000000),
                      (0x47000000,0x47400000),(0x47400000,0x47c00000),
                      (0x48000000,0x79000000)]:
        assert high<=0x42200000 or 0x42a00000<=low

    def machine(decode=1):
        m = walk_fixture.Machine(image)
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

    cases = []
    for decode in (0, 1):
        m = machine(decode=decode)
        assert m.call('a733_cached_memory_prepare') == 1
        for address in range(0x42200000, 0x42a00000, 4096):
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
                assert translated and translated[:4] == (address-0x37e00000, 3, 2, 3)
                assert translated[4] >> 53 & 1 and translated[4] >> 54 & 1
                # Page offsets must preserve identity to the same physical byte.
                assert walk(m, address+4095, 1)[0] == address-0x37e00000+4095
            else:
                assert translated is None
        # Neighboring reservations retain write-back attributes. In
        # particular, no USB NC mapping can reach the NPU pool or staging.
        for low,high in ((0x42a00000,0x43000000),(0x43000000,0x43100000),
                         (0x44000000,0x47000000),(0x47000000,0x47400000),
                         (0x47400000,0x47c00000)):
            for address in range(low,high,4096):
                translated=walk(m,address,1)
                assert translated and translated[0]==address and translated[2:4]==(1,3)
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
        cases.append(dict(dram_decode=decode, physical_pages_checked=2048, alias_pages_checked=1536,
            shared_pages=1024, cores_checked=8, maintenance_lines_per_core=320, corrupt_l3_pages_refused=3))
    result = {'result': 'PASS', 'hardware_access': False, 'cases': cases,
        'session_geometry_checked': bool(args.native_flags),
        'image_sha256': hashlib.sha256(image_path.read_bytes()).hexdigest(),
        'table_bytes': image.address('__tables_end')-image.address('__tables_start'),
        'scope': 'Actual AArch64 table builder and per-core cold checksum; software page walks verify physical identity, AP and NC attributes. Registers and cache effects are modeled; ELF reservation and allocator leases execute; no physical MMU, DMA or board qualification.'}
    inputs = [ROOT/p for p in units]+[ROOT/'platform/a733/memory_uncached.c', ROOT/'include/a733_usb_memory.h',
        ROOT/'include/a733_cpu_coherence.h', ROOT/'checks/usb-memory.py', ROOT/'checks/usb-memory.ld',
        ROOT/'checks/usb-memory-pool.c', ROOT/'apps/os/usb-memory-link.patch',
        ROOT/'apps/os/session.c', ROOT/'include/sunstone_session.h', ROOT/'checks/usb-memory-session.c',
        ROOT/'platform/a733-smp-cached/link.ld', ROOT/'platform/a733-storage-maintenance/link.patch']
    result['inputs'] = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    (out/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({k: result[k] for k in ('result', 'cases', 'table_bytes')}, indent=2))


if __name__ == '__main__':
    main()
