"""Read the existing immutable thermal window after a successful native return.

The defining compilation unit and DWARF bind local objects to the exact ELF.
Capture both objects before restoring uploaded RAM. This adds no native work.
"""
import importlib.util
from pathlib import Path
import struct

from elftools.elf.elffile import ELFFile
from elftools.dwarf.dwarf_expr import DWARFExprParser

spec = importlib.util.spec_from_file_location('return_thermal_report', Path(__file__).with_name('thermal-report.py'))
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)
require = report.require


def geometry(image):
    """Resolve the native producer's actual object layouts; reject ambiguity."""
    with Path(image).open('rb') as stream:
        elf = ELFFile(stream)
        symbols = elf.get_section_by_name('.symtab')
        require(symbols is not None and elf.has_dwarf_info(), 'Thermal capture requires symbols and DWARF.')
        objects = {}
        dwarf = elf.get_dwarf_info()
        for cu in dwarf.iter_CUs():
            name = cu.get_top_DIE().attributes.get('DW_AT_name')
            if not name or Path(name.value.decode()).name not in ('return-thermal.c', 'thermal.c'):
                continue
            for die in cu.iter_DIEs():
                attr = die.attributes.get('DW_AT_name')
                if die.tag != 'DW_TAG_variable' or not attr or attr.value not in (b'window', b'readings'):
                    continue
                label = attr.value.decode()
                location = die.attributes.get('DW_AT_location')
                require(location is not None and location.form == 'DW_FORM_exprloc',
                        'Thermal object has no fixed DWARF address.')
                ops = DWARFExprParser(dwarf.structs).parse_expr(location.value)
                require(len(ops) == 1 and ops[0].op_name in ('DW_OP_addr', 'DW_OP_addrx'),
                        'Thermal object location is not a fixed address.')
                address = (dwarf.get_addr(cu, ops[0].args[0]) if ops[0].op_name == 'DW_OP_addrx'
                           else ops[0].args[0])
                matches = [s for s in symbols.iter_symbols() if s.name == label and
                           s['st_value'] == address and s['st_shndx'] != 'SHN_UNDEF']
                require(label not in objects and len(matches) == 1, 'Ambiguous thermal object.')
                dtype = die.get_DIE_from_attribute('DW_AT_type')
                members = {}
                for member in dtype.iter_children():
                    if member.tag != 'DW_TAG_member':
                        continue
                    offset = member.attributes['DW_AT_data_member_location'].value
                    require(isinstance(offset, int), 'Unsupported thermal member location.')
                    members[member.attributes['DW_AT_name'].value.decode()] = offset
                size = dtype.attributes['DW_AT_byte_size'].value
                require(size == matches[0]['st_size'] and 0x41200000 <= address < address + size <= 0x42000000,
                        'Thermal object extent differs.')
                objects[label] = dict(address=address, bytes=size, members=members)
        require(set(objects) == {'window', 'readings'}, 'Thermal producer objects absent.')
    w, r = objects['window'], objects['readings']
    require(set(w['members']) == {'header', 'first', 'collecting', 'ready'} and
            set(r['members']) == {'rows', 'count', 'overflow', 'next'}, 'Thermal producer fields differ.')
    require(w['members']['header'] == 0 and w['members']['first'] in (report.LEGACY_HEADER_BYTES, report.HEADER_BYTES) and
            r['members']['rows'] == 0 and r['members']['count'] % report.ROW_BYTES == 0 and
            report.lifetime.thermal_rows(1) <= r['members']['count'] // report.ROW_BYTES <= report.MAX_ROWS and
            all(0 <= w['members'][k] <= w['bytes'] - 4 for k in ('first', 'collecting', 'ready')) and
            all(0 <= r['members'][k] <= r['bytes'] - 4 for k in ('count', 'overflow')),
            'Thermal producer geometry differs from report decoder.')
    return objects


def captures(layout):
    return [('thermal-' + name, layout[name]['address'], layout[name]['bytes'])
            for name in ('window', 'readings')]


def decode(layout, copied, epoch, generation):
    """Reconstruct the existing report; malformed capture is a host failure.

    Completeness remains evidence, independent of ROM return and RAM cleanup.
    """
    w, r = (copied['thermal-' + name] for name in ('window', 'readings'))
    require(len(w) == layout['window']['bytes'] and len(r) == layout['readings']['bytes'],
            'Short thermal object capture.')
    def word(raw, name, field):
        return struct.unpack_from('<I', raw, layout[name]['members'][field])[0]
    require(word(w, 'window', 'ready') == 1 and word(w, 'window', 'collecting') == 1,
            'Thermal window was not published.')
    first, count = word(w, 'window', 'first'), word(r, 'readings', 'count')
    header_bytes = layout['window']['members']['first']
    h = report.header(w[:header_bytes], epoch, generation)
    capacity = layout['readings']['members']['count'] // report.ROW_BYTES
    require(first <= count <= capacity and h['rows'] <= count - first and
            word(r, 'readings', 'overflow') in (0, 1), 'Thermal snapshot bounds differ.')
    raw = w[:header_bytes] + r[first * report.ROW_BYTES:(first + h['rows']) * report.ROW_BYTES]
    decoded = report.decode(raw, epoch, generation)
    decoded['classification'] = report.classify(decoded)
    return decoded
