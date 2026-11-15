#!/usr/bin/env python3
"""Host side of g2d-paths: the decoder of its result and the reference of its five copies (docs/g2d-os.md#copy-paths).

tools/board.py loads this module with --decoder. decode() reads the result that ss_finish returns (struct
g2d_paths_result in apps/g2d-paths/main.c, which embeds struct ss_g2d_info, struct ss_g2d_closed and five struct
ss_g2d_done from include/sunstone_g2d.h) and names every recorded word.

Every copy reads the same 96 by 40 ARGB8888 rectangle of the source, unconverted, on one blender path. For each copy
the decoder names which output model its rectangle CRC-32 equals:
- exact: every byte of the source;
- zeroed: a pixel with source alpha 0 written as 0x00000000, as the vendor path does (board run
  1794723622887180087-445859c1);
- fill_marker or background_marker: such a pixel written as the marker the MARKERS path puts in pipe 0's fill colour
  (0x00a1b2c3) or the blender background (0x00d4e5f6);
- opaque: every pixel's alpha byte 0xff.

It also gives the output at the first source pixel with alpha 0, with alpha 0xff and with any other alpha under every
model, and the whole-surface CRC-32 that follows from the models the copies matched.
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
OPS = 5
SIZE = HEAD.size + INFO.size + CLOSED.size + PLAN.size + OPS * (OP.size + DONE.size)
CHECK_MAGIC, INFO_MAGIC, DONE_MAGIC = 0x50443247, 0x49443247, 0x44443247
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


SEED = 0x3c6ef372
WIDTH, HEIGHT = 400, 260
STRIDE = WIDTH * 4 + 64
SX, SY, W, H = 24, 16, 96, 40
DX, DY, STEP = 150, 52, 42
PATHS = ('vendor', 'markers', 'premultiplied', 'rop_copy', 'no_fill')
FILL_MARKER, BACKGROUND_MARKER = 0x00a1b2c3, 0x00d4e5f6
MODELS = ('exact', 'zeroed', 'fill_marker', 'background_marker', 'opaque')
# The model each path is predicted to give (the stage-5 hypothesis).
PREDICTED = {'vendor': 'zeroed', 'markers': 'fill_marker', 'premultiplied': 'exact', 'rop_copy': 'exact',
             'no_fill': 'exact'}


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


def pixel(word, model):
    """One copied ARGB8888 pixel under an output model."""
    if model == 'opaque':
        return word | 0xff000000
    if word >> 24 or model == 'exact':
        return word
    return {'zeroed': 0, 'fill_marker': FILL_MARKER, 'background_marker': BACKGROUND_MARKER}[model]


def rows(source):
    """The source rectangle, one tuple of words per row."""
    return [struct.unpack_from(f'<{W}I', source, (SY + y) * STRIDE + SX * 4) for y in range(H)]


def rectangle(source, model):
    """The bytes of one copy's output rectangle under a model, rows concatenated."""
    return b''.join(struct.pack(f'<{W}I', *(pixel(w, model) for w in row)) for row in rows(source))


def picks(source):
    """The row-order indices of the first source pixels with alpha 0, alpha 0xff and any other alpha, as main.c's
    find(); None when there is none."""
    words = [w for row in rows(source) for w in row]
    tests = (lambda a: a == 0, lambda a: a == 0xff, lambda a: a not in (0, 0xff))
    return [next((i for i, w in enumerate(words) if test(w >> 24)), None) for test in tests]


def output(seed, models):
    """The whole output surface after the copies, copy k under models[k]."""
    source, out = surfaces(seed)
    for k, model in enumerate(models):
        for y, row in enumerate(rows(source)):
            struct.pack_into(f'<{W}I', out, (DY + k * STEP + y) * STRIDE + DX * 4, *(pixel(w, model) for w in row))
    return source, bytes(out)


def predicted(seed=SEED):
    """The predicted rectangle CRC-32 and samples of each copy, and the whole-surface CRC-32 values."""
    source, background = surfaces(seed)
    index = picks(source)
    words = [w for row in rows(source) for w in row]
    per = {m: {'rect_crc': zlib.crc32(rectangle(source, m)),
               'samples': [pixel(words[i], m) if i is not None else 0 for i in index]} for m in MODELS}
    models = [PREDICTED[p] for p in PATHS]
    _, out = output(seed, models)
    return {'models': per, 'picks': index, 'output_crc': zlib.crc32(out), 'source_crc': zlib.crc32(source),
            'output_before_crc': zlib.crc32(bytes(background))}


def decode(payload):
    """A dict of the g2d-paths record; ValueError if the payload is malformed."""
    if len(payload) != SIZE:
        raise ValueError(f'g2d-paths result is {len(payload)} bytes, not {SIZE}.')
    magic, stage, status, open_result, close_result, fault_step, fault_value, reserved = HEAD.unpack_from(payload)
    if magic != CHECK_MAGIC or reserved:
        raise ValueError('g2d-paths result has a wrong magic or a nonzero reserved word.')
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
    """The plan record, each copy's record and the model its rectangle matched, and both surfaces' CRC-32 values."""
    seed, width, height, stride, output_crc, source_crc, output_before, source_before = PLAN.unpack_from(payload, at)
    source, background = surfaces(seed)
    index = picks(source)
    words = [w for row in rows(source) for w in row]
    crcs = {m: zlib.crc32(rectangle(source, m)) for m in MODELS}
    ops, at = [], at + PLAN.size
    for k in range(OPS):
        r = OP.unpack_from(payload, at)
        result, path, sx, sy, dx, dy, w, h, rect_crc, alpha_0, alpha_ff, partial = r[:12]
        sample = r[12:16]
        signed = result - (1 << 32) if result & 0x80000000 else result
        # A copy after the first failure is not run and its record stays zero, path word included.
        name = (PATHS[path] if path < len(PATHS) else path) if result else None
        o = {'result': signed, 'run': bool(result), 'path': name,
             'plan_matches': (path, sx, sy, dx, dy, w, h) == (k, SX, SY, DX, DY + k * STEP, W, H) or not result,
             'picks': {'alpha_0': alpha_0, 'alpha_ff': alpha_ff, 'partial': partial,
                       'match': [alpha_0, alpha_ff, partial] == [0xffffffff if i is None else i for i in index]},
             'rect_crc': f'0x{rect_crc:08x}'}
        if signed == 1:
            o['model'] = next((m for m in MODELS if crcs[m] == rect_crc), None)
            o['predicted_model'] = PREDICTED.get(name)
            before = struct.unpack_from('<I', background, (DY + k * STEP) * STRIDE + DX * 4 - 4)[0]
            o['samples'] = {n: {'board': f'0x{got:08x}',
                                'models': {m: f'0x{pixel(words[i], m):08x}' for m in MODELS} if i is not None else None}
                            for n, got, i in zip(('alpha_0', 'alpha_ff', 'partial'), sample, index)}
            o['samples']['before'] = {'board': f'0x{sample[3]:08x}', 'expected': f'0x{before:08x}'}
            o['samples_model'] = next((m for m in MODELS if all(
                i is None or got == pixel(words[i], m) for got, i in zip(sample, index))), None)
        o['done'] = done_record(payload, at + OP.size)
        ops.append(o)
        at += OP.size + DONE.size
    done = all(o['result'] == 1 for o in ops)
    models = [o.get('model') for o in ops]
    out = {'plan': {'seed': f'0x{seed:08x}', 'width': width, 'height': height, 'stride': stride,
                    'matches': (width, height, stride) == (WIDTH, HEIGHT, STRIDE) and all(o['plan_matches'] for o in ops)},
           'ops': ops,
           'crc': {'output': f'0x{output_crc:08x}', 'source': f'0x{source_crc:08x}',
                   'output_before': f'0x{output_before:08x}', 'source_before': f'0x{source_before:08x}',
                   'before_match': output_before == zlib.crc32(bytes(background)) and source_before == zlib.crc32(source),
                   'source_unchanged': source_crc == zlib.crc32(source) if done else None,
                   'rectangle_models': {m: f'0x{c:08x}' for m, c in crcs.items()}}}
    if done and None not in models:
        out['crc']['expected_output'] = f'0x{zlib.crc32(output(seed, models)[1]):08x}'
        out['crc']['output_matches'] = output_crc == zlib.crc32(output(seed, models)[1])
    out['predicted'] = all(o.get('model') == o.get('predicted_model') for o in ops) and done
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
    update_after, mixer_after, prefetch0, prefetch1, source_iova, status_launch = d[34:40]
    submit, launch, irq, done, attach_us, surface, directory = d[40:47]
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
        'stray_interrupts': stray,
        'surface_physical': f'0x{surface:x}', 'directory_physical': f'0x{directory:x}',
        'submit_to_launch_us': (launch - submit) / TICKS_PER_US if launch else None,
        'launch_to_irq_us': (irq - launch) / TICKS_PER_US if irq and launch else None,
        'irq_to_done_us': (done - irq) / TICKS_PER_US if done and irq else None,
        'submit_to_done_us': (done - submit) / TICKS_PER_US if done else None,
    }
