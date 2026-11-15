#!/usr/bin/env python3
"""Host side of g2d-check: the decoder of its result and the reference of its two copies.

tools/board.py loads this module with --decoder or --decode-application. decode() reads the result that ss_finish
returns (struct g2d_check_result in apps/g2d-check/main.c, which embeds struct ss_g2d_info, struct ss_g2d_closed and
two struct ss_g2d_done from include/sunstone_g2d.h), names every recorded word, and compares the board's CRC-32 values
of both surfaces with expected(), which rebuilds them from the recorded seed as main.c does: the source and the output
background, then operation 1 (ARGB8888 to BGRA8888) and operation 2 (ARGB8888 unconverted) in order. The check takes
no input beyond the seed compiled into main.c.

Converting ARGB8888 to BGRA8888 reverses the bytes of each 32-bit word: both names list channels from the most
significant byte. The decoder also reports whether the board's output CRC-32 equals the output with both copies
unconverted, which identifies a conversion that did not happen, or the output with every copied pixel's alpha byte
0xff, which identifies a copy that forces alpha.
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
PLAN = struct.Struct('<8I')
OP = struct.Struct('<16I')
DONE = struct.Struct('<9I8I2I10I5I6I7Q')
OPS = 2
SIZE = HEAD.size + INFO.size + CLOSED.size + PLAN.size + OPS * (OP.size + DONE.size)
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
    """The geometry and both operations main.c draws from the seed, and the sequence state after them."""
    x, v = seed, []
    for _ in range(2 + 6 * OPS):
        x = xorshift(x)
        v.append(x)
    width, height = 320 + v[0] % 321, 180 + v[1] % 181
    ops = []
    for k in range(OPS):
        w, h = 1 + v[2 + 6 * k] % width, 1 + v[3 + 6 * k] % height
        ops.append({'w': w, 'h': h, 'sx': v[4 + 6 * k] % (width - w + 1), 'sy': v[5 + 6 * k] % (height - h + 1),
                    'dx': v[6 + 6 * k] % (width - w + 1), 'dy': v[7 + 6 * k] % (height - h + 1),
                    'format': 0, 'output_format': 3 if k == 0 else 0})
    return {'width': width, 'height': height, 'stride': width * 4 + 64, 'ops': ops}, x


def surfaces(seed):
    """The source and the output background: the seed's sequence after the plan, one word per 4 bytes, padding
    included, source first."""
    p, x = plan(seed)
    n = p['stride'] * p['height']
    out = []
    for _ in range(2):
        buf = bytearray(n)
        for i in range(0, n, 4):
            x = xorshift(x)
            struct.pack_into('<I', buf, i, x)
        out.append(buf)
    return p, bytes(out[0]), out[1]


def convert(word, source_format, output_format, converted=True, opaque=False):
    """One pixel from the source format to the output format. Only ARGB8888 (0) to ARGB8888 or BGRA8888 (3) is
    modeled; with converted False the word is copied as it is, and with opaque the output's alpha byte is 0xff."""
    assert source_format == 0 and output_format in (0, 3)
    if output_format == 0 or not converted:
        return word | (0xff000000 if opaque else 0)
    return int.from_bytes(word.to_bytes(4, 'little'), 'big') | (0xff if opaque else 0)


def expected(seed, converted=True, count=OPS, opaque=False):
    """The plan, the source bytes, and the output bytes after the first count operations."""
    p, source, out = surfaces(seed)
    s = p['stride']
    for op in p['ops'][:count]:
        for y in range(op['h']):
            row = struct.unpack_from(f'<{op["w"]}I', source, (op['sy'] + y) * s + op['sx'] * 4)
            struct.pack_into(f'<{op["w"]}I', out, (op['dy'] + y) * s + op['dx'] * 4,
                             *(convert(w, op['format'], op['output_format'], converted, opaque) for w in row))
    return p, source, bytes(out)


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
    record.update(runs(payload, HEAD.size + INFO.size + CLOSED.size))
    return record


def runs(payload, at):
    """The plan record, both surfaces' CRC-32 values against expected(), and each operation's record."""
    seed, width, height, stride, output_crc, source_crc, output_before, source_before = PLAN.unpack_from(payload, at)
    p, source, reference = expected(seed)
    _, _, unconverted = expected(seed, converted=False)
    _, _, opaque = expected(seed, opaque=True)
    _, _, background = surfaces(seed)
    out = {'plan': {'seed': f'0x{seed:08x}', 'width': width, 'height': height, 'stride': stride,
                    'matches_seed': (width, height, stride) == (p['width'], p['height'], p['stride'])},
           'crc': {'output': f'0x{output_crc:08x}', 'expected_output': f'0x{zlib.crc32(reference):08x}',
                   'unconverted_output': f'0x{zlib.crc32(unconverted):08x}',
                   'opaque_output': f'0x{zlib.crc32(opaque):08x}',
                   'source': f'0x{source_crc:08x}', 'expected_source': f'0x{zlib.crc32(source):08x}',
                   'output_before': f'0x{output_before:08x}', 'expected_output_before': f'0x{zlib.crc32(bytes(background)):08x}',
                   'source_before': f'0x{source_before:08x}'}}
    ops, at = [], at + PLAN.size
    for k in range(OPS):
        ops.append(operation(payload, at, seed, p, k))
        at += OP.size + DONE.size
    out['ops'] = ops
    done = all(o['result'] == 1 for o in ops)
    out['crc']['output_matches'] = done and output_crc == zlib.crc32(reference)
    out['crc']['output_unconverted'] = done and output_crc == zlib.crc32(unconverted)
    out['crc']['output_opaque'] = done and output_crc == zlib.crc32(opaque)
    out['crc']['source_unchanged'] = done and source_crc == zlib.crc32(source) == source_before
    out['crc']['before_match'] = output_before == zlib.crc32(bytes(background)) and source_before == zlib.crc32(source)
    out['plan']['matches_seed'] = out['plan']['matches_seed'] and all(o['plan_matches_seed'] for o in ops)
    return out


def operation(payload, at, seed, p, k):
    """Operation k's record: its plan against the seed's, its samples against the output after operations 1 to k + 1
    (main.c reads them right after the operation), and its completion record."""
    r = OP.unpack_from(payload, at)
    result, sx, sy, dx, dy, w, h, fmt, output_format = r[:9]
    sample = r[12:16]
    signed = result - (1 << 32) if result & 0x80000000 else result
    want = p['ops'][k]
    board = {'w': w, 'h': h, 'sx': sx, 'sy': sy, 'dx': dx, 'dy': dy, 'format': fmt, 'output_format': output_format}
    out = {'result': signed, 'plan': board, 'plan_matches_seed': board == want}
    reference = expected(seed, count=k + 1)[2]
    s = p['stride']
    first = want['dy'] * s + want['dx'] * 4
    last = (want['dy'] + want['h'] - 1) * s + (want['dx'] + want['w'] - 1) * 4
    before = first - 4 if want['dx'] else want['dy'] * s + s - 4
    pad = want['dy'] * s + p['width'] * 4
    expect = [struct.unpack_from('<I', reference, a)[0] for a in (first, last, before, pad)]
    out['samples'] = {name: {'board': f'0x{got:08x}', 'expected': f'0x{exp:08x}'}
                      for name, got, exp in zip(('first', 'last', 'before', 'padding'), sample, expect)}
    out['samples_match'] = signed == 1 and list(sample) == expect
    out['done'] = done_record(payload, at + OP.size)
    return out


def done_record(payload, at):
    """One struct ss_g2d_done, or None when the operation left none."""
    d = DONE.unpack_from(payload, at)
    if d[0] != DONE_MAGIC:
        return None
    if d[1] != DONE.size:
        raise ValueError(f'ss_g2d_done is {d[1]} bytes, not {DONE.size}.')
    rcq_status, reset_word, status_after, irq_count, scan, output_iova, attached = d[2:9]
    bank, bypass, fault = d[9:17], d[17:19], d[19:29]
    fault_step, fault_word, fault_read, refused, stray = d[29:34]
    update_after, mixer_after, prefetch0, prefetch1, source_iova, reserved = d[34:40]
    submit, launch, irq, done, attach_us, surface, directory = d[40:47]
    return {
        'rcq_status': f'0x{rcq_status:08x}', 'frame_cnt': rcq_status >> 8 & 255, 'reset_word': f'0x{reset_word:08x}',
        'status_after': f'0x{status_after:08x}', 'update_after': f'0x{update_after:08x}',
        'mixer_after': f'0x{mixer_after:08x}', 'irq_count': irq_count, 'iommu_scan': scan,
        'output_iova': f'0x{output_iova:08x}', 'source_iova': f'0x{source_iova:08x}', 'iommu_attached': attached,
        'iommu_prefetch': {'bank0': f'0x{prefetch0:08x}', 'bank1': f'0x{prefetch1:08x}'},
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
