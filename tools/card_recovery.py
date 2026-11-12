"""Admit and verify the RAM-only first-stage RTC-to-FEL recovery proof."""
import hashlib
import io
from pathlib import Path
import secrets
import struct

from elftools.elf.elffile import ELFFile

PROFILE_MAGIC = 0x314c454644524143
REQUEST_MAGIC = 0x3151524644524143
REPORT_MAGIC = 0x3152454644524143
ENTRY, GATE, REQUEST, REPORT = 0x41000000, 0x41001000, 0x41002000, 0x42000000
RTC_FLAG, FEL_FLAG, GATE_COMPLETE = 0x07090108, 0x5aa5a55a, 0x46454c31
PROFILE = struct.Struct('<7Q32s')
REQUEST_RECORD, REPORT_RECORD = struct.Struct('<4Q'), struct.Struct('<8Q')


def require(ok, message):
    if not ok:
        raise ValueError(message)


def inspect(image):
    """Bind the executable, mutable request and report to one immutable gate identity."""
    data = Path(image).read_bytes()
    elf = ELFFile(io.BytesIO(data))
    require(elf['e_type'] == 'ET_EXEC' and elf['e_machine'] == 'EM_AARCH64' and elf['e_entry'] == ENTRY,
            'Card recovery requires an AArch64 executable at 0x41000000.')
    table = elf.get_section_by_name('.symtab')
    require(table is not None, 'Card recovery requires retained symbols.')
    symbols = {}
    for symbol in table.iter_symbols():
        if symbol['st_shndx'] != 'SHN_UNDEF' and symbol.name.startswith('a733_card_fel_'):
            require(symbol.name not in symbols, 'Duplicate card recovery symbol.')
            symbols[symbol.name] = symbol
    loads = [p for p in elf.iter_segments() if p['p_type'] == 'PT_LOAD' and p['p_memsz']]
    uploads, occupied = [], []
    for segment in loads:
        low, size = int(segment['p_vaddr']), int(segment['p_memsz'])
        high = low + size
        require(segment['p_paddr'] == low and segment['p_filesz'] <= size and segment['p_flags'] in (4, 5, 6),
                'Card recovery load flags or physical address differ.')
        require((ENTRY <= low < high <= 0x41f00000) or
                (low == REPORT and size == REPORT_RECORD.size and not segment['p_filesz'] and segment['p_flags'] == 6),
                'Card recovery load leaves its RAM reservations.')
        require(all(high <= begin or end <= low for begin, end in occupied), 'Card recovery loads overlap.')
        occupied.append((low, high))
        value = segment.data()
        require(len(value) == segment['p_filesz'], 'Truncated card recovery load.')
        if value:
            uploads.append((low, value))
    require(any(p['p_flags'] == 5 and p['p_vaddr'] <= ENTRY and ENTRY + 4 <= p['p_vaddr'] + p['p_filesz']
                for p in loads), 'Card recovery entry lacks initialized executable bytes.')

    def object_at(name, address, size, writable, initialized=True):
        symbol = symbols.get(name)
        require(symbol is not None and symbol['st_info']['bind'] == 'STB_GLOBAL' and
                symbol['st_info']['type'] == 'STT_OBJECT' and type(symbol['st_shndx']) is int and
                symbol['st_size'] == size and (address is None or symbol['st_value'] == address),
                'Card recovery object differs: ' + name)
        address = int(symbol['st_value'])
        section = elf.get_section(symbol['st_shndx'])
        require(section['sh_flags'] & 2 and bool(section['sh_flags'] & 1) == writable and
                (not writable or not section['sh_flags'] & 4) and section['sh_addr'] <= address and
                address + size <= section['sh_addr'] + section['sh_size'],
                'Card recovery object allocation differs: ' + name)
        if initialized:
            require(section['sh_type'] == 'SHT_PROGBITS', 'Card recovery object must have file bytes: ' + name)
        else:
            require(section['sh_type'] in ('SHT_PROGBITS', 'SHT_NOBITS'), 'Card recovery object section differs: ' + name)
        require(any(p['p_flags'] == (6 if writable else 4) and p['p_vaddr'] <= address and
                    address + size <= p['p_vaddr'] + (p['p_filesz'] if initialized else p['p_memsz'])
                    for p in loads) or (not writable and any(p['p_flags'] == 5 and p['p_vaddr'] <= address and
                    address + size <= p['p_vaddr'] + p['p_filesz'] for p in loads)),
                'Card recovery object lacks matching load coverage: ' + name)
        offset = address - section['sh_addr']
        value = section.data()[offset:offset + size] if initialized else b''
        if initialized:
            require(any(p['p_vaddr'] <= address and address + size <= p['p_vaddr'] + p['p_filesz'] and
                        p.data()[address - p['p_vaddr']:address - p['p_vaddr'] + size] == value for p in loads),
                    'Card recovery section and uploaded bytes differ: ' + name)
        return symbol, section, value

    _, _, marker = object_at('a733_card_fel_profile', None, PROFILE.size, False)
    magic, version, size, gate, length, state, test, digest = PROFILE.unpack(marker)
    require((magic, version, size, gate) == (PROFILE_MAGIC, 1, PROFILE.size, GATE) and 8 <= length <= 512,
            'Unsupported card recovery profile or gate extent.')
    require(state % 4 == 0 and test % 4 == 0 and state != test and state + 4 <= length and test + 4 <= length,
            'Card recovery gate state/test offsets differ.')
    gates = [s for s in elf.iter_sections() if s.name == '.card_gate']
    require(len(gates) == 1, 'Card recovery requires one .card_gate section.')
    gate_section = gates[0]
    require(gate_section['sh_type'] == 'SHT_PROGBITS' and gate_section['sh_flags'] & 7 == 6 and
            (gate_section['sh_addr'], gate_section['sh_size']) == (GATE, length) and
            any(p['p_flags'] == 5 and p['p_vaddr'] <= GATE and GATE + length <= p['p_vaddr'] + p['p_filesz'] for p in loads),
            'Card recovery gate lacks exact immutable executable coverage.')
    gate_bytes = gate_section.data()
    require(any(p['p_vaddr'] <= GATE and GATE + length <= p['p_vaddr'] + p['p_filesz'] and
                p.data()[GATE - p['p_vaddr']:GATE - p['p_vaddr'] + length] == gate_bytes for p in loads),
            'Card recovery gate section and uploaded bytes differ.')
    require(not any(gate_bytes[state:state + 4] + gate_bytes[test:test + 4]) and
            hashlib.sha256(gate_bytes).digest() == digest, 'Card recovery gate identity differs.')
    object_at('a733_card_fel_request', REQUEST, REQUEST_RECORD.size, True, initialized=False)
    _, report_section, _ = object_at('a733_card_fel_report', REPORT, REPORT_RECORD.size, True, initialized=False)
    require(report_section['sh_type'] == 'SHT_NOBITS' and
            (report_section['sh_addr'], report_section['sh_size']) == (REPORT, REPORT_RECORD.size),
            'Card recovery report must be the exact NOLOAD reservation.')
    return {'profile': {'gate_address': GATE, 'gate_bytes': length, 'state_offset': state,
                        'test_offset': test, 'gate_sha256': digest.hex()}, 'uploads': uploads,
            'watchdog': {'mode': 0x41, 'session_seconds': 60}}


def prepare(info, inputs, out):
    require(not inputs, 'Card recovery takes only its generated nonce request.')
    out.mkdir(parents=True, exist_ok=False)
    nonce = secrets.randbits(62) | 1
    uploads = []
    for index, (address, data) in enumerate(info['uploads']):
        path = out / ('segment-%d.bin' % index)
        path.write_bytes(data)
        uploads.append((address, path))
    for name, address, data in (
            ('request.bin', REQUEST, REQUEST_RECORD.pack(REQUEST_MAGIC, 1, nonce, 0x41)),
            ('report-zero.bin', REPORT, bytes(REPORT_RECORD.size))):
        path = out / name
        path.write_bytes(data)
        uploads.append((address, path))
    return {'epoch': nonce, 'generation': nonce, 'nonce': nonce, 'format': 'card-fel-recovery',
            'profile': info['profile'], 'watchdog': info['watchdog'],
            'uploads': uploads, 'launch': ['reset64', hex(ENTRY)]}


def arm(owner):
    """The locked runner sets the retained flag after WDT0 has been armed."""
    require(owner.armed, 'Card recovery RTC flag requires the armed watchdog.')
    value = int(owner.tx('writel', hex(RTC_FLAG), hex(FEL_FLAG), 'readl', hex(RTC_FLAG)), 16)
    require(value == FEL_FLAG, 'Card recovery RTC flag arm readback differs.')
    owner.record['card_recovery_flag'] = {'armed': True, 'readback': value}
    owner.journal()


def capture(owner, plan):
    require(owner.record.get('normal_return') and not owner.record.get('dram_lost'),
            'Card recovery receipt requires a verified normal ROM return.')
    report = owner.read(REPORT, REPORT_RECORD.size, owner.evidence / 'card-recovery-report.bin')
    require(len(report) == REPORT_RECORD.size, 'Truncated card recovery report.')
    fields = REPORT_RECORD.unpack(report)
    require(fields == (REPORT_MAGIC, 1, REPORT_RECORD.size, plan['nonce'], 1, FEL_FLAG, 0x41, 12),
            'Card recovery report identity or entry conditions differ.')
    profile = plan['profile']
    gate = bytearray(owner.read(profile['gate_address'], profile['gate_bytes'], owner.evidence / 'card-recovery-gate.bin'))
    require(len(gate) == profile['gate_bytes'], 'Truncated card recovery gate.')
    state = struct.unpack_from('<I', gate, profile['state_offset'])[0]
    test = struct.unpack_from('<I', gate, profile['test_offset'])[0]
    require(state == GATE_COMPLETE and test == 1, 'Card recovery gate did not complete the test path.')
    for offset in (profile['state_offset'], profile['test_offset']):
        struct.pack_into('<I', gate, offset, 0)
    require(hashlib.sha256(gate).hexdigest() == profile['gate_sha256'], 'Returned card recovery gate changed.')
    flag = int(owner.tx('readl', hex(RTC_FLAG)), 16)
    require(flag == 0, 'Card recovery gate did not clear the retained flag.')
    decoded = dict(zip(('magic', 'version', 'bytes', 'nonce', 'status', 'entry_flag', 'entry_mode', 'current_el'), fields))
    decoded.update(gate_state=state, gate_test=test, gate_sha256=profile['gate_sha256'], rtc_flag=flag)
    owner.record['card_recovery_validated'] = True
    owner.journal()
    return {'valid': True, 'payload': b'First-stage RTC flag selected FEL.\n', 'report': decoded}
