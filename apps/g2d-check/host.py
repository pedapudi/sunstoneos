#!/usr/bin/env python3
"""Host side of g2d-check: the decoder of its 432-byte result.

tools/board.py loads this module with --decoder or --decode-application. decode() reads the result that ss_finish
returns (struct g2d_check_result in apps/g2d-check/main.c, which embeds struct ss_g2d_info and struct ss_g2d_closed
from include/sunstone_g2d.h) and names every recorded word. Stage 1 takes no input.
"""
import struct

PLATFORM = ('pll_ref', 'peri0', 'peri0_gates', 'peri0_status', 'g2d_clk', 'g2d_bgr', 'de_sys_bgr', 'ahb_gates',
            'mbus_gates', 'rccu_pck', 'pck1_policy', 'pck1_status', 'pck1_delay', 'iommu0', 'iommu1', 'rtc_osc',
            'de_clk', 'de_bgr', 'di_clk', 'di_bgr', 'gicd_enable', 'gicd_pending', 'gicd_active')
LOCAL = ('sclk_gate', 'hclk_gate', 'ahb_reset', 'sclk_div', 'version', 'mclk_gate', 'rcq_irq', 'rcq_status',
         'rcq_ctrl', 'mixer_ctrl', 'mixer_irq')
SAMPLES = 8
HEAD = struct.Struct('<8I')
INFO = struct.Struct(f'<4I{len(PLATFORM)}I{len(PLATFORM)}I{len(LOCAL)}I4x3Q3Q2I{SAMPLES}I{SAMPLES}I')
CLOSED = struct.Struct('<6IQ')
SIZE = HEAD.size + INFO.size + CLOSED.size
CHECK_MAGIC, INFO_MAGIC = 0x4b433247, 0x49443247
STATUS = {1: 'opened', 2: 'refused', 3: 'close failed'}
TICKS_PER_US = 24  # the system counter runs from the 24 MHz oscillator
INTID_BIT = 1 << (83 % 32)


def words(names, values):
    return {name: f'0x{value:08x}' for name, value in zip(names, values)}


def decode(payload):
    """A dict of the stage-1 record; ValueError if the payload is malformed."""
    if len(payload) != SIZE:
        raise ValueError(f'g2d-check result is {len(payload)} bytes, not {SIZE}.')
    magic, stage, status, open_result, close_result, fault_step, fault_value, reserved = HEAD.unpack_from(payload)
    if magic != CHECK_MAGIC or reserved:
        raise ValueError('g2d-check result has a wrong magic or a nonzero reserved word.')
    v = INFO.unpack_from(payload, HEAD.size)
    p = len(PLATFORM)
    info_magic, info_bytes, domain_entry, module_hz = v[:4]
    entry, after = v[4:4 + p], v[4 + p:4 + 2 * p]
    local = v[4 + 2 * p:4 + 2 * p + len(LOCAL)]
    rest = v[4 + 2 * p + len(LOCAL):]
    requested, domain_on, opened, row_request_us, pck_command_us, row_on_us, bit8, reads = rest[:8]
    status_word, status_ticks = rest[8:8 + SAMPLES], rest[8 + SAMPLES:]
    c = CLOSED.unpack_from(payload, HEAD.size + INFO.size)
    signed = lambda x: x - (1 << 32) if x & 0x80000000 else x
    record = {
        'stage': stage, 'status': STATUS.get(status, status),
        'open_result': signed(open_result), 'close_result': signed(close_result),
        'fault_step': fault_step, 'fault_value': f'0x{fault_value:08x}',
    }
    if info_magic != INFO_MAGIC:
        # A refusal before the owner took the request leaves no open record.
        record['info'] = None
        return record
    if info_bytes != INFO.size:
        raise ValueError(f'ss_g2d_info is {info_bytes} bytes, not {INFO.size}.')
    peri = entry[PLATFORM.index('peri0')]
    version = local[LOCAL.index('version')]
    record['info'] = {
        'domain_entry': domain_entry, 'module_hz': module_hz,
        'entry': words(PLATFORM, entry), 'after': words(PLATFORM, after), 'local': words(LOCAL, local),
        'peri0_2x_mhz': 24 * (((peri >> 8) & 255) + 1) // ((peri >> 20 & 7) + 1) if peri >> 31 else 0,
        'intid83_entry': {'enabled': bool(entry[PLATFORM.index('gicd_enable')] & INTID_BIT),
                          'pending': bool(entry[PLATFORM.index('gicd_pending')] & INTID_BIT),
                          'active': bool(entry[PLATFORM.index('gicd_active')] & INTID_BIT)},
        'version': {'ip_version': f'0x{version >> 16:04x}', 'low_fields': f'0x{version & 0xffff:04x}'},
        'open_us': (opened - requested) / TICKS_PER_US if opened else None,
        'domain_on_us': (domain_on - requested) / TICKS_PER_US if domain_on else None,
        'owner_clock_us': {'row_request': row_request_us, 'pck_command': pck_command_us, 'row_on': row_on_us,
                           'adopted': pck_command_us == 0},
        'pck_bit8': {'entry_policy': bit8 & 1, 'entry_status': bit8 >> 1 & 1,
                     'after_policy': bit8 >> 8 & 1, 'after_status': bit8 >> 9 & 1},
        'status_reads': reads,
        'status_samples': [{'word': f'0x{w:08x}', 'us': t / TICKS_PER_US}
                           for w, t in list(zip(status_word, status_ticks))[:min(reads, SAMPLES)]],
    }
    record['closed'] = {'g2d_clk': f'0x{c[0]:08x}', 'g2d_bgr': f'0x{c[1]:08x}', 'de_sys_bgr': f'0x{c[2]:08x}',
                        'pck1_status': f'0x{c[3]:08x}', 'de_di_open': c[4],
                        'closed_us': (c[6] - requested) / TICKS_PER_US if c[6] else None}
    return record
