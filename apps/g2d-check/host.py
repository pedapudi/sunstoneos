#!/usr/bin/env python3
"""Host side of g2d-check: the decoder of its 712-byte result and the reference of its fill.

tools/board.py loads this module with --decoder or --decode-application. decode() reads the result that ss_finish
returns (struct g2d_check_result in apps/g2d-check/main.c, which embeds struct ss_g2d_info, struct ss_g2d_closed and
struct ss_g2d_done from include/sunstone_g2d.h), names every recorded word, and compares the board's CRC-32 of the
surface with expected(), which rebuilds the surface from the recorded seed as main.c does: the background sequence,
then the fill rectangle in the color. The check takes no input beyond the seed compiled into main.c.

The prediction is that the fill writes the color's own alpha byte. The decoder also reports whether the board's
CRC-32 equals the surface with the rectangle's alpha byte forced to 0xff, which identifies that one failure.
"""
import struct
import zlib

PLATFORM = ('pll_ref', 'peri0', 'peri0_gates', 'peri0_status', 'g2d_clk', 'g2d_bgr', 'de_sys_bgr', 'ahb_gates',
            'mbus_gates', 'rccu_pck', 'pck1_policy', 'pck1_status', 'pck1_delay', 'iommu0', 'iommu1', 'rtc_osc',
            'de_clk', 'de_bgr', 'di_clk', 'di_bgr', 'gicd_enable', 'gicd_pending', 'gicd_active')
LOCAL = ('sclk_gate', 'hclk_gate', 'ahb_reset', 'sclk_div', 'version', 'mclk_gate', 'rcq_irq', 'rcq_status',
         'rcq_ctrl', 'mixer_ctrl', 'mixer_irq')
SAMPLES = 8
HEAD = struct.Struct('<8I')
INFO = struct.Struct(f'<4I{len(PLATFORM)}I{len(PLATFORM)}I{len(LOCAL)}I4x3Q3Q2I{SAMPLES}I{SAMPLES}I2Q2I')
CLOSED = struct.Struct('<6IQ')
RUN = struct.Struct('<16I')
DONE = struct.Struct('<9I8I2I10I5I7Q')
SIZE = HEAD.size + INFO.size + CLOSED.size + RUN.size + DONE.size
CHECK_MAGIC, INFO_MAGIC, DONE_MAGIC = 0x4b433247, 0x49443247, 0x44443247
STATUS = {1: 'done', 2: 'refused', 3: 'close failed', 4: 'run failed', 5: 'no surface'}
FAULT = ('bank_status0', 'bank_status1', 'l1_status0', 'l1_status1', 'l2_status0', 'l2_status1',
         'micro_status', 'micro_low', 'micro_high', 'micro_data')
BANK = ('enable', 'int_status', 'l1_status', 'l2_status')
TICKS_PER_US = 24  # the system counter runs from the 24 MHz oscillator
INTID_BIT = 1 << (83 % 32)


def words(names, values):
    return {name: f'0x{value:08x}' for name, value in zip(names, values)}


def xorshift(x):
    x ^= (x << 13) & 0xffffffff
    x ^= x >> 17
    x ^= (x << 5) & 0xffffffff
    return x


def plan(seed):
    """The geometry, rectangle and color main.c draws from the seed, and the sequence state after them."""
    x, values = seed, []
    for _ in range(7):
        x = xorshift(x)
        values.append(x)
    width = 320 + values[0] % 321
    height = 180 + values[1] % 181
    w = 1 + values[2] % width
    h = 1 + values[3] % height
    dx = values[4] % (width - w + 1)
    dy = values[5] % (height - h + 1)
    return {'width': width, 'height': height, 'stride': width * 4 + 64, 'dx': dx, 'dy': dy, 'w': w, 'h': h,
            'color': values[6]}, x


def expected(seed, opaque=False):
    """The surface bytes after the fill: the background sequence (one word per 4 bytes, padding included), then the
    w by h rectangle at dx, dy in the color as one little-endian ARGB8888 word per pixel. With opaque, the rectangle's
    alpha byte is 0xff."""
    p, x = plan(seed)
    out = bytearray(p['stride'] * p['height'])
    for i in range(0, len(out), 4):
        x = xorshift(x)
        struct.pack_into('<I', out, i, x)
    row = struct.pack('<I', p['color'] | (0xff000000 if opaque else 0)) * p['w']
    for y in range(p['h']):
        at = (p['dy'] + y) * p['stride'] + p['dx'] * 4
        out[at:at + len(row)] = row
    return p, bytes(out)


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
    status_word, status_ticks = rest[8:8 + SAMPLES], rest[8 + SAMPLES:8 + 2 * SAMPLES]
    verify_at_us, verified_seen_us, settled_supply = rest[8 + 2 * SAMPLES:8 + 2 * SAMPLES + 3]
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
        'supply': {'verify_at_us': verify_at_us, 'verified_seen_us': verified_seen_us,
                   'settled_supply': settled_supply,
                   'seen_after_request_us': verified_seen_us - row_request_us if verified_seen_us and row_request_us else None},
    }
    record['closed'] = {'g2d_clk': f'0x{c[0]:08x}', 'g2d_bgr': f'0x{c[1]:08x}', 'de_sys_bgr': f'0x{c[2]:08x}',
                        'pck1_status': f'0x{c[3]:08x}', 'de_di_open': c[4],
                        'closed_us': (c[6] - requested) / TICKS_PER_US if c[6] else None}
    record['run'] = run(payload, HEAD.size + INFO.size + CLOSED.size)
    return record


def run(payload, at):
    """The fill record: the seed's plan, the board's CRC against expected(), and the completion record."""
    r = RUN.unpack_from(payload, at)
    result, seed, width, height, stride, dx, dy, w, h, color, crc, reserved = r[:12]
    sample = r[12:16]
    signed = result - (1 << 32) if result & 0x80000000 else result
    p, reference = expected(seed)
    board = {'width': width, 'height': height, 'stride': stride, 'dx': dx, 'dy': dy, 'w': w, 'h': h, 'color': color}
    out = {'result': signed, 'seed': f'0x{seed:08x}', 'plan': board, 'plan_matches_seed': board == p,
           'crc': f'0x{crc:08x}', 'expected_crc': f'0x{zlib.crc32(reference):08x}',
           'crc_matches': signed == 1 and crc == zlib.crc32(reference),
           'crc_matches_opaque_alpha': signed == 1 and crc == zlib.crc32(expected(seed, opaque=True)[1])}
    first = p['dy'] * p['stride'] + p['dx'] * 4
    last = (p['dy'] + p['h'] - 1) * p['stride'] + (p['dx'] + p['w'] - 1) * 4
    before = first - 4 if p['dx'] else p['dy'] * p['stride'] + p['stride'] - 4
    pad = p['dy'] * p['stride'] + p['width'] * 4
    want = [struct.unpack_from('<I', reference, at)[0] for at in (first, last, before, pad)]
    out['samples'] = {name: {'board': f'0x{got:08x}', 'expected': f'0x{exp:08x}'}
                      for name, got, exp in zip(('first', 'last', 'before', 'padding'), sample, want)}
    d = DONE.unpack_from(payload, at + RUN.size)
    if d[0] != DONE_MAGIC:
        out['done'] = None
        return out
    if d[1] != DONE.size:
        raise ValueError(f'ss_g2d_done is {d[1]} bytes, not {DONE.size}.')
    rcq_status, reset_word, status_after, irq_count, scan, output_iova, attached = d[2:9]
    bank, bypass, fault = d[9:17], d[17:19], d[19:29]
    fault_step, fault_word, fault_read, refused, stray = d[29:34]
    submit, launch, irq, done, attach_us, surface, directory = d[34:41]
    out['done'] = {
        'rcq_status': f'0x{rcq_status:08x}', 'reset_word': f'0x{reset_word:08x}',
        'status_after': f'0x{status_after:08x}', 'irq_count': irq_count, 'iommu_scan': scan,
        'output_iova': f'0x{output_iova:08x}', 'iommu_attached': attached,
        'iommu_entry': {f'bank{b}': words(BANK, bank[4 * b:4 * b + 4]) for b in (0, 1)},
        'iommu_bypass_entry': {'usb': bypass[0], 'g2d': bypass[1]},
        'iommu_fault': words(FAULT, fault), 'attach_us': attach_us,
        'iommu_owner': {'fault_step': fault_step, 'fault_word': f'0x{fault_word:08x}',
                        'fault_read': f'0x{fault_read:08x}', 'refused_word': refused},
        'stray_interrupts': stray,
        'surface_physical': f'0x{surface:x}', 'directory_physical': f'0x{directory:x}',
        'submit_to_launch_us': (launch - submit) / TICKS_PER_US if launch else None,
        'launch_to_irq_us': (irq - launch) / TICKS_PER_US if irq and launch else None,
        'irq_to_done_us': (done - irq) / TICKS_PER_US if done and irq else None,
        'submit_to_done_us': (done - submit) / TICKS_PER_US if done else None,
    }
    return out
