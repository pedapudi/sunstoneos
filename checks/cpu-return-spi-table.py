#!/usr/bin/env python3
"""Execute ARM SPI return snapshots sized by the generated image route table.

GIC effects are modeled. The generated declarations and the monitor PTE loop
are compiled from current source; this does not qualify whole-boot cache or
physical security behavior.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shlex
import subprocess
from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP
from harness import Image, machine, save, environment

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--build', '{build:headset}', '--output', '{output}']]
CHECK_INPUTS = ['apps/os/cpu-return-spi.c', 'apps/os/cpu-return.h', 'tools/sunstone-manifest.py',
                'checks/cpu-return-spi-table.py', 'platform/a733-smp-cached/irq_table.h',
                'platform/a733-smp-cached/irq_table.c', 'platform/a733-smp-cached/memory.c',
                'platform/a733-smp-cached/link.ld']
GIC, MONITOR, TABLES = 0x03400000, 0x42000000, 0x421f0000


def compile_image(args, profile, mutant=None):
    where = args.output/(profile + ('-' + mutant if mutant else ''))
    where.mkdir()
    spec = importlib.util.spec_from_file_location('spi_manifest', args.tree/'tools/sunstone-manifest.py')
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    tables = generator.c_tables(None)
    begin = tables.index('const struct a733_irq_route a733_irq_routes[]')
    end = tables.index('\n', tables.index('const unsigned os_cpu_return_spi_capacity=', begin))
    declaration = tables[begin:end]
    memory = (args.tree/'platform/a733-smp-cached/memory.c').read_text()
    start = memory.index('    for (unsigned i = 0; i < 512; ++i) {\n        uintptr_t a = 0x42000000')
    end_mapping = memory.index(';', memory.index('    a733_cached_el1_l2[16] = 0;', start))+1
    mapping = memory[start:end_mapping]
    handlers = sorted(set(re.findall(r',\s*([A-Za-z_]\w*)\s*}', declaration)))
    fixture = '#include "os.h"\n#include "irq_table.h"\n#include "cpu-return-runtime.h"\n'
    fixture += '\n'.join('void '+name+'(void){}' for name in handlers) + '\n' + declaration + '\n'
    fixture += '''
#define MONITOR_DATA __attribute__((section(".bss.a733_smp_monitor")))
volatile struct qualification_report a733_qualification_report MONITOR_DATA;
const unsigned fixture_snapshot_bytes=sizeof(struct os_cpu_return_spi_snapshot);
uint64_t fixture_time MONITOR_DATA;
uint64_t a733_smp_counter(void){return ++fixture_time;}
#define TABLE __attribute__((section(".bss.a733_cached_tables"),aligned(4096)))
uint64_t a733_cached_el3_l2[512] TABLE,a733_cached_el3_l3[512] TABLE;
uint64_t a733_cached_el1_l1[512] TABLE,a733_cached_el1_l2[512] TABLE;
extern unsigned char __tables_start[],__tables_end[];
#define BIT(n) (UINT64_C(1)<<(n))
void fixture_monitor_mapping(void){
''' + mapping + '\n}\n'
    (where/'fixture.c').write_text(fixture)
    command = next(row for row in json.loads((args.build/'compile_commands.json').read_text())
                   if row['file'].endswith('/apps/os/cpu-return-spi.c'))
    flags = list(command.get('arguments') or shlex.split(command['command']))
    compiler, flags = flags[0], flags[1:]
    # Keep the selected native ABI, dependency roots and generated runtime
    # header. Replace only the input/output and the selected route features.
    for option in ('-o', '-c'):
        index = flags.index(option)
        del flags[index:index+2]
    flags += ['-I'+str(args.tree/'apps/os'), '-I'+str(args.tree/'platform/a733-smp-cached'),
              '-USUNSTONE_A733_HW_FACTS', '-USUNSTONE_A733_OS_SD_WRITE',
              '-USUNSTONE_A733_OS_GPU', '-USUNSTONE_BOARD_USB2_IRQ',
              '-DSUNSTONE_A733_OS_HEADSET=1', '-DSUNSTONE_A733_OS_HEADSET_IRQ=1']
    if 'gpu' in profile:
        flags += ['-DSUNSTONE_A733_OS_GPU=1']
    if 'usb' in profile:
        flags += ['-DSUNSTONE_BOARD_USB2_IRQ=1']
    source = args.tree/'apps/os/cpu-return-spi.c'
    if mutant:
        text = source.read_text()
        if mutant == 'hardcap-four':
            old = 'a733_irq_route_count>os_cpu_return_spi_capacity'
            assert text.count(old) == 1
            text = text.replace(old, 'a733_irq_route_count>4u')
        elif mutant == 'stale-resave':
            assert text.count('saved_count=0;') == 1
            text = text.replace('saved_count=0;', '')
        source = where/'cpu-return-spi-mutant.c'
        source.write_text(text)
    objects = []
    for index, unit in enumerate((source, args.tree/'platform/a733-smp-cached/irq_table.c', where/'fixture.c')):
        obj = where/(str(index)+'.o')
        subprocess.run([compiler, *flags, '-c', str(unit), '-o', str(obj)], check=True, env=environment())
        objects.append(obj)
    script = where/'link.ld'
    script.write_text('''ENTRY(os_cpu_return_spi_save)
SECTIONS {
 . = 0x41000000; .text : { *(.text*) }
 . = ALIGN(4096); .rodata : { *(.rodata*) }
 .monitor_bss 0x42005000 (NOLOAD) : ALIGN(128) { *(.bss.a733_smp_monitor) }
 .tables 0x421f0000 (NOLOAD) : ALIGN(4096) { __tables_start = .; *(.bss.a733_cached_tables) __tables_end = .; }
 .bss 0x41200000 (NOLOAD) : { *(.bss*) *(COMMON) }
 /DISCARD/ : { *(.comment) *(.note*) }
}
''')
    output = where/'fixture.elf'
    linker = re.search(r'^CMAKE_LINKER:FILEPATH=(.+)$', (args.build/'CMakeCache.txt').read_text(), re.M)
    assert linker, 'The native build must declare its configured linker'
    subprocess.run([linker[1], '-T', str(script), '-o', str(output), *map(str, objects)],
                   check=True, env=environment())
    return output, hashlib.sha256(mapping.encode()).hexdigest(), hashlib.sha256(declaration.encode()).hexdigest()


def readonly_word(elf, symbols, name):
    symbol = symbols[name]
    section = elf.get_section(symbol['st_shndx'])
    offset = symbol['st_value']-section['sh_addr']
    return int.from_bytes(section.data()[offset:offset+4], 'little')


def run(path, profile, case):
    image = Image(path, {'qualification_report', 'os_cpu_return_spi_snapshot'})
    with path.open('rb') as stream:
        elf = ELFFile(stream)
        symbols = {symbol.name: symbol for symbol in elf.get_section_by_name('.symtab').iter_symbols()}
        expected = readonly_word(elf, symbols, 'a733_irq_route_count')
        stride = readonly_word(elf, symbols, 'fixture_snapshot_bytes')
        snapshot = symbols['os_cpu_return_spi_saved']
        section = elf.get_section(snapshot['st_shndx'])
        assert snapshot['st_size'] == expected*stride and snapshot['st_value'] % 128 == 0
        assert section['sh_type'] == 'SHT_NOBITS' and section['sh_flags'] & 3 == 3
        assert MONITOR <= snapshot['st_value'] < snapshot['st_value']+snapshot['st_size'] < TABLES
        assert not elf.get_section(symbols['os_cpu_return_spi_capacity']['st_shndx'])['sh_flags'] & 1
    u = machine()
    u.mem_map(0x41000000, 0x1400000)
    u.mem_map(GIC, 0x10000)
    for at, data in image.parts:
        if data:
            u.mem_write(at, data)
    def put(at, value, width=4):
        u.mem_write(at, int(value).to_bytes(width, 'little'))
    def get(at, width=4):
        return int.from_bytes(u.mem_read(at, width), 'little')
    count = image.address('a733_irq_route_count')
    capacity = image.address('os_cpu_return_spi_capacity')
    assert get(count) == get(capacity) == expected
    ids = [get(image.address('a733_irq_routes')+16*i, 2) for i in range(expected)]
    assert {35,71,97,143} <= set(ids)
    if 'gpu' in profile:
        assert expected >= 5 and 95 in ids
    if 'usb' in profile:
        assert 187 in ids
    original = {}
    for ident in ids:
        for off in (0x80+ident//32*4, 0xd00+ident//32*4, 0xc00+ident//16*4, 0x400+ident//4*4):
            original[GIC+off] = 0xa5a50000 | off
        original[GIC+0x6000+ident*8] = 0x1234567800000000 | ident<<8
    for address, value in original.items():
        put(address, value, 8 if address >= GIC+0x6000 else 4)
    report = image.address('a733_qualification_report')
    put(report+image.layouts['qualification_report']['deadline'], 100000, 8)
    state = {'post_clean': False, 'writes': [], 'gic_reads': 0, 'fail_readback': False}
    def invoke(name):
        u.reg_write(UC_ARM64_REG_SP, 0x420e0000)
        u.reg_write(UC_ARM64_REG_LR, 0x423ff000)
        u.emu_start(image.address(name), 0x423ff000, count=1000000)
        assert u.reg_read(UC_ARM64_REG_PC) == 0x423ff000, (case, 'instruction budget')
        return u.reg_read(UC_ARM64_REG_X0)
    invoke('fixture_monitor_mapping')
    def device(address):
        if not MONITOR <= address < TABLES:
            return False
        pte = get(image.address('a733_cached_el3_l3')+(address-MONITOR)//4096*8, 8)
        return pte & 3 == 3 and pte & ((7<<2)|(1<<5)) == 0
    assert all(device(address) for address in (snapshot['st_value'], snapshot['st_value']+snapshot['st_size']-1,
                                               image.address('saved_count'), 0x420dfff0))
    assert not get(image.address('a733_cached_el1_l2')+16*8, 8)
    def write(uc, access, address, size, value, unused):
        if state['post_clean']:
            assert GIC <= address < GIC+0x10000 or device(address), ('post-clean Normal write', case, hex(address))
        if GIC <= address < GIC+0x10000:
            state['writes'].append((address, size, value))
            if 0x280 <= address-GIC < 0x300:
                target = address-0x80
                put(target, get(target)&~value)
    def read(uc, access, address, size, value, unused):
        if GIC <= address < GIC+0x10000:
            state['gic_reads'] += 1
            if state['fail_readback'] and address == GIC+0x80+ids[0]//32*4:
                put(address, get(address)^1)
    u.hook_add(UC_HOOK_MEM_WRITE, write)
    u.hook_add(UC_HOOK_MEM_READ, read)
    last = ids[-1]
    if case in ('save-enabled', 'save-pending', 'save-active'):
        base = {'save-enabled':0x100, 'save-pending':0x200, 'save-active':0x300}[case]
        put(GIC+base+last//32*4, 1<<(last%32))
    invoke('os_cpu_return_spi_save')
    saved_count = image.address('saved_count')
    if case.startswith('save-'):
        assert get(saved_count) == 0 and not invoke('os_cpu_return_spi_restore'), (case, 'failed initial save accepted')
        return {'case':case, 'routes':expected, 'refused':True}
    assert get(saved_count) == expected, (case, 'declaration-sized save refused', expected)
    fields = image.layouts['os_cpu_return_spi_snapshot']
    for index, ident in enumerate(ids):
        at = snapshot['st_value']+index*stride
        assert get(at+fields['intid']) == ident and get(at+fields['valid']) == 1
        assert get(at+fields['route'], 8) == original[GIC+0x6000+ident*8]
    if case in ('resave-overflow', 'resave-table', 'resave-enabled'):
        if case == 'resave-overflow':
            put(count, expected+1)
        elif case == 'resave-table':
            put(image.address('a733_irq_routes')+16, ids[0], 2)
        else:
            put(GIC+0x100+last//32*4, 1<<(last%32))
        invoke('os_cpu_return_spi_save')
        if case == 'resave-overflow':
            put(count, expected)
        if case == 'resave-enabled':
            assert all(not get(snapshot['st_value']+i*stride+fields['valid']) for i in range(expected))
        restored = invoke('os_cpu_return_spi_restore')
        assert get(saved_count) == 0 and not restored, (case, 'old save survived refused re-save', restored)
        return {'case':case, 'routes':expected, 'refused':True}
    for address in original:
        put(address, 0, 8 if address >= GIC+0x6000 else 4)
    state['post_clean'] = True
    if case == 'restore-control':
        put(GIC, 1<<31)
    elif case == 'restore-enabled':
        put(GIC+0x100+last//32*4, 1<<(last%32))
    elif case == 'restore-active-deadline':
        put(GIC+0x300+last//32*4, 1<<(last%32))
        put(report+image.layouts['qualification_report']['deadline'], 1, 8)
    elif case == 'restore-readback':
        state['fail_readback'] = True
    result = invoke('os_cpu_return_spi_restore')
    assert bool(result) == (case == 'success'), (case, result)
    if result:
        assert state['writes']
        for address, value in original.items():
            assert get(address, 8 if address >= GIC+0x6000 else 4) == value
    return {'case':case, 'routes':expected, 'restored':bool(result), 'device_writes':len(state['writes'])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--tree', type=Path, default=ROOT)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    rows = []
    mapping_hash = table_hash = None
    cases = ('success', 'save-enabled', 'save-pending', 'save-active', 'resave-overflow',
             'resave-table', 'resave-enabled', 'restore-control', 'restore-enabled',
             'restore-active-deadline', 'restore-readback')
    for profile in ('headset', 'gpu-headset', 'usb-gpu-headset'):
        image, mapping_hash, table_hash = compile_image(args, profile)
        rows.extend(run(image, profile, case) for case in cases)
    rejected = []
    for name, case in (('hardcap-four', 'success'), ('stale-resave', 'resave-table')):
        image, _, _ = compile_image(args, 'gpu-headset', name)
        try:
            run(image, 'gpu-headset', case)
        except AssertionError as error:
            diagnostic = 'declaration-sized save refused' if name == 'hardcap-four' else 'old save survived refused re-save'
            assert diagnostic in str(error), (name, 'wrong mutant failure', str(error))
            rejected.append({'mutation':name, 'failure':str(error)})
        else:
            raise AssertionError(('regression mutation passed', name))
    save(args.output/'results.json', {'physical_access':False, 'cases':rows, 'negative_controls':rejected,
         'monitor_mapping_block_sha256':mapping_hash, 'generated_table_block_sha256':table_hash,
         'scope':'ARM snapshot/table/monitor-PTE code with modeled GIC effects; no wholeboot or hardware qualification.'})
    print('PASS declaration-sized SPI return:', len(rows), 'ARM cases;', len(rejected), 'mutations rejected')


if __name__ == '__main__':
    main()
