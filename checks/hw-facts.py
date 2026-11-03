#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check the hardware facts table: layout, decoder, facts file and the host models that read it.

No board is used. The check compares the C layout (include/a733_hw_facts.h and the slot table) with the
decoder's description, decodes synthetic tables, checks docs/hardware/a733-facts.json, evaluates the GIC
model against the facts file and against contradicting files, and fails when a host model hard-codes the
GIC priority-mask or priority-view values that the facts file states (docs/hardware/a733-facts.md).
"""
import argparse
import copy
import importlib.util
import json
import re
from pathlib import Path

if not __debug__:
    raise SystemExit('checks require assertions')

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('hw_facts', ROOT / 'tools/hw_facts.py')
hw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hw)

# Host models that stand in for the GIC. Each reads GicModel instead of restating the values.
MODELS = ('checks/a733-smp-cached.py',)
# The literals that encoded the priority mask and the Non-secure views.
FORBIDDEN = (
    (re.compile(r'&\s*0xf8\b'), 'a priority mask literal'),
    (re.compile(r'<<\s*1\)\s*&\s*255'), 'a Non-secure priority shift'),
    (re.compile(r'>>\s*1\)\s*\|\s*128'), 'a Non-secure priority write transform'),
    (re.compile(r'==\s*0xe0\b'), 'a Non-secure priority value'),
)


def scan(text):
    """The first forbidden literal on each line that concerns priorities or ICC_PMR_EL1."""
    found = []
    for number, line in enumerate(text.splitlines(), 1):
        code = line.split('#')[0]
        if not re.search(r'pmr|prior|raw|rpr|mask', code, re.I):
            continue
        for pattern, what in FORBIDDEN:
            if pattern.search(code):
                found.append((number, what, line.strip()))
                break
    return found


def test_layout():
    header = (ROOT / 'include/a733_hw_facts.h').read_text()
    constants = dict(re.findall(r'#define (HWF_\w+) (?:UINT64_C\()?(0x[0-9a-fA-F]+|\d+)u?\)?', header))
    number = lambda name: int(constants[name], 0)
    assert number('HWF_MAGIC') == hw.MAGIC and number('HWF_VERSION') == hw.VERSION
    assert (number('HWF_CORES'), number('HWF_CORE_SLOTS'), number('HWF_SHARED_SLOTS')) == (hw.CORES, hw.CORE_SLOTS, hw.SHARED_SLOTS)
    assert (number('HWF_PLATFORM_A733'), number('HWF_PLATFORM_QEMU_OS')) == (hw.PLATFORM_A733, hw.PLATFORM_QEMU_OS)
    assert (number('HWF_VALID'), number('HWF_SKIPPED'), number('HWF_ANOMALY')) == (hw.FLAG_VALID, hw.FLAG_SKIPPED, hw.FLAG_ANOMALY)
    assert hw.CORE_OFFSET == 64 + 16 * hw.SHARED_SLOTS and hw.TABLE_BYTES == hw.CORE_OFFSET + 16 * hw.CORES * hw.CORE_SLOTS
    assert (ROOT / 'include/a733_hw_facts_table.inc').read_text() == hw.table_inc(), \
        'include/a733_hw_facts_table.inc differs from the catalogue; run tools/hw_facts.py gen'
    manifest = json.loads((ROOT / 'apps/hw-facts/manifest.json').read_text())
    assert manifest['output_limit'] == hw.TABLE_BYTES
    ids = [spec_id for spec_id, _ in hw.CORE_SPECS + hw.SHARED_SPECS]
    assert len(set(hw.CORE_SPECS)) == len(hw.CORE_SPECS) and len(set(hw.SHARED_SPECS)) == len(hw.SHARED_SPECS) and ids
    names = [d['name'] for k in hw.CORE_SPECS + hw.SHARED_SPECS for d in hw.FACTS[k]]
    assert len(names) == len(set(names)), 'a fact name repeats'


def synthetic():
    """A table as the board writes it: every slot read, a few values chosen."""
    values = {}
    for core in range(hw.CORES):
        values[('core', core, (0x0002, 0))] = 0x414fd0b1 if core >= 6 else 0x412fd050
        values[('core', core, (0x0001, 0))] = 0x81000000 | core << 8
        values[('core', core, (0x0030, 0))] = 0x8c00 | 5 << 8
        values[('core', core, (0x0038, 0))] = (0xf8, 0xff, hw.FLAG_VALID)
        values[('core', core, (0x0039, 1))] = (0x98, 0x98, hw.FLAG_VALID)
        values[('core', core, (0x004a, 6))] = 0xd8 << 24 | 0xa0 << 16
    values[('shared', 0, (0x0100, 0))] = 0x12
    values[('core', 0, (0x004a, 0))] = 0xe0
    values[('shared', 0, (0x0110, 97))] = 0x58 | 1 << 8 | 1 << 10
    values[('shared', 0, (0x0111, 97))] = 2 << 8
    values[('shared', 0, (0x0112, 97))] = 1 << 16 | 0xd8
    values[('shared', 0, (0x0130, 0x070101ac))] = 1
    skipped = {('shared', 0, (0x0130, 0x07064000)), ('shared', 0, (0x0130, 0x07064008))}
    return hw.encode(values=values, skipped=skipped)


def test_decoder():
    blob = synthetic()
    header, facts = hw.decode(blob)
    by = {(f['name'], f['core']): f for f in facts}
    assert header['platform'] == 'a733-board' and header['done_mask'] == 0xff
    assert by[('cpu.midr_el1.partnum', 7)]['value'] == 0xd0b and by[('cpu.midr_el1.partnum', 0)]['value'] == 0xd05
    assert by[('cpu.mpidr_el1.aff1', 5)]['value'] == 5
    assert by[('gic.cpuif.icc_ctlr_el1.pribits', 3)]['value'] == 5
    assert by[('gic.cpuif.icc_pmr_el1.readback_after_write_ff', 2)]['written'] == 0xff
    assert by[('gic.cpuif.icc_pmr_el1.readback_after_write_api_mask', 2)]['value'] == 0x98
    assert by[('gic.redist.intid27.priority', 4)]['value'] == hw.ns_view(0x6c) and by[('gic.redist.intid26.priority', 4)]['value'] == hw.ns_view(0x50)
    assert by[('gic.dist.spi97.priority', 0)]['value'] == 0x58 and by[('gic.dist.spi97.priority', 0)]['status'] == 'measured'
    # GICD_CTLR 0x12 read at Non-secure EL1 with DS clear: EnableGrp1A and ARE_NS, and no field of the Secure layout.
    assert by[('gic.dist.gicd_ctlr', 0)]['value'] == 0x12
    assert by[('gic.dist.gicd_ctlr.are_ns', 0)]['value'] == 1 and by[('gic.dist.gicd_ctlr.enable_grp1a', 0)]['value'] == 1
    assert by[('gic.dist.gicd_ctlr.enable_grp1', 0)]['value'] == 0 and by[('gic.dist.gicd_ctlr.rwp', 0)]['value'] == 0
    assert not any(n.startswith('gic.dist.gicd_ctlr.') and n.split('.')[-1] in ('ds', 'are_s', 'enable_grp0', 'enable_grp1s', 'enable_grp1ns') or '.ds1_' in n
                   for n, _ in by), 'a field the Non-secure view does not show'
    # Fields the view cannot see are not measurements: IGROUPR and IGRPMODR always, and an interrupt whose every field
    # reads zero, such as a Secure SPI, SGI 1 or the zero words of the SGI and PPI frame.
    for name in ('gic.dist.spi97.group1', 'gic.dist.spi97.group_modifier', 'gic.redist.sgi_ppi.igroupr0', 'gic.redist.sgi_ppi.igrpmodr0'):
        assert by[(name, 0)]['status'] == 'raz-in-view' and by[(name, 0)]['value'] is None and by[(name, 0)]['note'], name
    for field in ('priority', 'enabled', 'pending', 'active', 'config', 'irouter', 'group1'):
        item = by[('gic.dist.spi71.' + field, 0)]
        assert item['status'] == 'raz-in-view' and item['value'] is None and item['read_value'] == 0, field
    assert by[('gic.redist.intid0.priority', 0)]['status'] == 'measured' and by[('gic.redist.intid0.priority', 0)]['value'] == 0xe0
    assert by[('gic.redist.intid1.priority', 0)]['status'] == 'raz-in-view'
    assert by[('gic.redist.sgi_ppi.isenabler0', 0)]['status'] == 'raz-in-view'
    assert by[('gic.dist.spi97.enabled', 0)]['status'] == 'measured' and by[('gic.dist.spi97.irouter', 0)]['value'] == 2 << 8
    # The virtual board's GIC has DS set: the single-state layout, and every field is visible.
    q_header, q_facts = hw.decode(hw.encode(platform=hw.PLATFORM_QEMU_OS, values={('shared', 0, (0x0100, 0)): 0x50}))
    q = {(f['name'], f['core']): f for f in q_facts}
    assert q[('gic.dist.gicd_ctlr.ds1_ds', 0)]['value'] == 1 and q[('gic.dist.gicd_ctlr.ds1_are', 0)]['value'] == 1
    assert ('gic.dist.gicd_ctlr.are_ns', 0) not in q and q[('gic.dist.spi71.priority', 0)]['status'] == 'measured'
    # MIDR and MPIDR are what EL1 reads, which the monitor sets from the hardware registers.
    assert 'VPIDR_EL2' in by[('cpu.midr_el1', 0)]['note']
    assert by[('gic.dist.spi97.route_requested', 0)]['value'] == 0x100d8
    assert by[('mmio.0x070101ac', 0)]['value'] == 1
    ppu = by[('mmio.0x07064000', 0)]
    assert ppu['status'] == 'unmeasured' and ppu['value'] is None
    assert by[('gic.cpuif.icc_ctlr_el1.pribits', 3)]['view'] == {'el': 1, 'security': 'non-secure', 'interface': 'gic-cpu-interface'}
    assert by[('cpu.midr_el1', 0)]['source'] == 'MIDR_EL1'
    # A table the decoder must refuse.
    for broken in (blob[:-4], bytes(len(blob)), blob[:8] + bytes([2]) + blob[9:]):
        try:
            hw.decode(broken)
        except ValueError:
            continue
        raise AssertionError('a malformed table was accepted')
    unfinished = bytearray(blob)
    unfinished[44:48] = (0x7f).to_bytes(4, 'little')
    try:
        hw.decode(bytes(unfinished))
        raise AssertionError('a table with a core missing was accepted')
    except ValueError as error:
        assert 'finished' in str(error)
    moved = bytearray(blob)
    moved[hw.CORE_OFFSET + 2 * hw.CORE_SLOTS * 16 + 2] = 5  # core 2's first slot claims core 5
    try:
        hw.decode(bytes(moved))
        raise AssertionError('a slot of the wrong core was accepted')
    except ValueError:
        pass
    # The virtual board's table never becomes hardware facts.
    qemu = hw.encode(platform=hw.PLATFORM_QEMU_OS)
    q_header, q_facts = hw.decode(qemu)
    try:
        hw.merge(hw.empty_document(), q_header, q_facts, 'entry', '0' * 64)
        raise AssertionError('a virtual board table was merged')
    except ValueError:
        pass
    # A board table replaces the unmeasured and observed entries of the facts it reads.
    document = hw.merge(hw.seeded_document(), header, facts, 'queue-entry-x', 'a' * 64, '3-3')
    merged = {(e['name'], e['core']): e for e in document['facts']}
    assert merged[('gic.redist.intid27.priority', 0)]['status'] == 'measured'
    assert merged[('gic.redist.intid27.priority', 0)]['queue_entry'] == 'queue-entry-x'
    assert ('gic.cpuif.icc_pmr_el1.readback_after_write_ff', 'all') not in merged
    assert merged[('gic.cpuif.icc_igrpen0_el1.read_traps_to_el3', 4)]['status'] == 'measured'
    assert merged[('runner.mmio.0x070101ac', None)]['status'] == 'observed'
    return blob


def test_document():
    document = hw.load_document()
    assert document['schema'] == hw.SCHEMA
    names = {e['name'] for e in document['facts']}
    catalogued = {d['name'] for k in hw.CORE_SPECS + hw.SHARED_SPECS for d in hw.FACTS[k] if d.get('views') in (None, hw.NS0)}
    assert catalogued <= names, sorted(catalogued - names)[:5]
    seen = set()
    for item in document['facts']:
        assert item['status'] in ('measured', 'observed', 'unmeasured', 'raz-in-view', 'from-source', 'derived'), item
        key = (item['name'], item['core'])
        assert key not in seen, key
        seen.add(key)
        assert set(item['view']) == {'el', 'security', 'interface'} and item['source'], item
        if item['status'] == 'derived':
            assert item['value'] == hw.ns_view(next(e['value'] for e in document['facts'] if e['name'] == item['derived_from']))
            continue
        if item['status'] == 'from-source':
            assert isinstance(item['value'], int) and re.fullmatch(r'[\w./-]+\.\w+:\d+(-\d+)?.*', item['source_ref']), item
            assert (ROOT / item['source_ref'].split(':')[0]).is_file(), item['source_ref']
            continue
        if item['status'] in ('unmeasured', 'raz-in-view'):
            assert item['value'] is None
            assert item['status'] == 'unmeasured' or item['note'], item
        else:
            assert isinstance(item['value'], int) and 'queue_entry' in item, item
            if item['queue_entry'] is None:
                assert item['note'], 'an observation without a queue entry says why'
        assert item['core'] in (None, 'all') or item['core'] in range(hw.CORES), item
    facts = hw.Facts()
    # The board run of queue entry 1793590887901610846-fc9d445f (port 3-1).
    run = '1793590887901610846-fc9d445f'
    assert facts.entry('gic.cpuif.icc_pmr_el1.readback_after_write_ff', 3)['queue_entry'] == run
    assert facts.value('gic.cpuif.icc_pmr_el1.readback_after_write_ff', 3) == 0xf0
    assert facts.value('gic.cpuif.icc_ctlr_el1.pribits', 0) == 4
    assert facts.value('gic.redist.intid27.priority_after_write', 0) == 0xd0
    assert facts.value('gic.dist.spi97.priority', 0) == hw.ns_view(facts.value('config.spi97.stored_priority'))
    assert facts.value('cpu.cntfrq_el0', 5) == 24000000
    assert facts.entry('gic.cpuif.icc_igrpen0_el1.read_traps_to_el3', 4)['queue_entry'] == '1793590176547518274-e819ce89'
    assert facts.value('runner.mmio.0x07064000') == 8 and facts.value('runner.rail.dcdc3_uv') == 800000
    try:
        facts.value('gic.dist.nonexistent')
        raise AssertionError('an unknown fact answered')
    except hw.MissingFact:
        pass
    assert facts.assume('gic.dist.nonexistent', 7, 'test') == 7 and 'gic.dist.nonexistent' in facts.assumed


def variant(readback):
    document = hw.load_document()
    for item in document['facts']:
        if item['name'] == 'gic.cpuif.icc_pmr_el1.readback_after_write_ff':
            item['value'] = readback
    path = variant.directory / ('facts-%x.json' % readback)
    path.write_text(json.dumps(document))
    return hw.Facts(path)


def test_models():
    gic = hw.GicModel()
    facts = hw.Facts()
    assert gic.mask == 0xf8 and gic.shifted and not facts.assumed.get('gic.cpuif.icc_pmr_el1.readback_after_write_ff')
    # The model reproduces the measurements: an unmasked write reads back 0xf0, and the image's write of
    # 0xd8 to the priority byte of PPI 27 reads back 0xd0.
    assert gic.pmr_ns_read(gic.pmr_ns_write(0xf8, 0xff)) == facts.value('gic.cpuif.icc_pmr_el1.readback_after_write_ff', 0)
    assert gic.pmr_ns_write(0x00, 0xff) == 0x00 and gic.pmr_ns_read(0x00) == 0
    assert gic.ns_read(gic.ns_write(facts.value('config.intid27.stored_priority_written_ns'))) == facts.value('gic.redist.intid27.priority_after_write', 0)
    # The priority-write path of checks/cpu-local-timer.py: a Non-secure write reads back through the same pair.
    assert gic.ns_read(gic.ns_write(facts.value('config.intid27.stored_priority_written_ns'))) == facts.value('gic.redist.intid27.priority_after_write', 0)
    intid30 = next(f for f in facts.document['facts'] if f['name'] == 'gic.redist.intid30.priority_after_write' and f['core'] == 0)
    assert gic.ns_read(gic.ns_write(intid30['written'])) == intid30['value'], 'INTID 30 written 0xe0 reads back 0xe0'
    assert gic.ns_read(facts.value('config.spi97.stored_priority')) == facts.value('gic.dist.spi97.priority', 0), 'the stored route priority of INTID 97 as Non-secure EL1 reads it'
    # A table that reads 0xf8 selects the unshifted view; one that fits neither is a contradiction.
    plain = hw.GicModel(variant(0xf8))
    assert not plain.shifted and plain.pmr_ns_read(plain.pmr_ns_write(0xf8, 0xff)) == 0xf8
    try:
        hw.GicModel(variant(0xc4))
        raise AssertionError('a contradicting table was accepted')
    except hw.ContradictsTable:
        pass


def test_no_hardcoded_values():
    for pattern, sample in ((r'&0xf8', "raw=((v>>1)|128)&0xf8"), ('shift', "value=(mask<<1)&255 if mask&128 else 0"),
                            ('ns', "r['icc_pmr_el1']=((value>>1)|128)&0xf8"), ('rpr', "assert (priority << 1) & 255 == 0xe0")):
        assert scan(sample), 'the scan misses ' + pattern
    assert not scan("value = GIC.pmr_ns_read(mask)  # not (mask << 1) & 255")
    problems = []
    for name in MODELS:
        text = (ROOT / name).read_text()
        problems += ['%s:%d: %s: %s' % (name, n, what, line) for n, what, line in scan(text)]
        assert 'GicModel' in text or 'first.GIC' in text or 'GIC.' in text, name + ' does not use the facts model'
    assert not problems, 'a host model hard-codes values that docs/hardware/a733-facts.json states:\n' + '\n'.join(problems)


ALLOWED_SYSREGS = {
    'MPIDR_EL1', 'MIDR_EL1', 'REVIDR_EL1', 'CNTFRQ_EL0', 'CTR_EL0', 'CLIDR_EL1', 'DCZID_EL0', 'CCSIDR_EL1', 'CSSELR_EL1',
    'ID_AA64PFR0_EL1', 'ID_AA64PFR1_EL1', 'ID_AA64DFR0_EL1', 'ID_AA64DFR1_EL1', 'ID_AA64AFR0_EL1', 'ID_AA64ISAR0_EL1',
    'ID_AA64ISAR1_EL1', 'ID_AA64ISAR2_EL1', 'ID_AA64MMFR0_EL1', 'ID_AA64MMFR1_EL1', 'ID_AA64MMFR2_EL1', 'ID_AA64MMFR3_EL1',
    'ID_AA64ZFR0_EL1', 'ICC_CTLR_EL1', 'ICC_PMR_EL1', 'ICC_RPR_EL1', 'ICC_SRE_EL1', 'ICC_IGRPEN1_EL1', 'ICC_BPR1_EL1'}
# Registers whose Non-secure EL1 access traps to EL3 or EL2 on the monitor (queue entry 1793590176547518274-e819ce89 for the first).
TRAPPING = re.compile(r'ICC_(IGRPEN0|BPR0|AP0R\d|IAR0|EOIR0|HPPIR0|SRE_EL[23])|^(PM|DBG|OSL|MDS|MDC|S3_[1-7]|S[2-3]_)|_EL[23]$|^TRC|^CPU|^ACTLR|^AMU', re.I)


def disassemble(object_or_image, symbols=None):
    import subprocess
    command = ['/usr/lib/llvm-21/bin/llvm-objdump', '-d', '--no-show-raw-insn']
    for symbol in symbols or ():
        command.append('--disassemble-symbols=' + symbol)
    return subprocess.run(command + [str(object_or_image)], capture_output=True, text=True, check=True).stdout


def registers_of(line):
    match = re.search(r'\b(mrs|msr)\s+(.+)$', line)
    if not match:
        return []
    operands = [o.strip() for o in match.group(2).split(',')]
    return [o for o in operands if not re.fullmatch(r'[xw]\d+|xzr|wzr', o)]


def test_instructions(image):
    """The capture code reads and writes only registers that cannot trap at Non-secure EL1."""
    import subprocess, tempfile
    if image is None:
        build = Path(tempfile.mkdtemp(dir=variant.directory))
        obj = build / 'hw_facts.o'
        subprocess.run(['/usr/bin/clang-21', '--target=aarch64-none-elf', '-ffreestanding', '-O2', '-mgeneral-regs-only',
                        '-mstrict-align', '-I', str(ROOT / 'include'), '-I', str(ROOT / 'platform/a733-smp-cached'), '-c',
                        str(ROOT / 'platform/a733/hw_facts.c'), '-o', str(obj)], check=True)
        text = disassemble(obj)
    else:
        text = disassemble(image, ('hwf_capture', 'capture_core', 'capture_shared', 'ccsidr', 'id_register', 'pmr_read', 'pmr_write'))
    used = {r for line in text.splitlines() for r in registers_of(line)}
    assert used, 'no system register access found; the disassembly is empty'
    assert 'ICC_PMR_EL1' in used and 'MIDR_EL1' in used
    trapping = sorted(r for r in used if TRAPPING.search(r))
    assert not trapping, 'the probe accesses registers that trap at Non-secure EL1: %s' % trapping
    unknown = sorted(used - ALLOWED_SYSREGS)
    assert not unknown, 'the probe accesses registers outside the allowed list: %s' % unknown
    # Negative control: the register that trapped in entry 1793590176547518274-e819ce89 is refused.
    assert TRAPPING.search('ICC_IGRPEN0_EL1') and registers_of('mrs x21, ICC_IGRPEN0_EL1') == ['ICC_IGRPEN0_EL1']
    return sorted(used)


# Priority comparisons against a literal. A priority is compared with a literal only through the helper
# (include/a733_gic_priority.h, tools/hw_facts.py ns_view and stored_from_ns). These reviewed files still do, each with
# the number of comparisons: the stream that owns the file changes it, or the image reads the GIC in its own monitor's view.
LITERAL_COMPARISONS = {
    'apps/npu-finite/deadline.c': (1, 'NPU stream: expects the Non-secure view 0xb0 of the stored 0xd8'),
    'apps/spi/irq.c': (2, 'single-core SPI image: compares with 0xb0 in its own monitor view'),
    'drivers/pwm/a733_pwm.c': (1, 'single-core PWM image: compares with 0xd8 in its own monitor view'),
    'checks/a733-irq-table.c': (1, 'model of the route installer: stored priority'),
    'checks/sunstone-image-link.py': (1, 'route floor: the stored API floor 0xd8 compared with the stored priority'),
}
PRIORITY_LITERAL = re.compile(r'(?:==|!=|<=|>=|(?<![<])<(?![<])|(?<![>])>(?![>]))\s*(?:UINT32_C\()?0x(?:b0|d8)(?:0{6})?u?\b|'
                              r'\b0x(?:b0|d8)(?:0{6})?u?\)?\s*(?:==|!=|<=|>=)')
PRIORITY_CONTEXT = re.compile(r'prio|>>\s*24|0x4[0-9a-f]{2}\\b|SGI|GIC|configMAX_API|read32|mmio_read|\.read\(|\[.priority.\]', re.I)


def literal_comparisons(text):
    return [n for n, line in enumerate(text.splitlines(), 1)
            if PRIORITY_LITERAL.search(line.split('//')[0]) and PRIORITY_CONTEXT.search(line.split('//')[0])]


def test_helper():
    """The C macros and the Python functions are one rule, and the facts file obeys it."""
    import subprocess, tempfile
    source = ('#include <stdio.h>\n#include "a733_gic_priority.h"\nint main(void){for(unsigned i=0;i<256;++i)'
              'printf("%u %u %u\\n",i,A733_GIC_NS_VIEW(i),A733_GIC_STORED_FROM_NS(i));return 0;}\n')
    work = Path(tempfile.mkdtemp(dir=variant.directory))
    (work / 'p.c').write_text(source)
    subprocess.run(['cc', '-I', str(ROOT / 'include'), str(work / 'p.c'), '-o', str(work / 'p')], check=True)
    rows = [tuple(map(int, line.split())) for line in subprocess.run([str(work / 'p')], capture_output=True, text=True, check=True).stdout.splitlines()]
    assert len(rows) == 256 and all(ns == hw.ns_view(i) and st == hw.stored_from_ns(i) for i, ns, st in rows)
    facts = hw.Facts()
    for intid in (71, 95, 97, 143):
        stored = facts.value('config.spi%d.stored_priority' % intid)
        assert facts.value('gic.dist.spi%d.priority' % intid, 0) == hw.ns_view(stored) == facts.value('config.spi%d.ns_view_priority' % intid)
    ppi = facts.value('config.intid27.stored_priority_written_ns')
    assert facts.value('gic.redist.intid27.priority_after_write', 0) == hw.ns_view(hw.stored_from_ns(ppi))
    api = facts.value('gic.cpuif.icc_pmr_el1.readback_after_write_api_mask', 0)
    assert hw.stored_from_ns(api) == facts.value('config.spi97.stored_priority'), 'the FreeRTOS API mask and the INTID 97 route priority meet'


def test_priority_literals():
    import subprocess
    assert literal_comparisons('if((read32(0x4b8)>>24)!=0xb0)return 0;') and literal_comparisons("assert by['priority'] == 0xd8")
    assert not literal_comparisons('x = A733_GIC_NS_VIEW(0xd8) << 8;') and not literal_comparisons('put(g+0x460,0xb0<<8,4);')
    tracked = subprocess.run(['git', '-C', str(ROOT), 'ls-files'], capture_output=True, text=True, check=True).stdout.split()
    found = {}
    for name in tracked:
        if not name.endswith(('.c', '.h', '.py', '.S')) or name.startswith(('docs/', 'include/a733_gic_priority.h', 'tools/hw_facts.py', 'checks/hw-facts.py')):
            continue
        hits = literal_comparisons((ROOT / name).read_text(errors='replace'))
        if hits:
            found[name] = len(hits)
    unexpected = {n: c for n, c in found.items() if n not in LITERAL_COMPARISONS or LITERAL_COMPARISONS[n][0] != c}
    gone = [n for n in LITERAL_COMPARISONS if n not in found]
    assert not unexpected, 'priority compared with a literal outside the helper: %s' % unexpected
    assert not gone, 'remove from LITERAL_COMPARISONS: %s' % gone


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--image', type=Path, help='also disassemble the capture code of this linked image')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    variant.directory = args.output
    test_layout()
    blob = test_decoder()
    test_document()
    test_models()
    test_no_hardcoded_values()
    test_helper()
    test_priority_literals()
    registers = test_instructions(None)
    if args.image:
        test_instructions(args.image)
    assumed = hw.GicModel().facts.assumed
    (args.output / 'result.json').write_text(json.dumps({'passed': True, 'table_bytes': hw.TABLE_BYTES,
        'core_slots': len(hw.CORE_SPECS), 'shared_slots': len(hw.SHARED_SPECS), 'models': MODELS,
        'assumptions': sorted(assumed), 'system_registers': registers}, indent=2) + '\n')
    print('PASS hw-facts: layout, decoder, facts file and %d host models' % len(MODELS))


if __name__ == '__main__':
    main()
