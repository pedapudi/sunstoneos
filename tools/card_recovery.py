"""Admit the immutable ARM32 SRAM gate and verify its retained RTC receipt."""
import hashlib
import io
import os
from pathlib import Path
import secrets
import struct

from elftools.elf.elffile import ELFFile

PROFILE_MAGIC = 0x314c454644524143
ENTRY = GATE = 0x52000
PROFILE_ADDRESS, SRAM_END = 0x52400, 0x52d80
RTC_TAG, RTC_LAST, RTC_FLAG = 0x07090100, 0x07090104, 0x07090108
FEL_FLAG, RTC_STAGE, FINAL_VALUE = 0x5aa5a55a, 0x0c02, 5
# The cold-boot marker and the gate record (include/a733_card_fel_gate.h).
RTC_MARKER, MARKER_SET, RECORD_WARM = 0x070901f0, 0x16aa, 0x4000
PROFILE = struct.Struct('<9Q32s')
PROFILE_VERSION = 4
# The proof's entry state: the vendor FEL request, or no request with the
# cold-boot marker set, which a watchdog return through the card presents.
ENTRY_REQUEST, ENTRY_WARM = 1, 2
GATE_LIMIT = 1020  # gate code plus its policy word fill at most two appended sectors
# The proof's WDT0 policy word: the gate re-arms WDT0 at 16 s, the largest
# interval (A733 manual 8.3.6.6), instead of disarming it. A ROM that does not
# re-enumerate therefore still resets to FEL. include/a733_card_fel_gate.h.
GATE_WATCHDOG = 0xb1
# The ROM reported the execute status before running the gate (0.013 s on the
# board); if the disconnect loses it, end that transaction after 2 s. Gate entry to capture: exe end 2 + enumeration 5 + capture 1 = 8 s
# of the 16 s interval (docs/designs/card-fel-recovery.md).
EXE_SECONDS = 2
INTERVAL_SECONDS = 16
# This patched tool disables and invalidates the A733 I-cache before MMIO helpers.
FEL_SHA256 = '4a54cdc179baafbdecac47eb85714c3745d6d9952e05b482c7016afd4b22bf3e'
FEL_ICACHE_PATCH = 'patches/sunxi-a733-sram.patch'


def require(ok, message):
    if not ok:
        raise ValueError(message)


def fel_bytes(path):
    data = Path(path).read_bytes()
    require(hashlib.sha256(data).hexdigest() == FEL_SHA256,
            'Card recovery requires the reviewed FEL binary with the A733 I-cache fix.')
    return data


def freeze_fel(path, destination):
    data = fel_bytes(path)
    with Path(destination).open('xb') as stream:
        stream.write(data)
        stream.flush()
        os.fchmod(stream.fileno(), 0o500)
        os.fsync(stream.fileno())
    return {'sha256': FEL_SHA256, 'icache_patch': FEL_ICACHE_PATCH}


def inspect(image):
    """Bind the direct ARM32 entry and readonly profile to their uploaded bytes."""
    data = Path(image).read_bytes()
    elf = ELFFile(io.BytesIO(data))
    require(elf.elfclass == 32 and elf.little_endian and elf['e_type'] == 'ET_EXEC' and
            elf['e_machine'] == 'EM_ARM' and elf['e_entry'] == ENTRY,
            'Card recovery requires a little-endian ARM32 executable at 0x52000.')
    table = elf.get_section_by_name('.symtab')
    require(table is not None, 'Card recovery requires retained symbols.')
    symbols = {}
    for symbol in table.iter_symbols():
        if symbol['st_shndx'] != 'SHN_UNDEF' and symbol.name.startswith('a733_card_fel_'):
            require(symbol.name not in symbols, 'Duplicate card recovery symbol.')
            symbols[symbol.name] = symbol
    require(not {'a733_card_fel_request', 'a733_card_fel_report'} & symbols.keys(),
            'Card recovery has no mutable RAM request or report.')
    loads = [p for p in elf.iter_segments() if p['p_type'] == 'PT_LOAD' and p['p_memsz']]
    uploads, occupied = [], []
    for segment in loads:
        low, size = int(segment['p_vaddr']), int(segment['p_memsz'])
        high = low + size
        require(segment['p_paddr'] == low and segment['p_filesz'] == size and segment['p_flags'] in (4, 5),
                'Card recovery loads must be immutable initialized SRAM.')
        require(GATE <= low < high <= SRAM_END, 'Card recovery load leaves its SRAM reservation.')
        require(all(high <= begin or end <= low for begin, end in occupied), 'Card recovery loads overlap.')
        occupied.append((low, high))
        value = segment.data()
        require(len(value) == size, 'Truncated card recovery load.')
        uploads.append((low, value))

    def object_at(name, address, size=None, executable=False):
        symbol = symbols.get(name)
        require(symbol is not None and symbol['st_info']['bind'] == 'STB_GLOBAL' and
                symbol['st_info']['type'] == 'STT_OBJECT' and type(symbol['st_shndx']) is int and
                symbol['st_value'] == address and (size is None or symbol['st_size'] == size),
                'Card recovery object differs: ' + name)
        size = int(symbol['st_size'])
        section = elf.get_section(symbol['st_shndx'])
        require(section['sh_type'] == 'SHT_PROGBITS' and section['sh_flags'] & 3 == 2 and
                bool(section['sh_flags'] & 4) == executable and section['sh_addr'] <= address and
                address + size <= section['sh_addr'] + section['sh_size'],
                'Card recovery object allocation differs: ' + name)
        value = section.data()[address - section['sh_addr']:address - section['sh_addr'] + size]
        require(any(p['p_flags'] == (5 if executable else 4) and p['p_vaddr'] <= address and
                    address + size <= p['p_vaddr'] + p['p_filesz'] and
                    p.data()[address - p['p_vaddr']:address - p['p_vaddr'] + size] == value for p in loads),
                'Card recovery object lacks matching load bytes: ' + name)
        return section, value

    _, marker = object_at('a733_card_fel_profile', PROFILE_ADDRESS, PROFILE.size)
    magic, version, size, gate, length, stage, final, watchdog, entry, digest = PROFILE.unpack(marker)
    require((magic, version, size, gate, stage, final, watchdog) ==
            (PROFILE_MAGIC, PROFILE_VERSION, PROFILE.size, GATE, RTC_STAGE, FINAL_VALUE, GATE_WATCHDOG) and
            entry in (ENTRY_REQUEST, ENTRY_WARM) and 8 <= length <= GATE_LIMIT and length % 4 == 0,
            'Unsupported card recovery profile, gate extent, entry or WDT0 policy.')
    section, gate_bytes = object_at('a733_card_fel_gate_image', GATE, length, executable=True)
    _, policy = object_at('a733_card_fel_gate_watchdog', GATE + length, 4, executable=True)
    require((section['sh_addr'], section['sh_size']) == (GATE, length + 4) and
            struct.unpack('<I', policy)[0] == watchdog,
            'Card recovery gate requires its exact executable section and the profile WDT0 policy word.')
    require(hashlib.sha256(gate_bytes).digest() == digest, 'Card recovery gate identity differs.')
    return {'profile': {'version': PROFILE_VERSION, 'gate_address': GATE, 'gate_bytes': length, 'gate_watchdog': watchdog,
                        'rtc_stage': stage, 'final_value': final, 'entry': entry, 'gate_sha256': digest.hex()},
            'uploads': uploads, 'watchdog': {'mode': 0x41, 'gate_mode': watchdog, 'session_seconds': 60}}


def prepare(info, inputs, out):
    require(not inputs, 'Card recovery takes no external inputs.')
    out.mkdir(parents=True, exist_ok=False)
    nonce = secrets.randbits(62) | 1
    uploads = []
    for index, (address, data) in enumerate(info['uploads']):
        path = out / ('segment-%d.bin' % index)
        path.write_bytes(data)
        uploads.append((address, path))
    return {'epoch': nonce, 'generation': nonce, 'nonce': nonce, 'format': 'card-fel-recovery',
            'profile': info['profile'], 'watchdog': info['watchdog'],
            'uploads': uploads, 'launch': ['exe', hex(ENTRY)], 'launch_seconds': EXE_SECONDS}


def arm(owner, plan):
    """Set the session tag and the entry state after the locked runner arms WDT0:
    the retained request, or no request with the cold-boot marker set."""
    require(owner.armed, 'Card recovery RTC request requires the armed watchdog.')
    tag = 0x5c0d0000 | (plan['epoch'] & 0xffff)
    flag = FEL_FLAG if plan['profile']['entry'] == ENTRY_REQUEST else 0
    values = [int(value, 16) for value in owner.tx('writel', hex(RTC_TAG), hex(tag),
              'writel', hex(RTC_FLAG), hex(flag), 'readl', hex(RTC_TAG), 'readl', hex(RTC_FLAG),
              'readl', hex(RTC_MARKER)).split()]
    require(values[:2] == [tag, flag], 'Card recovery RTC tag or request arm readback differs.')
    require(plan['profile']['entry'] == ENTRY_REQUEST or values[2] & 0xffff == MARKER_SET,
            'The warm-entry proof requires the cold-boot marker the runner set at first contact.')
    owner.record['card_recovery_flag'] = {'armed': True, 'tag': tag, 'readback': values[1], 'marker': values[2],
                                          'entry': plan['profile']['entry']}
    owner.journal()


def receipt(owner, launched):
    """Read the retained request on every fresh return, including a watchdog failure,
    and record how long after the execute request the new instance appeared."""
    seconds = round(owner.now() - launched, 3)
    flag = int(owner.tx('readl', hex(RTC_FLAG)), 16)
    owner.record['card_recovery_return'] = {'rtc_flag': flag, 'seconds_after_exe': seconds}
    owner.journal()


def gate_return(plan, before, seconds):
    """True when WDT0 still holds the gate's re-armed mode and the instance appeared
    inside its interval: the ROM re-enumerated. A WDT0 reset leaves mode 1."""
    return before == plan['watchdog']['gate_mode'] and seconds < INTERVAL_SECONDS


def expected_flag(plan):
    """GP2 after the gate: the request path clears it; the warm path stores its
    record with the WDT0 mode the runner armed before the execute request."""
    if plan['profile']['entry'] == ENTRY_REQUEST:
        return 0
    return RTC_STAGE << 16 | RECORD_WARM | plan['watchdog']['mode'] & 0xff


def expected_last(plan):
    """The request path ends its progress in LAST; the warm path leaves the
    value the runner's arm cleared."""
    return RTC_STAGE << 16 | FINAL_VALUE if plan['profile']['entry'] == ENTRY_REQUEST else 0


def accepted(plan, record):
    """The retained receipt of one proof: WDT0 still held the gate's mode, the
    disarm read back clear and the RTC words match the entry's outcome."""
    returned = record['watchdog_return']
    words = returned['words']
    marker = returned.get('reset_words', {}).get('marker', 0)
    return (gate_return(plan, returned['before'], record['card_recovery_return']['seconds_after_exe']) and
            returned['after'] == 0 and words['tag'] == (0x5c0d0000 | (plan['epoch'] & 0xffff)) and
            words['last'] == expected_last(plan) and not words['fault'] and
            record['card_recovery_return']['rtc_flag'] == expected_flag(plan) and
            (plan['profile']['entry'] == ENTRY_REQUEST or marker & 0xffff == MARKER_SET))


def capture(owner, plan):
    require(owner.record.get('normal_return') and not owner.record.get('dram_lost'),
            'Card recovery receipt requires a verified normal ROM return.')
    returned = owner.record['watchdog_return']
    flag = owner.record['card_recovery_return']['rtc_flag']
    require(accepted(plan, owner.record) and not owner.record.get('cleanup_failure'),
            'Card recovery retained receipt differs.')
    owner.record['card_recovery_validated'] = True
    owner.journal()
    payload = (b'ARM32 SRAM gate consumed the RTC request and selected FEL.\n'
               if plan['profile']['entry'] == ENTRY_REQUEST else
               b'ARM32 SRAM gate read the cold-boot marker, recorded the warm entry and selected FEL.\n')
    return {'valid': True, 'payload': payload,
            'report': {'epoch': plan['epoch'], 'entry': plan['profile']['entry'], 'rtc_flag': flag,
                       'stage': RTC_STAGE, 'value': FINAL_VALUE,
                       'watchdog_before': returned['before'], 'watchdog_after': returned['after'],
                       'seconds_after_exe': owner.record['card_recovery_return']['seconds_after_exe'],
                       'gate_sha256': plan['profile']['gate_sha256']}}
