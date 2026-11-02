#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The A733 hardware facts: the probe's table layout, its decoder and the loader that host models use.

The catalogue below is the single description of the table that the probe image
writes (platform/a733/hw_facts.c). `gen` writes the C slot table
include/a733_hw_facts_table.inc from it, `decode` turns a returned table into
facts, `merge` folds measured facts into docs/hardware/a733-facts.json, and the
`Facts` class gives host models the values of that file.

  hw_facts.py gen                      rewrite include/a733_hw_facts_table.inc
  hw_facts.py init                     write docs/hardware/a733-facts.json with only the observed seeds
  hw_facts.py decode BIN [--merge JSON --queue-entry ID ...]
  hw_facts.py expect OUT.json          the board word expectation for a returned table
"""
import argparse
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FACTS_JSON = ROOT / 'docs/hardware/a733-facts.json'
TABLE_INC = ROOT / 'include/a733_hw_facts_table.inc'
SCHEMA = 'sunstone-a733-facts-v1'

MAGIC = 0x3146574833373341  # "A733HWF1" little-endian
VERSION = 1
HEADER_BYTES, SLOT_BYTES = 64, 16
CORES, CORE_SLOTS, SHARED_SLOTS = 8, 64, 96
SHARED_OFFSET = HEADER_BYTES
CORE_OFFSET = SHARED_OFFSET + SHARED_SLOTS * SLOT_BYTES
TABLE_BYTES = CORE_OFFSET + CORES * CORE_SLOTS * SLOT_BYTES
PLATFORM_A733, PLATFORM_QEMU_OS = 1, 2
PLATFORMS = {PLATFORM_A733: 'a733-board', PLATFORM_QEMU_OS: 'qemu-os-board'}
HEADER = struct.Struct('<Q10I16x')  # magic version bytes platform cores shared core shared_off core_off stride done
assert HEADER.size == HEADER_BYTES
SLOT = struct.Struct('<HBBIQ')  # id core flags aux value
assert SLOT.size == SLOT_BYTES
FLAG_VALID, FLAG_SKIPPED, FLAG_ANOMALY = 1, 2, 4

SECURITY = 'non-secure'
VIEWS = {
    'sysreg': {'el': 1, 'security': SECURITY, 'interface': 'cpu-register'},
    'cpuif': {'el': 1, 'security': SECURITY, 'interface': 'gic-cpu-interface'},
    'redist': {'el': 1, 'security': SECURITY, 'interface': 'gic-redistributor-sgi-frame'},
    'dist': {'el': 1, 'security': SECURITY, 'interface': 'gic-distributor'},
    'mmio': {'el': 1, 'security': SECURITY, 'interface': 'device-memory'},
}

# The shared peripheral interrupts that some SunstoneOS image uses (apps, drivers and the
# generated route table): SPI ports 56..61 and 242, PWM 67, LRADC 69, ADC 70, thermal 71,
# video units 80 and 81, GPU 95, NPU 97, DMA0 143, ISP0 149, 153 and 161, PCIe 185, the USB2
# host 187, crypto engine 200 and 201, RTC alarm 228.
SPIS = (56, 57, 58, 59, 60, 61, 67, 69, 70, 71, 80, 81, 95, 97, 143, 149, 153, 161, 185, 187, 200, 201, 228, 242)
NPU_WORDS = (0x070101ac, 0x020025c0, 0x020025e0, 0x07064000, 0x07064008)
# Gates that must be open before the NPU PPU words are read (tools/board_images.py NPU_ENTRY_GATES).
NPU_GATES = {0x070101ac: 1, 0x020025c0: 1 << 6, 0x020025e0: 1 << 18}


NS0, DS1 = 'ns-ds0', 'ds1'
# The layout in which each platform's GIC registers are decoded. The A733 has two security states (GICD_CTLR.DS clear):
# inferred from the shifted ICC_PMR_EL1 view and from the trap of ICC_IGRPEN0_EL1, not read, because DS is not visible
# to Non-secure software. The virtual board's GIC has DS set.
LAYOUT = {1: NS0, 2: DS1}


def f(name, lo, hi, text='', views=None):
    return (name, lo, hi, text, views)


MIDR = [f('implementer', 24, 31), f('variant', 20, 23), f('architecture', 16, 19), f('partnum', 4, 15), f('revision', 0, 3)]
MPIDR = [f('aff0', 0, 7), f('aff1', 8, 15), f('aff2', 16, 23), f('mt', 24, 24), f('u', 30, 30), f('aff3', 32, 39)]
CTR = [f('iminline', 0, 3), f('dminline', 16, 19), f('erg', 20, 23), f('cwg', 24, 27), f('idc', 28, 28), f('dic', 29, 29)]
PFR0 = [f('el0', 0, 3), f('el1', 4, 7), f('el2', 8, 11), f('el3', 12, 15), f('fp', 16, 19), f('advsimd', 20, 23),
        f('gic', 24, 27), f('ras', 28, 31), f('sve', 32, 35)]
ISAR0 = [f('aes', 4, 7), f('sha1', 8, 11), f('sha2', 12, 15), f('crc32', 16, 19), f('atomic', 20, 23),
         f('rdm', 28, 31), f('dp', 44, 47)]
MMFR0 = [f('parange', 0, 3), f('asidbits', 4, 7), f('tgran4', 28, 31), f('tgran64', 24, 27), f('tgran16', 20, 23)]
MMFR2 = [f('ccidx', 20, 23), f('varange', 16, 19)]
ICC_CTLR = [f('cbpr', 0, 0), f('eoimode', 1, 1), f('pmhe', 6, 6), f('pribits', 8, 10), f('idbits', 11, 13),
            f('seis', 14, 14), f('a3v', 15, 15), f('rss', 18, 18), f('extrange', 19, 19)]
# GICD_CTLR as Non-secure software reads it with DS clear (IHI0069, GICD_CTLR, "Non-secure access when GICD_CTLR.DS == 0"):
# bit 0 EnableGrp1, bit 1 EnableGrp1A, bit 4 ARE_NS, bit 31 RWP, the rest RES0. DS, ARE_S, EnableGrp0 and EnableGrp1S are
# not visible. With DS set the register is the single-state layout.
GICD_CTLR = [f('enable_grp1', 0, 0, views=NS0), f('enable_grp1a', 1, 1, views=NS0), f('are_ns', 4, 4, views=NS0), f('rwp', 31, 31, views=NS0),
             f('ds1_enable_grp0', 0, 0, views=DS1), f('ds1_enable_grp1', 1, 1, views=DS1), f('ds1_are', 4, 4, views=DS1),
             f('ds1_ds', 6, 6, views=DS1), f('ds1_e1nwf', 7, 7, views=DS1), f('ds1_rwp', 31, 31, views=DS1)]
GICD_TYPER = [f('itlinesnumber', 0, 4), f('cpunumber', 5, 7), f('espi', 8, 8), f('securityextn', 10, 10),
              f('mbis', 16, 16), f('lpis', 17, 17), f('dvis', 18, 18), f('idbits', 19, 23), f('a3v', 24, 24)]
GICR_TYPER = [f('plpis', 0, 0), f('vlpis', 1, 1), f('last', 4, 4), f('processor_number', 8, 23), f('affinity', 32, 63)]
SPI_STATE = [f('priority', 0, 7), f('group1', 8, 8), f('group_modifier', 9, 9), f('enabled', 10, 10),
             f('pending', 11, 11), f('active', 12, 12), f('config', 13, 14)]

# kind: 'reg' (one slot), or 'array' (one slot per aux value). `op` names the C reader.
# Entries: (id, scope, key, source, view, width, fields, aux values)
CORE_REGS = [
    (0x0001, 'cpu.mpidr_el1', 'MPIDR_EL1', 'sysreg', 64, MPIDR),
    (0x0002, 'cpu.midr_el1', 'MIDR_EL1', 'sysreg', 64, MIDR),
    (0x0003, 'cpu.revidr_el1', 'REVIDR_EL1', 'sysreg', 64, []),
    (0x0004, 'cpu.cntfrq_el0', 'CNTFRQ_EL0', 'sysreg', 64, []),
    (0x0005, 'cpu.ctr_el0', 'CTR_EL0', 'sysreg', 64, CTR),
    (0x0006, 'cpu.clidr_el1', 'CLIDR_EL1', 'sysreg', 64, []),
    (0x0007, 'cpu.dczid_el0', 'DCZID_EL0', 'sysreg', 64, []),
]
CORE_ARRAYS = [
    (0x0008, 'cache.ccsidr_el1.sel{aux}', 'CCSIDR_EL1 (CSSELR_EL1={aux})', 'sysreg', 64, [], tuple(range(8))),
]
ID_REGS = [
    (0x0010, 'cpu.id_aa64pfr0_el1', 'ID_AA64PFR0_EL1', PFR0), (0x0011, 'cpu.id_aa64pfr1_el1', 'ID_AA64PFR1_EL1', []),
    (0x0012, 'cpu.id_aa64dfr0_el1', 'ID_AA64DFR0_EL1', []), (0x0013, 'cpu.id_aa64dfr1_el1', 'ID_AA64DFR1_EL1', []),
    (0x0014, 'cpu.id_aa64afr0_el1', 'ID_AA64AFR0_EL1', []), (0x0015, 'cpu.id_aa64isar0_el1', 'ID_AA64ISAR0_EL1', ISAR0),
    (0x0016, 'cpu.id_aa64isar1_el1', 'ID_AA64ISAR1_EL1', []), (0x0017, 'cpu.id_aa64isar2_el1', 'ID_AA64ISAR2_EL1', []),
    (0x0018, 'cpu.id_aa64mmfr0_el1', 'ID_AA64MMFR0_EL1', MMFR0), (0x0019, 'cpu.id_aa64mmfr1_el1', 'ID_AA64MMFR1_EL1', []),
    (0x001a, 'cpu.id_aa64mmfr2_el1', 'ID_AA64MMFR2_EL1', MMFR2), (0x001b, 'cpu.id_aa64mmfr3_el1', 'ID_AA64MMFR3_EL1', []),
    (0x001c, 'cpu.id_aa64zfr0_el1', 'ID_AA64ZFR0_EL1', []),
]
CPUIF = [
    (0x0030, 'gic.cpuif.icc_ctlr_el1', 'ICC_CTLR_EL1', ICC_CTLR),
    (0x0031, 'gic.cpuif.icc_pmr_el1.inherited', 'ICC_PMR_EL1 (value on entry to EL1)', []),
    (0x0032, 'gic.cpuif.icc_rpr_el1.idle', 'ICC_RPR_EL1 (no interrupt active)', []),
    (0x0033, 'gic.cpuif.icc_sre_el1', 'ICC_SRE_EL1', []),
    (0x0034, 'gic.cpuif.icc_igrpen1_el1', 'ICC_IGRPEN1_EL1', []),
    (0x0035, 'gic.cpuif.icc_bpr1_el1', 'ICC_BPR1_EL1', []),
]
# ICC_IGRPEN0_EL1 and ICC_BPR0_EL1 are not read: a Non-secure EL1 read of a Group 0 register traps to EL3 on the A733 (see SEEDS).
# Writes of ICC_PMR_EL1 with values that every image writes (0xff, and the FreeRTOS API mask); the
# value on entry is restored. aux 0 writes 0xff, aux 1 writes the image's API mask; the slot's aux
# field returns the written value.
PMR_WRITES = [(0x0038, 'gic.cpuif.icc_pmr_el1.readback_after_write_ff', 'ICC_PMR_EL1 (read back after writing 0xff)', 0),
              (0x0039, 'gic.cpuif.icc_pmr_el1.readback_after_write_api_mask',
               'ICC_PMR_EL1 (read back after writing the FreeRTOS API mask)', 1)]
REDIST = [
    (0x0040, 'gic.redist.gicr_typer', 'GICR_TYPER', 64, GICR_TYPER), (0x0041, 'gic.redist.gicr_waker', 'GICR_WAKER', 32, []),
    (0x0042, 'gic.redist.gicr_iidr', 'GICR_IIDR', 32, []), (0x0043, 'gic.redist.sgi_ppi.igroupr0', 'GICR_IGROUPR0', 32, []),
    (0x0044, 'gic.redist.sgi_ppi.igrpmodr0', 'GICR_IGRPMODR0', 32, []), (0x0045, 'gic.redist.sgi_ppi.isenabler0', 'GICR_ISENABLER0', 32, []),
    (0x0046, 'gic.redist.sgi_ppi.ispendr0', 'GICR_ISPENDR0', 32, []), (0x0047, 'gic.redist.sgi_ppi.isactiver0', 'GICR_ISACTIVER0', 32, []),
    (0x0048, 'gic.redist.sgi_ppi.icfgr0', 'GICR_ICFGR0', 32, []), (0x0049, 'gic.redist.sgi_ppi.icfgr1', 'GICR_ICFGR1', 32, []),
]
PRIORITY_WORDS = 8  # GICR_IPRIORITYR0..7: the priority bytes of INTID 0..31

SHARED_REGS = [
    (0x0100, 'gic.dist.gicd_ctlr', 'GICD_CTLR', 32, GICD_CTLR), (0x0101, 'gic.dist.gicd_typer', 'GICD_TYPER', 32, GICD_TYPER),
    (0x0102, 'gic.dist.gicd_iidr', 'GICD_IIDR', 32, []), (0x0103, 'gic.dist.gicd_typer2', 'GICD_TYPER2', 32, []),
    (0x0104, 'gic.dist.gicd_pidr2', 'GICD_PIDR2', 32, []),
]
ID_SPI_STATE, ID_SPI_ROUTER, ID_SPI_REQUEST = 0x0110, 0x0111, 0x0112
ID_NPU_WORD = 0x0130
# Write tests of the redistributor priority byte of PPI 27 and PPI 30 on the capturing core, with the
# value the cached platform writes next in the same boot; aux returns the written value.
ID_PPI_WRITE = 0x0140
PPI_WRITES = (27, 30)


def view(name):
    return dict(VIEWS[name])


def entry(name, source, view_name, scope, fields=(), **extra):
    return dict(name=name, source=source, view=view(view_name), scope=scope, fields=list(fields), **extra)


def build():
    """Ordered slot specifications per scope, and the fact descriptions the slots decode to.

    Returns (core_specs, shared_specs, facts) where a spec is (id, aux) and facts maps a slot
    key (id, aux) to a list of fact descriptions (name, source, view, scope, extractor)."""
    core, shared, facts = [], [], {}

    def add(specs, scope, key, descriptions):
        specs.append(key)
        facts[key] = descriptions

    def raw_and_fields(name, source, view_name, scope, width, fields, rule=None):
        out = [dict(name=name, source=source, view=view(view_name), scope=scope, width=width, lo=0, hi=width - 1)]
        for field_name, lo, hi, _, views in fields:
            out.append(dict(name=f'{name}.{field_name}', source=source, view=view(view_name), scope=scope,
                            width=hi - lo + 1, lo=lo, hi=hi, views=views))
        if name == 'cpu.midr_el1' or name == 'cpu.mpidr_el1':
            for item in out:
                item['note'] = 'the value EL1 reads: VPIDR_EL2 or VMPIDR_EL2, which the monitor sets from the hardware register'
        if rule:
            for item in out:
                item['rule'] = rule
        return out

    for ident, name, source, view_name, width, fields in CORE_REGS:
        add(core, 'core', (ident, 0), raw_and_fields(name, source, view_name, 'core', width, fields))
    for ident, name, source, view_name, width, fields, auxes in CORE_ARRAYS:
        for aux in auxes:
            add(core, 'core', (ident, aux), raw_and_fields(name.format(aux=aux), source.format(aux=aux), view_name,
                                                          'core', width, fields))
    for ident, name, source, fields in ID_REGS:
        add(core, 'core', (ident, 0), raw_and_fields(name, source, 'sysreg', 'core', 64, fields))
    for ident, name, source, fields in CPUIF:
        add(core, 'core', (ident, 0), raw_and_fields(name, source, 'cpuif', 'core', 64, fields))
    for ident, name, source, aux in PMR_WRITES:
        add(core, 'core', (ident, aux), raw_and_fields(name, source, 'cpuif', 'core', 64, []))
    for ident, name, source, width, fields in REDIST:
        rule = ('always-raz' if name.endswith(('igroupr0', 'igrpmodr0')) else
                'zero-raz' if '.sgi_ppi.' in name or name.endswith('gicr_waker') else None)
        add(core, 'core', (ident, 0), raw_and_fields(name, source, 'redist', 'core', width, fields, rule))
    for word in range(PRIORITY_WORDS):
        descriptions = []
        for byte in range(4):
            intid = word * 4 + byte
            descriptions.append(dict(name=f'gic.redist.intid{intid}.priority', source=f'GICR_IPRIORITYR{word} byte {byte}',
                                     view=view('redist'), scope='core', width=8, lo=byte * 8, hi=byte * 8 + 7, rule='zero-raz'))
        add(core, 'core', (0x004a, word), descriptions)
    assert len(core) <= CORE_SLOTS, len(core)

    for ident, name, source, width, fields in SHARED_REGS:
        add(shared, 'shared', (ident, 0), raw_and_fields(name, source, 'dist', 'shared', width, fields))
    for intid in SPIS:
        descriptions = []
        for field_name, lo, hi, _, _views in SPI_STATE:
            descriptions.append(dict(rule='always-raz' if field_name in ('group1', 'group_modifier') else 'spi', intid=intid, name=f'gic.dist.spi{intid}.{field_name}',
                                     source={'priority': f'GICD_IPRIORITYR{intid // 4} byte {intid % 4}',
                                             'group1': f'GICD_IGROUPR{intid // 32} bit {intid % 32}',
                                             'group_modifier': f'GICD_IGRPMODR{intid // 32} bit {intid % 32}',
                                             'enabled': f'GICD_ISENABLER{intid // 32} bit {intid % 32}',
                                             'pending': f'GICD_ISPENDR{intid // 32} bit {intid % 32}',
                                             'active': f'GICD_ISACTIVER{intid // 32} bit {intid % 32}',
                                             'config': f'GICD_ICFGR{intid // 16} bits {2 * (intid % 16) + 1}:{2 * (intid % 16)}'}[field_name],
                                     view=view('dist'), scope='shared', width=hi - lo + 1, lo=lo, hi=hi))
        add(shared, 'shared', (ID_SPI_STATE, intid), descriptions)
        add(shared, 'shared', (ID_SPI_ROUTER, intid), [dict(name=f'gic.dist.spi{intid}.irouter', source=f'GICD_IROUTER{intid}',
                                                            view=view('dist'), scope='shared', width=64, lo=0, hi=63, rule='spi', intid=intid)])
        add(shared, 'shared', (ID_SPI_REQUEST, intid), [
            dict(name=f'gic.dist.spi{intid}.route_requested', source='image route table (a733_irq_routes)',
                 view=view('dist'), scope='shared', width=32, lo=0, hi=31, note='bit 16 routed, bits 15:8 core, bits 7:0 secure priority; image data, not a register')])
    for ppi in PPI_WRITES:
        add(shared, 'shared', (ID_PPI_WRITE, ppi), [
            dict(name=f'gic.redist.intid{ppi}.priority_after_write', source=f'GICR_IPRIORITYR{ppi // 4} byte {ppi % 4} (read back after write)',
                 view=view('redist'), scope='shared', width=8, lo=0, hi=7, rule='zero-raz')])
    for address in NPU_WORDS:
        add(shared, 'shared', (ID_NPU_WORD, address), [
            dict(name=f'mmio.{address:#010x}', source=f'MMIO word {address:#010x}', view=view('mmio'), scope='shared', width=32, lo=0, hi=31)])
    assert len(shared) <= SHARED_SLOTS, len(shared)
    return core, shared, facts


CORE_SPECS, SHARED_SPECS, FACTS = build()


def table_inc():
    lines = ['/* Generated by tools/hw_facts.py gen; do not edit. Slot order of the hardware facts table. */']
    for spec in SHARED_SPECS:
        lines.append('HWF_SHARED(0x%04x, 0x%x)' % spec)
    for spec in CORE_SPECS:
        lines.append('HWF_CORE(0x%04x, 0x%x)' % spec)
    return '\n'.join(lines) + '\n'


# ---------------------------------------------------------------------------
# Decoding

def slot_offset(scope, index, core=0):
    if scope == 'shared':
        return SHARED_OFFSET + index * SLOT_BYTES
    return CORE_OFFSET + (core * CORE_SLOTS + index) * SLOT_BYTES


def parse_header(blob):
    if len(blob) != TABLE_BYTES:
        raise ValueError('the facts table must hold %d bytes, not %d' % (TABLE_BYTES, len(blob)))
    magic, version, size, platform, cores, shared, per_core, shared_off, core_off, stride, done = HEADER.unpack_from(blob)
    if (magic, version, size, cores, shared, per_core, shared_off, core_off, stride) != (
            MAGIC, VERSION, TABLE_BYTES, CORES, SHARED_SLOTS, CORE_SLOTS, SHARED_OFFSET, CORE_OFFSET, CORE_SLOTS * SLOT_BYTES):
        raise ValueError('the facts table header differs from the layout')
    if platform not in PLATFORMS:
        raise ValueError('unknown facts platform %d' % platform)
    return dict(platform=PLATFORMS[platform], platform_id=platform, done_mask=done)


RAZ = 'raz-in-view'
RAZ_NOTES = {
    'always-raz': 'GICD_IGROUPR, GICD_IGRPMODR, GICR_IGROUPR0 and GICR_IGRPMODR0 are RAZ/WI at Non-secure EL1 when GICD_CTLR.DS is clear '
                  '(IHI0069; platform/a733/npu_irq.c), so the value 0 carries no information',
    'zero-raz': 'reads 0: the interrupt may be Secure, for which a Non-secure read is RAZ/WI (IHI0069 4.8.1), or Non-secure with a zero '
                'field; the view cannot tell the two apart',
    'spi': 'every field of this interrupt reads 0 in the Non-secure view, which is also what a Secure interrupt reads (RAZ/WI); no field '
           'shows that the view can see it',
}


def apply_view_visibility(items):
    """Fields that read 0 because the Non-secure view cannot see them are not measurements.

    An SPI whose priority, enable, pending, active, configuration and route all read 0 is indistinguishable from a
    Secure interrupt; one with any nonzero field is visible, and its fields are measured."""
    visible = {}
    for item in items:
        if item.get('_rule') == 'spi':
            visible.setdefault(item['_intid'], False)
            if item['value']:
                visible[item['_intid']] = True
    for item in items:
        rule = item.get('_rule')
        if not rule or item['status'] != 'measured':
            continue
        hidden = rule == 'always-raz' or (rule == 'spi' and not visible[item['_intid']]) or (rule == 'zero-raz' and item['value'] == 0)
        if hidden:
            item.update(read_value=item['value'], value=None, status=RAZ, note=RAZ_NOTES[rule])


def decode(blob):
    """Facts of a returned table: a list of dicts with name, source, view, core, status, value."""
    header = parse_header(blob)
    layout = LAYOUT[header['platform_id']]
    if header['done_mask'] != (1 << CORES) - 1:
        raise ValueError('cores %#x finished, not all eight' % header['done_mask'])
    out, seen = [], set()
    for scope, specs in (('shared', SHARED_SPECS), ('core', CORE_SPECS)):
        for core in range(1 if scope == 'shared' else CORES):
            for index, key in enumerate(specs):
                ident, slot_core, flags, aux, value = SLOT.unpack_from(blob, slot_offset(scope, index, core))
                spec_id = key[0]
                if ident != spec_id or (scope == 'core' and slot_core != core):
                    raise ValueError('%s slot %d of core %d does not hold fact %#06x' % (scope, index, core, spec_id))
                state = 'skipped' if flags & FLAG_SKIPPED else 'measured' if flags & FLAG_VALID else 'missing'
                if state == 'missing':
                    raise ValueError('%s slot %d of core %d was never written' % (scope, index, core))
                for description in FACTS[key]:
                    if description.get('views') not in (None, layout):
                        continue
                    item = {k: description[k] for k in ('name', 'source', 'view')}
                    item['core'] = core
                    if description.get('note'):
                        item['note'] = description['note']
                    if state == 'skipped':
                        item.update(status='unmeasured', value=None, note='the probe skipped this read because its precondition was not met')
                    else:
                        width = description['width']
                        item.update(status='measured', value=(value >> description['lo']) & ((1 << width) - 1))
                        if description.get('rule'):
                            item['_rule'] = description['rule']
                            item['_intid'] = description.get('intid')
                    if ident in (0x0038, 0x0039, ID_PPI_WRITE):
                        item['written'] = aux
                    if flags & FLAG_ANOMALY:
                        item['note'] = 'the entry value did not read back after the restoring write'
                    out.append(item)
                    seen.add((item['name'], core))
    if layout == NS0:
        apply_view_visibility(out)
    for item in out:
        item.pop('_rule', None)
        item.pop('_intid', None)
    assert len(seen) == len(out)
    return header, out


def encode(platform=PLATFORM_A733, values=None, skipped=(), done=(1 << CORES) - 1):
    """A table the way the probe writes it. `values` maps (scope, core, spec) to a value or (value, aux, flags)."""
    values = values or {}
    blob = bytearray(TABLE_BYTES)
    HEADER.pack_into(blob, 0, MAGIC, VERSION, TABLE_BYTES, platform, CORES, SHARED_SLOTS, CORE_SLOTS, SHARED_OFFSET,
                     CORE_OFFSET, CORE_SLOTS * SLOT_BYTES, done)
    for scope, specs in (('shared', SHARED_SPECS), ('core', CORE_SPECS)):
        for core in range(1 if scope == 'shared' else CORES):
            for index, key in enumerate(specs):
                supplied = values.get((scope, core, key), 0)
                value, aux, flags = supplied if isinstance(supplied, tuple) else (supplied, 0, FLAG_VALID)
                if (scope, core, key) in skipped:
                    flags, value = FLAG_SKIPPED, 0
                SLOT.pack_into(blob, slot_offset(scope, index, core), key[0], core, flags, aux, value)
    return bytes(blob)


def expectation():
    """Board word expectation (sunstone-board-expect-u32-v1) for any complete table: the layout words and the
    slot identities are exact; the measured values are free."""
    words = []
    full = {'min': 0, 'max': 0xffffffff}
    for offset in range(0, TABLE_BYTES, 4):
        words.append(full)
    def exact(offset, value):
        words[offset // 4] = value
    words[0:2] = [MAGIC & 0xffffffff, MAGIC >> 32]
    words[2:11] = [VERSION, TABLE_BYTES, {'min': 1, 'max': 2}, CORES, SHARED_SLOTS, CORE_SLOTS, SHARED_OFFSET, CORE_OFFSET,
                   CORE_SLOTS * SLOT_BYTES]
    words[11] = (1 << CORES) - 1
    words[12:16] = [0, 0, 0, 0]
    for scope, specs in (('shared', SHARED_SPECS), ('core', CORE_SPECS)):
        for core in range(1 if scope == 'shared' else CORES):
            for index in range(CORE_SLOTS if scope == 'core' else SHARED_SLOTS):
                offset = slot_offset(scope, index, core)
                if index < len(specs):
                    ident, _ = specs[index]
                    # id (16 bits), core (8 bits) and flags (8 bits): flags may carry skipped or anomaly bits.
                    exact_low = ident | core << 16
                    words[offset // 4] = {'min': exact_low | 1 << 24, 'max': exact_low | 7 << 24}
                else:
                    words[offset // 4] = 0
                    words[offset // 4 + 1] = 0
                    words[offset // 4 + 2] = 0
                    words[offset // 4 + 3] = 0
    return {'schema': 'sunstone-board-expect-u32-v1', 'words': words}


def matches(blob, expected):
    """Whether a returned table satisfies a sunstone-board-expect-u32-v1 expectation."""
    if len(blob) != 4 * len(expected['words']):
        return False
    for word, want in zip(struct.unpack('<%dI' % len(expected['words']), blob), expected['words']):
        if isinstance(want, dict):
            if not want['min'] <= word <= want['max']:
                return False
        elif word != want:
            return False
    return True


# ---------------------------------------------------------------------------
# The facts file

FEL_VIEW = {'el': None, 'security': 'boot ROM FEL session before launch', 'interface': 'runner readl'}
NPU_ENTRY = '1793587317962103239-5bbfc48d'
# Values already seen on boards before the probe, each with the queue entry that saw it. The first two are
# what code or models asserted wrongly; the rest are the words and rails the runner reads.
SEEDS = [
    dict(name='gic.redist.intid27.priority', source='GICR_IPRIORITYR6 byte 3 (read at EL1 after the image wrote 0xd8)',
         view=view('redist'), core=0, status='observed', value=0xb0, written=0xd8, queue_entry=NPU_ENTRY,
         note='the shared timer (PPI 27); the image asserted 0xd8, the Non-secure view reads 0xb0; whether the write took effect is open'),
    dict(name='gic.cpuif.icc_pmr_el1.readback_after_write_ff', source='ICC_PMR_EL1 (read back after an unmasked write)',
         view=view('cpuif'), core='all', status='observed', value=0xf8, written=0xff, queue_entry=None,
         note='code asserted 0xff; first recorded in commit 55a35b4d; the queue entry that showed it is not recorded'),
]
for address, value in ((0x070101ac, 1), (0x020025c0, 285279231), (0x020025e0, 4043659267), (0x07064000, 8), (0x07064008, 8)):
    SEEDS.append(dict(name='runner.mmio.%#010x' % address, source='MMIO word %#010x (runner FEL readl, npu_on_admission)' % address,
                      view=FEL_VIEW, core=None, status='observed', value=value, queue_entry=NPU_ENTRY,
                      note='the runner reads these before launching an NPU image; the probe reads the same words at EL1 (mmio.*)'))
for name, value in (('dcdc3_uv', 800000), ('dcdc5_uv', 800000), ('dcxo_hz', 26000000), ('cpu_voltage_bin', 4)):
    SEEDS.append(dict(name='runner.rail.' + name, source='rail helper SUPPLY line (SID 0x496)', view=FEL_VIEW, core=None,
                      status='observed', value=value, queue_entry=NPU_ENTRY,
                      note='the rail helper reads the PMIC and the SID before an image with the CPU entry guard'))


TRAP_ENTRY = '1793590176547518274-e819ce89'
SEEDS.append(dict(name='gic.cpuif.icc_igrpen0_el1.read_traps_to_el3', source='ICC_IGRPEN0_EL1 (mrs at Non-secure EL1)',
                  view=view('cpuif'), core=4, status='measured', value=1, queue_entry=TRAP_ENTRY,
                  note='measured: the first EL3 exception was ESR 0x623c32b9 (EC 0x18, a trapped system register access) at the mrs of '
                       'ICC_IGRPEN0_EL1, ELR 0x4107c1c0, on core 4, and the monitor stopped. With SCR_EL3.FIQ set (FROM SOURCE: the monitor writes 0x535, platform/a733-smp-cached/continue.S:98-99, fact monitor.scr_el3.fiq) the '
                       'GIC architecture gives EL3 the Group 0 interface, so Non-secure EL1 accesses to ICC_IGRPEN0_EL1, ICC_BPR0_EL1, ICC_AP0Rn_EL1, '
                       'ICC_IAR0_EL1, ICC_EOIR0_EL1 and ICC_HPPIR0_EL1 trap to EL3 (IHI0069, the ICC_IGRPEN0_EL1 and ICC_BPR0_EL1 register descriptions; '
                       'Linux irq-gic-v3.c gic_has_group0).'))


# ---------------------------------------------------------------------------
# What our own source sets. The monitor, the route installer and the platform code configure the security state, the
# group, the trigger, the priority and the route of every interrupt the OS uses, so these values need no inference.

FROM_SOURCE = 'from-source'
ROUTES = (  # INTID, core, stored (Secure) priority, route-table file and line, images
    (71, 0, 0xd8, 'tools/sunstone-manifest.py:345', 'every OS image'),
    (187, 0, 0xb0, 'tools/sunstone-manifest.py:346', 'images with SUNSTONE_BOARD_USB2_IRQ'),
    (97, 1, 0xd8, 'tools/sunstone-manifest.py:347', 'NPU images (OS_NPU_CORE is 1, apps/os/npu.h:6)'),
    (143, 0, 0xd8, 'tools/sunstone-manifest.py:348-349', 'audio and DMA images'),
    (95, 0, 0xd8, 'tools/sunstone-manifest.py:350', 'GPU images (core 2 or the application core)'),
    (149, 0, 0xd8, 'apps/isp0/irq.h:26', 'ISP0 image'), (153, 0, 0xd8, 'apps/isp0/irq.h:26', 'ISP0 image'),
    (161, 0, 0xd8, 'apps/isp0/irq.h:26', 'ISP0 image'),
)
LOCAL_VISIBLE = tuple(range(16)) + (27, 30)  # SGIs and the two timer PPIs that source assigns to Non-secure Group 1
SOURCE_SCALARS = (
    ('monitor.scr_el3', 0x535, 'platform/a733-smp-cached/continue.S:98-99', 'SCR_EL3 the cached monitor writes before entering Non-secure EL1: NS, FIQ, HCE and RW set, with the RES1 bits 4 and 5 (platform/a733/monitor.c:124-125 writes 0x505 without them)'),
    ('monitor.scr_el3.ns', 1, 'platform/a733-smp-cached/continue.S:98-99', 'EL1 runs in the Non-secure state'),
    ('monitor.scr_el3.fiq', 1, 'platform/a733-smp-cached/continue.S:98-99', 'Group 0 interrupts go to EL3 as FIQ, so Non-secure EL1 accesses to the Group 0 CPU interface registers trap to EL3 (consistent with queue entry 1793590176547518274-e819ce89)'),
    ('monitor.gicd_ctlr.ds', 0, 'platform/a733-smp-cached/boot.c:95', 'the boot stops if DS is set: the GIC keeps two security states'),
    ('monitor.gicd_ctlr.are_s_are_ns', 3, 'platform/a733-smp-cached/boot.c:103', 'affinity routing enabled for both states'),
    ('monitor.gicd_ctlr.enable_grp0_grp1ns', 3, 'platform/a733-smp-cached/boot.c:104', 'Group 0 and Non-secure Group 1 enabled'),
)


def ns_view(stored):
    """The Non-secure view of a stored (Secure) priority with two security states and five priority bits."""
    return (stored << 1) & 0xff


def stored_from_ns(ns):
    """The stored priority that a Non-secure write of `ns` produces (IHI0069 4.8.1)."""
    return ((ns >> 1) | 0x80) & 0xf8


def source_facts():
    out = []
    for name, value, ref, text in SOURCE_SCALARS:
        out.append(dict(name=name, source='source: ' + ref, view=dict(el=3, security='secure', interface='monitor code'), core=None,
                        status=FROM_SOURCE, value=value, source_ref=ref, note=text))
    for intid in LOCAL_VISIBLE:
        for field, value, ref in (('group', 1, 'platform/a733/cpu_continue.c:115 (verified for SGIs and PPIs 29 and 30 at :126-127; PPI 27 at platform/a733-smp-cached/continue.c:120-121)'),
                                  ('group_modifier', 0, 'platform/a733/cpu_continue.c:116')):
            out.append(dict(name='config.intid%d.%s' % (intid, field), source='GICR_IGROUPR0/IGRPMODR0 bit %d as EL3 sets it' % intid,
                            view=dict(el=3, security='secure', interface='gic-redistributor-sgi-frame'), core='all', status=FROM_SOURCE,
                            value=value, source_ref=ref, note='Non-secure Group 1' if field == 'group' else 'Group 1 modifier clear'))
    for intid, field, value, ref in ((0, 'stored_priority', 0xf0, 'platform/a733/cpu_continue.c:120'),
                                     (27, 'trigger_level', 1, 'platform/a733-smp-cached/continue.c:122-124'),
                                     (30, 'trigger_level', 1, 'platform/a733/cpu_continue.c:118'),
                                     (30, 'stored_priority_written_ns', 0xe0, 'platform/a733-smp-cached/platform.c:116 (portLOWEST_USABLE_INTERRUPT_PRIORITY << 4, written at Non-secure EL1)'),
                                     (27, 'stored_priority_written_ns', 0xd8, 'platform/a733-smp-cached/platform.c:121 (written at Non-secure EL1 under SUNSTONE_TASK_DEADLINES)')):
        out.append(dict(name='config.intid%d.%s' % (intid, field), source='source: ' + ref, view=dict(el=3 if 'written_ns' not in field else 1,
                        security='secure' if 'written_ns' not in field else SECURITY, interface='gic-redistributor-sgi-frame'),
                        core='all', status=FROM_SOURCE, value=value, source_ref=ref,
                        note='the value the code writes; a Non-secure write of v stores ((v >> 1) | 0x80) & 0xf8' if 'written_ns' in field else
                             'the value the code sets'))
    for intid, core, stored, ref, images in ROUTES:
        base = 'config.spi%d.' % intid
        for field, value, where in (('group', 1, 'platform/a733-smp-cached/irq_table.c:70'), ('group_modifier', 0, 'platform/a733-smp-cached/irq_table.c:71'),
                                    ('trigger_level', 1, 'platform/a733-smp-cached/irq_table.c:72'),
                                    ('stored_priority', stored, ref + ' applied at platform/a733-smp-cached/irq_table.c:73-74'),
                                    ('route_core', core, ref + ' applied at platform/a733-smp-cached/irq_table.c:76'),
                                    ('ns_view_priority', ns_view(stored), 'derived: ns_view(stored_priority), IHI0069 4.8.1')):
            item = dict(name=base + field, source='source: ' + where, view=dict(el=3, security='secure', interface='gic-distributor'),
                        core=None, status=FROM_SOURCE, value=value, source_ref=where, note='route of %s' % images)
            if field == 'ns_view_priority':
                item.update(status='derived', derived_from=base + 'stored_priority', source='derived: the Non-secure view of the stored priority',
                            view=view('dist'), note='IHI0069 4.8.1: (stored << 1) & 0xff; INTID 97 measured 0xb0 for stored 0xd8; route of %s' % images)
                del item['source_ref']
            out.append(item)
    return out


def annotate(document):
    """Add the source facts and resolve the raz-in-view entries that our source configures."""
    document['facts'] = [e for e in document['facts'] if e['status'] not in (FROM_SOURCE, 'derived')] + source_facts()
    routed = {r[0]: r for r in ROUTES}
    # The hw-facts image routes 71, 95, 97 and 143 itself (tools/sunstone-manifest.py SUNSTONE_A733_HW_FACTS).
    image_routes = {71, 95, 97, 143}
    for item in document['facts']:
        if item['status'] != RAZ:
            continue
        parts = item['name'].split('.')
        if parts[:2] == ['gic', 'dist'] and parts[2].startswith('spi') and parts[3] in ('group1', 'group_modifier'):
            intid = int(parts[2][3:])
            if intid in image_routes:
                value = 1 if parts[3] == 'group1' else 0
                where = 'platform/a733-smp-cached/irq_table.c:%d' % (70 if value else 71)
                item.update(status=FROM_SOURCE, value=value, source_ref=where,
                            note='RAZ/WI at Non-secure EL1 (read_value 0); this image\'s route table sets %s' % ('Non-secure Group 1' if value else 'the group modifier clear'))
        elif parts[:2] == ['gic', 'redist'] and parts[2].startswith('intid') and parts[3] == 'priority':
            intid = int(parts[2][5:])
            if intid in LOCAL_VISIBLE:
                item.update(status='measured', value=item['read_value'], source_ref='platform/a733/cpu_continue.c:115',
                            note='visible: source sets this interrupt to Non-secure Group 1 (cpu_continue.c:115); our code writes only SGI 0 '
                                 '(cpu_continue.c:120), so this is the reset value of the stored priority, seen through the shifted view')
    return document


def seeded_document():
    document = empty_document()
    for seed in SEEDS:
        match = [e for e in document['facts'] if e['name'] == seed['name']]
        if match:
            document['facts'].remove(match[0])
        document['facts'].append(seed)
    order = {e['name']: i for i, e in enumerate(blank_facts())}
    document['facts'].sort(key=lambda e: (order.get(e['name'], len(order)), e['name']))
    return annotate(document)

def blank_facts():
    """Every fact name once, unmeasured; per-core facts use core "all"."""
    facts = []
    for scope, specs in (('shared', SHARED_SPECS), ('core', CORE_SPECS)):
        for key in specs:
            for description in FACTS[key]:
                if description.get('views') not in (None, NS0):
                    continue
                facts.append(dict(name=description['name'], source=description['source'], view=description['view'],
                                  core=None if scope == 'shared' else 'all', status='unmeasured', value=None))
    return facts


def empty_document():
    return {'schema': SCHEMA, 'security_state_note': 'the image runs at EL1 in the Non-secure state that the cached monitor enters '
            '(SCR_EL3.NS set at entry); SCR_EL3 is not readable at EL1, so the state is stated, not read back',
            'decoding': {'a733-board': 'GIC registers decoded in the ns-ds0 layout: Non-secure access with two security states (GICD_CTLR.DS clear). '
                         'DS is not visible to Non-secure software; that it is clear is FROM SOURCE (monitor.gicd_ctlr.ds: platform/a733-smp-cached/boot.c:95 '
                         'stops the boot when DS is set) and agrees with the shifted ICC_PMR_EL1 view. A field the view cannot see is recorded as '
                         'raz-in-view, not as measured 0.'},
            'facts': blank_facts()}


def load_document(path=FACTS_JSON):
    return json.loads(Path(path).read_text())


def merge(document, header, facts, queue_entry, image_sha256, port=None):
    """Replace unmeasured and observed entries by this run's measured ones. Facts read on the virtual board are refused."""
    if header['platform_id'] != PLATFORM_A733:
        raise ValueError('only a table read on the A733 board becomes hardware facts')
    index = {(e['name'], e['core']): e for e in document['facts']}
    for fact in facts:
        key = (fact['name'], fact['core'])
        item = dict(fact, queue_entry=queue_entry, image_sha256=image_sha256)
        if port:
            item['port'] = port
        index.pop((fact['name'], 'all'), None)
        index.pop((fact['name'], None), None)
        index[key] = item
    names = {}
    for (name, core), item in index.items():
        names.setdefault(name, []).append(item)
    ordered = []
    for blank in blank_facts():
        ordered.extend(sorted(names.pop(blank['name'], []), key=lambda e: (e['core'] == 'all', e['core'] if isinstance(e['core'], int) else -1)))
    for rest in names.values():
        ordered.extend(rest)
    document['facts'] = ordered
    return annotate(document)


# ---------------------------------------------------------------------------
# The loader that host models use

class MissingFact(KeyError):
    pass


class Facts:
    """Measured and observed values of docs/hardware/a733-facts.json.

    value() answers only from a measurement or an observation. assume() is the single way for a
    model to proceed on a fact that is not yet measured: it returns the stated default and records
    the name, so that the table check can list every remaining assumption."""

    def __init__(self, path=FACTS_JSON):
        self.document = load_document(path)
        assert self.document['schema'] == SCHEMA
        self.assumed = {}
        self._index = {}
        for item in self.document['facts']:
            if item['status'] in ('measured', 'observed', FROM_SOURCE, 'derived'):
                self._index.setdefault(item['name'], {})[item['core']] = item

    def entry(self, name, core=None):
        per_core = self._index.get(name, {})
        for candidate in (core, 'all', None, 0):
            if candidate in per_core:
                return per_core[candidate]
        return None

    def has(self, name, core=None):
        return self.entry(name, core) is not None

    def value(self, name, core=None):
        item = self.entry(name, core)
        if item is None:
            raise MissingFact(name)
        return item['value']

    def assume(self, name, default, reason, core=None):
        item = self.entry(name, core)
        if item is not None:
            return item['value']
        self.assumed[name] = (default, reason)
        return default


class ContradictsTable(ValueError):
    pass


PRIBITS = 'gic.cpuif.icc_ctlr_el1.pribits'
PMR_FF = 'gic.cpuif.icc_pmr_el1.readback_after_write_ff'
RPR_IDLE = 'gic.cpuif.icc_rpr_el1.idle'


class GicModel:
    """Priority and priority-mask behaviour of the A733 GIC that the facts table states.

    The number of implemented priority bits is ICC_CTLR_EL1.PRIbits + 1 when that is measured. The
    Non-secure EL1 view of ICC_PMR_EL1 and of Group 1 priority bytes is either the Secure value or
    the Secure value shifted left by one bit, when the GIC has two security states (Linux
    irq-gic-v3.c:95-115, IHI0069 4.8.1). The model takes the one of the two that reproduces the
    read-back of ICC_PMR_EL1 after an unmasked write, and fails with ContradictsTable when neither
    does. A Non-secure priority read of an interrupt whose read-back was observed after the image's
    own write returns the observed value. Anything the table does not hold is taken through
    Facts.assume, so that `facts.assumed` lists every remaining assumption."""

    def __init__(self, facts=None):
        self.facts = facts or Facts()
        bits = self.facts.assume(PRIBITS, 4, 'ICC_CTLR_EL1.PRIbits is unmeasured: five priority bits') + 1
        self.mask = (0xff00 >> bits) & 0xff
        shifted = (self.mask << 1) & 0xff
        readback = self.facts.assume(PMR_FF, shifted, 'ICC_PMR_EL1 read-back is unmeasured: shifted Non-secure view')
        if readback == self.mask:
            self.shifted = False
        elif readback == shifted:
            self.shifted = True
        else:
            raise ContradictsTable('ICC_PMR_EL1 reads back %#x after an unmasked write; the model has mask %#x' % (readback, self.mask))

    def secure_write(self, value):
        return value & self.mask

    def ns_write(self, value):
        """The stored (Secure) priority after a Non-secure write of `value`."""
        return (((value >> 1) | 0x80) if self.shifted else value) & self.mask

    def ns_read(self, stored):
        return (stored << 1) & 0xff if self.shifted else stored

    def pmr_ns_write(self, secure_pmr, value):
        """New Secure PMR after a Non-secure write; writes are ignored while the Secure mask is below 0x80."""
        return self.ns_write(value) if secure_pmr & 0x80 else secure_pmr

    def pmr_ns_read(self, secure_pmr):
        return self.ns_read(secure_pmr) if secure_pmr & 0x80 else 0

    def rpr_idle(self):
        return self.facts.assume(RPR_IDLE, 0xff, 'ICC_RPR_EL1 at idle is unmeasured')

    def priority_ns_read(self, intid, stored, core=None):
        """Non-secure read of the priority byte of `intid` after the image wrote it."""
        name = 'gic.redist.intid%d.priority' % intid if intid < 32 else 'gic.dist.spi%d.priority' % intid
        item = self.facts.entry(name, core)
        if item is not None and item['status'] == 'observed':
            return item['value']
        return self.ns_read(stored)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('gen')
    commands.add_parser('init')
    d = commands.add_parser('decode')
    d.add_argument('table', type=Path)
    d.add_argument('--merge', type=Path)
    d.add_argument('--queue-entry')
    d.add_argument('--image-sha256')
    d.add_argument('--port')
    e = commands.add_parser('expect')
    e.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.command == 'gen':
        TABLE_INC.write_text(table_inc())
    elif args.command == 'init':
        FACTS_JSON.parent.mkdir(exist_ok=True)
        FACTS_JSON.write_text(json.dumps(seeded_document(), indent=1) + '\n')
    elif args.command == 'expect':
        args.output.write_text(json.dumps(expectation()) + '\n')
    else:
        header, facts = decode(args.table.read_bytes())
        print(json.dumps(dict(header=header, facts=len(facts))))
        if args.merge:
            if not args.queue_entry or not args.image_sha256:
                sys.exit('--merge needs --queue-entry and --image-sha256')
            document = merge(load_document(args.merge), header, facts, args.queue_entry, args.image_sha256, args.port)
            args.merge.write_text(json.dumps(document, indent=1) + '\n')


if __name__ == '__main__':
    main()
