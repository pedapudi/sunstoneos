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
MODELS = ('checks/a733-smp-cached.py', 'checks/a733-supervised-scheduler.py', 'checks/all-core-first-task-probe.py',
          'checks/all-core-task-workload-probe.py', 'checks/primary-cache-probe.py', 'checks/secondary-interrupt-probe.py')
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
    values[('shared', 0, (0x0100, 0))] = 0x11
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
    assert by[('gic.redist.intid27.priority', 4)]['value'] == 0xd8 and by[('gic.redist.intid26.priority', 4)]['value'] == 0xa0
    assert by[('gic.dist.spi97.priority', 0)]['value'] == 0x58 and by[('gic.dist.spi97.group1', 0)]['value'] == 1
    assert by[('gic.dist.spi97.enabled', 0)]['value'] == 1 and by[('gic.dist.spi97.irouter', 0)]['value'] == 2 << 8
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
    assert merged[('runner.mmio.0x070101ac', None)]['status'] == 'observed'
    return blob


def test_expectation(blob):
    expected = hw.expectation()
    assert len(expected['words']) * 4 == hw.TABLE_BYTES
    assert hw.matches(blob, expected)
    bad = bytearray(blob)
    bad[hw.CORE_OFFSET] ^= 1  # a slot identity
    assert not hw.matches(bytes(bad), expected)
    bad = bytearray(blob)
    bad[12] ^= 1  # the table size
    assert not hw.matches(bytes(bad), expected)
    pad = bytearray(blob)
    pad[hw.SHARED_OFFSET + len(hw.SHARED_SPECS) * 16 + 8] = 1  # a slot past the catalogue is zero
    assert not hw.matches(bytes(pad), expected)


def test_document():
    document = hw.load_document()
    assert document['schema'] == hw.SCHEMA
    names = {e['name'] for e in document['facts']}
    catalogued = {d['name'] for k in hw.CORE_SPECS + hw.SHARED_SPECS for d in hw.FACTS[k]}
    assert catalogued <= names, sorted(catalogued - names)[:5]
    seen = set()
    for item in document['facts']:
        assert item['status'] in ('measured', 'observed', 'unmeasured'), item
        key = (item['name'], item['core'])
        assert key not in seen, key
        seen.add(key)
        assert set(item['view']) == {'el', 'security', 'interface'} and item['source'], item
        if item['status'] == 'unmeasured':
            assert item['value'] is None
        else:
            assert isinstance(item['value'], int) and 'queue_entry' in item, item
            if item['queue_entry'] is None:
                assert item['note'], 'an observation without a queue entry says why'
        assert item['core'] in (None, 'all') or item['core'] in range(hw.CORES), item
    facts = hw.Facts()
    assert facts.value('gic.redist.intid27.priority', 0) == 0xb0
    assert facts.entry('gic.redist.intid27.priority', 0)['queue_entry'] == '1793587317962103239-5bbfc48d'
    assert facts.value('gic.cpuif.icc_pmr_el1.readback_after_write_ff', 3) == 0xf8
    assert facts.value('runner.mmio.0x07064000') == 8 and facts.value('runner.rail.dcdc3_uv') == 800000
    assert not facts.has('gic.dist.spi97.priority')
    try:
        facts.value('gic.dist.spi97.priority')
        raise AssertionError('an unmeasured fact answered')
    except hw.MissingFact:
        pass
    assert facts.assume('gic.dist.spi97.priority', 0xd8, 'test') == 0xd8 and 'gic.dist.spi97.priority' in facts.assumed


def variant(readback, observed_priority=None):
    document = hw.load_document()
    for item in document['facts']:
        if item['name'] == 'gic.cpuif.icc_pmr_el1.readback_after_write_ff':
            item['value'] = readback
        if item['name'] == 'gic.redist.intid27.priority' and observed_priority is not None:
            item['value'] = observed_priority
    path = variant.directory / ('facts-%x-%s.json' % (readback, observed_priority))
    path.write_text(json.dumps(document))
    return hw.Facts(path)


def test_models():
    gic = hw.GicModel()
    assert gic.mask == 0xf8 and not gic.shifted
    # The model reproduces the observation: an unmasked write reads back 0xf8, whatever the Secure mask was.
    assert gic.pmr_ns_read(gic.pmr_ns_write(0xf8, 0xff)) == hw.Facts().value('gic.cpuif.icc_pmr_el1.readback_after_write_ff')
    assert gic.pmr_ns_write(0x00, 0xff) == 0x00 and gic.pmr_ns_read(0x00) == 0
    assert gic.priority_ns_read(27, gic.ns_write(0xd8)) == 0xb0, 'the observed Non-secure view of PPI 27'
    assert gic.priority_ns_read(30, gic.ns_write(0xe0)) == 0xe0
    # A table that reads 0xf0 selects the shifted view; one that fits neither is a contradiction.
    shifted = hw.GicModel(variant(0xf0))
    assert shifted.shifted and shifted.ns_read(shifted.ns_write(0xe0)) == 0xe0 and shifted.pmr_ns_read(shifted.pmr_ns_write(0xf8, 0xff)) == 0xf0
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    variant.directory = args.output
    test_layout()
    blob = test_decoder()
    test_expectation(blob)
    test_document()
    test_models()
    test_no_hardcoded_values()
    assumed = hw.GicModel().facts.assumed
    (args.output / 'result.json').write_text(json.dumps({'passed': True, 'table_bytes': hw.TABLE_BYTES,
        'core_slots': len(hw.CORE_SPECS), 'shared_slots': len(hw.SHARED_SPECS), 'models': MODELS,
        'assumptions': sorted(assumed)}, indent=2) + '\n')
    print('PASS hw-facts: layout, decoder, facts file and %d host models' % len(MODELS))


if __name__ == '__main__':
    main()
