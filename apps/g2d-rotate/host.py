#!/usr/bin/env python3
"""Host side of g2d-rotate: the decoder of its result and the reference of its four rotates
(docs/g2d-os.md#rotate-module).

tools/board.py loads this module with --decoder. decode() reads the result that ss_finish returns (struct
g2d_rotate_result in apps/g2d-rotate/main.c, which embeds struct ss_g2d_info, struct ss_g2d_closed and four struct
ss_g2d_done from include/sunstone_g2d.h) and names every recorded word.

Every rotate reads the same 96 by 40 ARGB8888 rectangle of the source, at 0, 180, 90 and 270 degrees, with no flip.
For each rotate the decoder names the output model its rectangle CRC-32 equals. A model is a placement and a pixel
rule. The placements give output pixel (row r, column c) from source pixel S[y][x] of the h-row, w-column rectangle:
- identity S[r][c], rot180 S[h-1-r][w-1-c], mirror_h S[r][w-1-c] and mirror_v S[h-1-r][c], for a w by h output;
- cw S[h-1-c][r] (clockwise), ccw S[c][w-1-r] (counter-clockwise), transpose S[c][r] and antitranspose
  S[h-1-c][w-1-r], for an h by w output.
The pixel rules are exact (every byte of the source), zeroed (a source pixel with alpha 0 written as 0x00000000, as the
blender's copy does, board run 1794723622887180087-445859c1) and opaque (every alpha byte 0xff). The model untouched is
the background left unchanged.

The prediction is exact at every degree, because the rotate module does not pass through the blender. The placements
predicted are identity for 0 and rot180 for 180. The vendor source does not say which way degree field 1 (90) turns:
the prediction is cw for 90 and ccw for 270, and the stage passes on the four exact models with 90 and 270 inverse to
each other. The decoder also checks the five sampled words of each rectangle (its corners and the pixel before its
first) against the matched model, and gives the whole-surface CRC-32 that follows from the matched models. A rotate
that failed is measured and matched as well, so its record shows whether the engine wrote the rectangle (untouched
means it did not). A rectangle that matches no model has model None.
"""
import struct
import zlib

PLATFORM = ('pll_ref', 'peri0', 'peri0_gates', 'peri0_status', 'g2d_clk', 'g2d_bgr', 'de_sys_bgr', 'ahb_gates',
            'mbus_gates', 'rccu_pck', 'pck1_policy', 'pck1_status', 'pck1_delay', 'iommu0', 'iommu1', 'rtc_osc',
            'de_clk', 'de_bgr', 'di_clk', 'di_bgr', 'gicd_enable', 'gicd_pending', 'gicd_active')
LOCAL = ('sclk_gate', 'hclk_gate', 'ahb_reset', 'sclk_div', 'version', 'mclk_gate', 'rcq_irq', 'rcq_status',
         'rcq_ctrl', 'mixer_ctrl', 'mixer_irq')
TRANSITION = ('policy_before', 'status_before', 'policy_written', 'first_word', 'pending_word', 'reached_word',
              'all_and', 'all_or', 'reads', 'spin_reads', 'commands', 'command_at', 'first_at', 'pending_at', 'reached_at')
HEAD = struct.Struct('<8I')
INFO = struct.Struct(f'<4I{len(PLATFORM)}I{len(PLATFORM)}I{len(LOCAL)}I4x3Q2Q2I2Q11I4x4Q')
CLOSED = struct.Struct('<6IQ')
PLAN = struct.Struct('<8I')
OP = struct.Struct('<16I')
DONE = struct.Struct('<9I8I2I10I5I6I7Q8I')
OPS = 4
SIZE = HEAD.size + INFO.size + CLOSED.size + PLAN.size + OPS * (OP.size + DONE.size)
CHECK_MAGIC, INFO_MAGIC, DONE_MAGIC = 0x52543247, 0x54443247, 0x44443247
EARLIER_INFO_MAGIC = 0x49443247  # "G2DI": the record of images before the transition record
STATUS = {1: 'done', 2: 'refused', 3: 'close failed', 4: 'run failed', 5: 'no surface'}
FAULT = ('bank_status0', 'bank_status1', 'l1_status0', 'l1_status1', 'l2_status0', 'l2_status1',
         'micro_status', 'micro_low', 'micro_high', 'micro_data')
BANK = ('enable', 'int_status', 'l1_status', 'l2_status')
TICKS_PER_US = 24  # the system counter runs from the 24 MHz oscillator
INTID_BIT = 1 << (83 % 32)
SEED = 0x3c6ef372
WIDTH, HEIGHT = 400, 260
STRIDE = WIDTH * 4 + 64
SX, SY, W, H = 24, 16, 96, 40
# Each rotate's degrees and output origin (apps/g2d-rotate/main.c plan).
PLAN_OPS = ((0, 150, 52), (180, 150, 100), (90, 260, 52), (270, 310, 52))
SAME_SHAPE = ('identity', 'rot180', 'mirror_h', 'mirror_v')
SWAPPED = ('cw', 'ccw', 'transpose', 'antitranspose')
RULES = ('exact', 'zeroed', 'opaque')
PREDICTED = {0: 'identity/exact', 180: 'rot180/exact', 90: 'cw/exact', 270: 'ccw/exact'}
INVERSE = {'cw': 'ccw', 'ccw': 'cw', 'transpose': 'transpose', 'antitranspose': 'antitranspose'}


def words(names, values):
    return {name: f'0x{value:08x}' for name, value in zip(names, values)}


def xorshift(x):
    x ^= (x << 13) & 0xffffffff
    x ^= x >> 17
    x ^= (x << 5) & 0xffffffff
    return x



def surfaces(seed=SEED):
    """The source and the output background: the seed's sequence, one word per 4 bytes, padding included, source
    first."""
    x, n, out = seed, STRIDE * HEIGHT, []
    for _ in range(2):
        buf = bytearray(n)
        for i in range(0, n, 4):
            x = xorshift(x)
            struct.pack_into('<I', buf, i, x)
        out.append(buf)
    return bytes(out[0]), out[1]


def source_rows(source):
    """The source rectangle, one tuple of words per row."""
    return [struct.unpack_from(f'<{W}I', source, (SY + y) * STRIDE + SX * 4) for y in range(H)]


def place(rows, placement):
    """The output rectangle's rows under a placement (see the module text)."""
    h, w = len(rows), len(rows[0])
    if placement in SAME_SHAPE:
        at = {'identity': lambda r, c: rows[r][c], 'rot180': lambda r, c: rows[h - 1 - r][w - 1 - c],
              'mirror_h': lambda r, c: rows[r][w - 1 - c], 'mirror_v': lambda r, c: rows[h - 1 - r][c]}[placement]
        return [[at(r, c) for c in range(w)] for r in range(h)]
    at = {'cw': lambda r, c: rows[h - 1 - c][r], 'ccw': lambda r, c: rows[c][w - 1 - r],
          'transpose': lambda r, c: rows[c][r], 'antitranspose': lambda r, c: rows[h - 1 - c][w - 1 - r]}[placement]
    return [[at(r, c) for c in range(h)] for r in range(w)]


def pixel(word, rule):
    if rule == 'opaque':
        return word | 0xff000000
    return 0 if rule == 'zeroed' and not word >> 24 else word


def shape(degrees):
    return (H, W) if degrees in (90, 270) else (W, H)


def models(source, background, degrees, dx, dy):
    """Every model's output rectangle rows for one rotate, by name, the background's included as untouched."""
    rows = source_rows(source)
    out = {}
    for placement in (SWAPPED if degrees in (90, 270) else SAME_SHAPE):
        placed = place(rows, placement)
        for rule in RULES:
            out[f'{placement}/{rule}'] = [[pixel(v, rule) for v in row] for row in placed]
    ow, oh = shape(degrees)
    out['untouched'] = [list(struct.unpack_from(f'<{ow}I', background, (dy + r) * STRIDE + dx * 4)) for r in range(oh)]
    return out


def rect_crc(rows):
    return zlib.crc32(b''.join(struct.pack(f'<{len(row)}I', *row) for row in rows))


def corners(rows):
    return [rows[0][0], rows[0][-1], rows[-1][0], rows[-1][-1]]


def output(seed, names):
    """The whole output surface after the rotates, rotate k under model names[k]."""
    source, out = surfaces(seed)
    background = bytes(out)
    for (degrees, dx, dy), name in zip(PLAN_OPS, names):
        for r, row in enumerate(models(source, background, degrees, dx, dy)[name]):
            struct.pack_into(f'<{len(row)}I', out, (dy + r) * STRIDE + dx * 4, *row)
    return source, bytes(out)


def predicted(seed=SEED):
    """The predicted rectangle CRC-32 and corner samples of each rotate, and the whole-surface CRC-32 values."""
    source, background = surfaces(seed)
    ops = []
    for degrees, dx, dy in PLAN_OPS:
        rows = models(source, bytes(background), degrees, dx, dy)[PREDICTED[degrees]]
        ops.append({'degrees': degrees, 'model': PREDICTED[degrees], 'rect_crc': f'0x{rect_crc(rows):08x}',
                    'corners': [f'0x{v:08x}' for v in corners(rows)]})
    _, out = output(seed, [PREDICTED[d] for d, _, _ in PLAN_OPS])
    return {'ops': ops, 'output_crc': f'0x{zlib.crc32(out):08x}', 'source_crc': f'0x{zlib.crc32(source):08x}',
            'output_before_crc': f'0x{zlib.crc32(bytes(background)):08x}'}


def runs(payload, at):
    """The plan record, each rotate's record and the model its rectangle matched, and both surfaces' CRC-32 values."""
    seed, width, height, stride, output_crc, source_crc, output_before, source_before = PLAN.unpack_from(payload, at)
    source, background = surfaces(seed)
    background = bytes(background)
    ops, at = [], at + PLAN.size
    for k in range(OPS):
        r = OP.unpack_from(payload, at)
        result, degrees, flip, sx, sy, dx, dy, w, h, crc = r[:10]
        sample, reserved = r[10:15], r[15]
        if reserved:
            raise ValueError(f'rotate {k + 1} has a nonzero reserved word.')
        signed = result - (1 << 32) if result & 0x80000000 else result
        planned = PLAN_OPS[k]
        o = {'result': signed, 'run': bool(result), 'degrees': degrees if result else None,
             'plan_matches': (degrees, flip, sx, sy, dx, dy, w, h) == (planned[0], 0, SX, SY, planned[1], planned[2], W, H)
             or not result,
             'rect_crc': f'0x{crc:08x}'}
        if result:
            # Measured for a failed rotate too (apps/g2d-rotate/main.c): untouched shows the engine never wrote.
            candidates = models(source, background, planned[0], planned[1], planned[2])
            o['model'] = next((n for n, rows in candidates.items() if rect_crc(rows) == crc), None)
            o['predicted_model'] = PREDICTED[planned[0]]
            before = struct.unpack_from('<I', background, planned[2] * STRIDE + planned[1] * 4 - 4)[0]
            o['samples'] = {'corners': [f'0x{v:08x}' for v in sample[:4]],
                            'before': {'board': f'0x{sample[4]:08x}', 'expected': f'0x{before:08x}'}}
            o['samples_models'] = [n for n, rows in candidates.items() if corners(rows) == list(sample[:4])]
            o['samples_match'] = (o['model'] is None or o['model'] in o['samples_models']) and sample[4] == before
        o['done'] = done_record(payload, at + OP.size)
        ops.append(o)
        at += OP.size + DONE.size
    done = all(o['result'] == 1 for o in ops)
    names = [o.get('model') for o in ops]
    out = {'plan': {'seed': f'0x{seed:08x}', 'width': width, 'height': height, 'stride': stride,
                    'matches': (width, height, stride) == (WIDTH, HEIGHT, STRIDE) and all(o['plan_matches'] for o in ops)},
           'ops': ops,
           'crc': {'output': f'0x{output_crc:08x}', 'source': f'0x{source_crc:08x}',
                   'output_before': f'0x{output_before:08x}', 'source_before': f'0x{source_before:08x}',
                   'before_match': output_before == zlib.crc32(background) and source_before == zlib.crc32(source),
                   'source_unchanged': source_crc == zlib.crc32(source) if done else None}}
    if done and None not in names:
        expected = zlib.crc32(output(seed, names)[1])
        out['crc']['expected_output'] = f'0x{expected:08x}'
        out['crc']['output_matches'] = output_crc == expected
    placements = {o.get('degrees'): (o.get('model') or '/').split('/') for o in ops}
    out['exact'] = done and all((o.get('model') or '').endswith('/exact') for o in ops)
    out['inverse_90_270'] = done and INVERSE.get(placements[90][0]) == placements[270][0]
    out['predicted'] = done and all(o.get('model') == o.get('predicted_model') for o in ops)
    return out


def decode(payload):
    """A dict of the g2d-rotate record; ValueError if the payload is malformed."""
    if len(payload) != SIZE:
        raise ValueError(f'g2d-rotate result is {len(payload)} bytes, not {SIZE}.')
    magic, stage, status, open_result, close_result, fault_step, fault_value, reserved = HEAD.unpack_from(payload)
    if magic != CHECK_MAGIC or reserved:
        raise ValueError('g2d-rotate result has a wrong magic or a nonzero reserved word.')
    v = INFO.unpack_from(payload, HEAD.size)
    p = len(PLATFORM)
    info_magic, info_bytes, domain_entry, module_hz = v[:4]
    entry, after = v[4:4 + p], v[4 + p:4 + 2 * p]
    local = v[4 + 2 * p:4 + 2 * p + len(LOCAL)]
    rest = v[4 + 2 * p + len(LOCAL):]
    requested, domain_on, opened, row_request_us, row_on_us, bit8, settled_supply, verify_at_us, verified_seen_us = rest[:9]
    transition = dict(zip(TRANSITION, rest[9:9 + len(TRANSITION)]))
    c = CLOSED.unpack_from(payload, HEAD.size + INFO.size)
    signed = lambda x: x - (1 << 32) if x & 0x80000000 else x
    record = {
        'stage': stage, 'status': STATUS.get(status, status),
        'open_result': signed(open_result), 'close_result': signed(close_result),
        'fault_step': fault_step, 'fault_value': f'0x{fault_value:08x}',
    }
    if info_magic == EARLIER_INFO_MAGIC:
        raise ValueError('ss_g2d_info is the earlier layout (magic G2DI, domain-1 status samples); '
                         'decode it with the host.py of the image that produced it.')
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
        'owner_clock_us': {'row_request': row_request_us, 'pck_command': transition['command_at'], 'row_on': row_on_us,
                           'adopted': transition['commands'] == 0 if row_on_us else None},
        'pck_bit8': {'entry_policy': bit8 & 1, 'entry_status': bit8 >> 1 & 1,
                     'after_policy': bit8 >> 8 & 1, 'after_status': bit8 >> 9 & 1},
        'transition': {k: f'0x{v:08x}' if k.endswith(('_before', '_written', '_word', '_and', '_or')) else v
                       for k, v in transition.items()},
        'supply': {'verify_at_us': verify_at_us, 'verified_seen_us': verified_seen_us,
                   'settled_supply': settled_supply,
                   'seen_after_request_us': verified_seen_us - row_request_us if verified_seen_us and row_request_us else None},
    }
    record['closed'] = {'g2d_clk': f'0x{c[0]:08x}', 'g2d_bgr': f'0x{c[1]:08x}', 'de_sys_bgr': f'0x{c[2]:08x}',
                        'pck1_status': f'0x{c[3]:08x}', 'de_di_open': c[4],
                        'closed_us': (c[6] - requested) / TICKS_PER_US if c[6] else None}
    record.update(runs(payload, HEAD.size + INFO.size + CLOSED.size))
    return record




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
    update_after, mixer_after, prefetch0, prefetch1, source_iova, status_launch = d[34:40]
    submit, launch, irq, done, attach_us, surface, directory = d[40:47]
    rot_ctl, rot_ctl_after, rot_int, rot_int_after, rot_timeout, stray_finishes, stray_word, reserved = d[47:55]
    if reserved:
        raise ValueError('ss_g2d_done has a nonzero reserved word.')
    return {
        'rcq_status': f'0x{rcq_status:08x}', 'frame_cnt': rcq_status >> 8 & 255, 'reset_word': f'0x{reset_word:08x}',
        'status_after': f'0x{status_after:08x}', 'status_launch': f'0x{status_launch:08x}', 'update_after': f'0x{update_after:08x}',
        'mixer_after': f'0x{mixer_after:08x}', 'irq_count': irq_count, 'iommu_scan': scan,
        'output_iova': f'0x{output_iova:08x}', 'source_iova': f'0x{source_iova:08x}', 'iommu_attached': attached,
        'iommu_prefetch': {'bank0': f'0x{prefetch0:08x}', 'bank1': f'0x{prefetch1:08x}'},
        'iommu_entry': {f'bank{b}': words(BANK, bank[4 * b:4 * b + 4]) for b in (0, 1)},
        'iommu_bypass_entry': {'usb': bypass[0], 'g2d': bypass[1]},
        'iommu_fault': words(FAULT, fault), 'attach_us': attach_us,
        'iommu_owner': {'fault_step': fault_step, 'fault_word': f'0x{fault_word:08x}',
                        'fault_read': f'0x{fault_read:08x}', 'refused_word': refused},
        'stray_interrupts': stray, 'stray_finishes': stray_finishes, 'stray_word': f'0x{stray_word:08x}',
        'rotate': {'ctl': f'0x{rot_ctl:08x}', 'ctl_after': f'0x{rot_ctl_after:08x}', 'int': f'0x{rot_int:08x}',
                   'int_after': f'0x{rot_int_after:08x}', 'timeout': f'0x{rot_timeout:08x}'},
        'surface_physical': f'0x{surface:x}', 'directory_physical': f'0x{directory:x}',
        'submit_to_launch_us': (launch - submit) / TICKS_PER_US if launch else None,
        'launch_to_irq_us': (irq - launch) / TICKS_PER_US if irq and launch else None,
        'irq_to_done_us': (done - irq) / TICKS_PER_US if done and irq else None,
        'submit_to_done_us': (done - submit) / TICKS_PER_US if done else None,
    }
