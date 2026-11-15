#!/usr/bin/env python3
"""Host side of g2d-convert: the decoder of its result and the models of its seven operations
(docs/g2d-os.md#second-conversion-stage).

tools/board.py loads this module with --decoder. decode() reads the result that ss_finish returns (struct
g2d_convert_result in apps/g2d-convert/main.c, which embeds struct ss_g2d_info, struct ss_g2d_closed and seven struct
ss_g2d_done from include/sunstone_g2d.h) and names every recorded word.

Frames. frames() rebuilds both NV16 frames from the seed as the application does. Frame A's luma is the seed's
sequence, with every fourth row replaced by (x - 8 + y) mod 256; its chroma is one pair per row in the left half and
one pair per two pixels in the right half. Frame B is luma 150 with chroma (90, 170) in the left half and a luma ramp
from 16 to 235 over chroma (128, 128) in the right half, the same on every row.

The engine's model, derived from stage 7 (run 1794749218366895381-6f184f31) and predicted for A: luma below 16 raised
to 16, the programmed table (drivers/g2d/a733_g2d_convert.c: BT.709 full is g2d_bld.c's Ycbcr2rgb_709 block at word
24, BT.601 limited its Ycbcr2rgb_601 block at word 0) rounded down, each channel clamped to 0..255 and alpha 0xff, U
the first byte of each chroma pair. An odd pixel's chroma is the scaler's half phase, taps -4, 36, 36, -4 over pairs
k - 1 to k + 2 with the edge pairs repeated, rounded to nearest and clamped; the last pixel of a row takes its own
pair. The four candidate rules of the application's counts: 1 raised and down (the model), 2 raised and nearest
(nearest adds 512 before the shift by 10), 3 kept and down, 4 kept and nearest. In the exact region (pixels 8 to 311
of every row) no chroma is interpolated, so the region's CRC-32 names the rule; the rectangle's CRC-32 tests the
whole model. The variants B to E are measurements: the decoder names the candidate each matches, or none.

Scale models. The scaler model applies the vendor's horizontal Lanczos bank (g2d_vsu_calc_fir_coef of the step) at the
phase of each output pixel: the position x * step with 19 fraction bits, the integer part selecting taps at positions
-1 to +2 with the edges repeated, the top 5 fraction bits the phase; the chroma pairs use the half step and the bank
of the half step. Frame B is the same on every row, so the vertical filter cannot change it. The prediction for each
scale: the flat region equals the flat colour under rule 1 exactly, rows 2 to oh - 3 equal the middle row, the ramp
does not decrease in any channel, and the middle row's ramp interior is within 2 of the scaler model under rule 1.

A failed operation is measured as well: untouched means the rectangle still holds the 0x5a background.
"""
import struct
import zlib

import numpy as np

PLATFORM = ('pll_ref', 'peri0', 'peri0_gates', 'peri0_status', 'g2d_clk', 'g2d_bgr', 'de_sys_bgr', 'ahb_gates',
            'mbus_gates', 'rccu_pck', 'pck1_policy', 'pck1_status', 'pck1_delay', 'iommu0', 'iommu1', 'rtc_osc',
            'de_clk', 'de_bgr', 'di_clk', 'di_bgr', 'gicd_enable', 'gicd_pending', 'gicd_active')
LOCAL = ('sclk_gate', 'hclk_gate', 'ahb_reset', 'sclk_div', 'version', 'mclk_gate', 'rcq_irq', 'rcq_status',
         'rcq_ctrl', 'mixer_ctrl', 'mixer_irq')
TRANSITION = ('policy_before', 'status_before', 'policy_written', 'first_word', 'pending_word', 'reached_word',
              'all_and', 'all_or', 'reads', 'spin_reads', 'commands', 'command_at', 'first_at', 'pending_at', 'reached_at')
HEAD = struct.Struct('<12I2Q8I128s')
INFO = struct.Struct(f'<4I{len(PLATFORM)}I{len(PLATFORM)}I{len(LOCAL)}I4x3Q2Q2I2Q11I4x4Q')
CLOSED = struct.Struct('<6IQ')
OP = struct.Struct('<28I')
DONE = struct.Struct('<9I8I2I10I5I6I7Q8I')
OPS = 7
CONVERSIONS = 5
FW, FH = 640, 480
ROW = FW * 4
ROWS_AT = HEAD.size + INFO.size + CLOSED.size + OPS * (OP.size + DONE.size)
SIZE = ROWS_AT + CONVERSIONS * 2 * ROW + (320 + 224) * 4
MAGIC, INFO_MAGIC, DONE_MAGIC = 0x56433247, 0x54443247, 0x44443247
STATUS = {1: 'done', 2: 'refused', 3: 'close failed', 4: 'run failed', 5: 'no surface', 6: 'missed'}
FAULT = ('bank_status0', 'bank_status1', 'l1_status0', 'l1_status1', 'l2_status0', 'l2_status1',
         'micro_status', 'micro_low', 'micro_high', 'micro_data')
BANK = ('enable', 'int_status', 'l1_status', 'l2_status')
TICKS_PER_US = 24
INTID_BIT = 1 << (83 % 32)
SEED = 0x6a09e667
EXACT = (8, 312)
CONV_ROWS = (100, 333)
BACKGROUND = 0x5a
FLAT = (150, 90, 170)
CSC_NAMES = {1: 'bt601-limited', 2: 'bt601-full', 3: 'bt709-limited', 4: 'bt709-full'}
# apps/g2d-convert/main.c plan: the table, the variant, the output size and stride, the output offset, the frame.
PLAN = ((4, 0, 640, 480, 2624, 0x000000, 0), (4, 1, 640, 480, 2624, 0x000000, 0), (4, 2, 640, 480, 2624, 0x000000, 0),
        (4, 4, 640, 480, 2624, 0x000000, 0), (4, 8, 640, 480, 2624, 0x000000, 0),
        (1, 0, 320, 240, 1344, 0x280000, 1), (1, 0, 224, 224, 960, 0x300000, 1))
VARIANTS = {0: 'A vendor page', 1: 'B fill colours luma 0', 2: 'C pipe 0 fill colour off', 4: 'D csc0, RGB blend',
            8: 'E write-back rounding'}
# The block each table programs (drivers/g2d/a733_g2d_convert.c), as (array, first word).
PROGRAMMED = {1: ('601', 0), 2: ('601', 24), 3: ('709', 0), 4: ('709', 24)}
RULES = ('raised/down', 'raised/nearest', 'kept/down', 'kept/nearest')
PREDICTED_RULE = 'raised/down'
# g2d_bld.c 37-50 and 66-79.
VENDOR = {
    '601': [0x04a8, 0x0, 0x0662, 0xFFFC8480, 0x04a8, 0xFFFFFE6F, 0xFFFFFCC0, 0x21E00, 0x04a8, 0x0812, 0x0, 0xFFFBAC80,
            0x0400, 0x0, 0x057c, 0xFFFD0200, 0x0400, 0xFFFFFEA7, 0xFFFFFD35, 0x1D200, 0x0400, 0x06EE, 0x0, 0xFFFC4900,
            0x0400, 0x0, 0x059c, 0xFFFD31FF, 0x0400, 0xFFFFFE9F, 0xFFFFFD24, 0x21D80, 0x0400, 0x0717, 0x0, 0xFFFC747F,
            0x036F, 0x0, 0x04D1, 0xFFFDA090, 0x36F, 0xFFFFFED1, 0xFFFFFD8C, 0x1DA90, 0x036F, 0x0616, 0x0, 0xFFFCFE10],
    '709': [0x04a8, 0x0, 0x072c, 0xFFFC1F80, 0x04a8, 0xFFFFFF26, 0xFFFFFDDE, 0x13380, 0x04a8, 0x0873, 0, 0xFFFB7C00,
            0x0400, 0x0, 0x0629, 0xFFFCAB80, 0x0400, 0xFFFFFF44, 0xFFFFE2B, 0x10880, 0x0400, 0x0742, 0x0, 0xFFFC1F00,
            0x0400, 0x0, 0x64D, 0xFFFC9980, 0x0400, 0xFFFFFF40, 0xFFFFFE21, 0x10F80, 0x0400, 0x076C, 0x0, 0xFFFC0A00,
            0x36f, 0x0, 0x0569, 0xFFFD5490, 0x36f, 0xFFFFFF5B, 0xFFFFFE64, 0x12990, 0x36f, 0x0660, 0x0, 0xFFFCD910]}
# g2d_scal.c lan2coefftab32_full (the same bank as drivers/g2d/a733_g2d_convert.c; checks/g2d-convert-host.py compares
# them): 16 banks of 32 phases, each word four signed taps, lowest byte first.
LAN2_HEX = (
    '00004000000140ff00033ffe00043ffd00063efcff083dfc000a3bfbff0d39fbff0f37fbff1136fafe1433fbfe1631fbfd192ffbfd1c2cfb'
    'fd1f29fbfc2127fcfc2424fcfc2721fcfb291ffdfb2c1cfdfb2f19fdfb3116fefb3314fefa3611fffb370ffffb390dfffb3b0a00fc3d08ff'
    'fc3e0600fd3f0400fe3f0300ff40010000004000000140ff00033ffe00043ffd00063efcff083dfc000a3bfbff0d39fbff0f37fbff1136fa'
    'fe1433fbfe1631fbfd192ffbfd1c2cfbfd1f29fbfc2127fcfc2424fcfc2721fcfb291ffdfb2c1cfdfb2f19fdfb3116fefb3314fefa3611ff'
    'fb370ffffb390dfffb3b0a00fc3d08fffc3e0600fd3f0400fe3f0300ff400100ff053804ff063803ff083801ff093701ff0a3700ff0c3500'
    'ff0e34ffff1033feff1232fdfe1431fdfe162ffdfe182dfdfd1b2cfcfd1d2afcfd1f28fcfd2126fcfd2323fdfc2621fdfc281ffdfc2a1dfd'
    'fc2c1bfdfd2d18fefd2f16fefd3114fefd3212fffe3310ffff340eff00350cff00360a00013609000237070003370600ff083207ff093206'
    'ff0a3205ff0c3203ff0d3103ff0e3102fe113001fe132f00fe142e00fe162dfffe182bfffe192afffe1b29fefe1d27fefe1f25fefd2124fe'
    'fe2222fefe2421fdfe251ffefe271dfefe291bfeff2a19feff2b18feff2d16fe002e14fe002f12ff013010ff02300fff03310dff04310cff'
    '05310a0006310900ff0a2e09ff0b2e08ff0c2e07ff0e2d06ff0f2d05ff102d04ff122c03fe142c02fe152b02fe172a01fe182901fe1a2800'
    'fe1b2700fe1d2500ff1e24fffe2023ffff2121ffff2320feff241eff00251dfe00261bff00281afe012818ff012a16ff022a15ff032b13ff'
    '032c12ff052c10ff052d0fff062d0d00072d0c00082d0b00ff0c2a0bff0d2a0aff0e2a09ff0f2a08ff102a07ff112a06ff132905ff142904'
    'ff162803ff172703ff182702ff1a2601ff1b2501ff1c2401ff1e2300ff1f22000020200000211f0001221d0001231c0001251bff02251aff'
    '032618ff032717ff042815ff052814ff052913ff06291100072a10ff082a0e00092a0d000a2a0c00ff0d280cff0e280bff0f280aff102809'
    'ff112808ff122708ff142706ff152705ff162605ff172604ff192503ff1a2403001b2302001c2202001d2201001e2101011f1f0101211e00'
    '01221d0002221c0002231b00032419000424180004251700052616ff06261400072713ff0827110008271100092710000a280e000b280d00'
    'ff0e260dff0f260cff10260bff11260aff122609ff132608ff142508ff15250700152506001724050018230500192304001b2203001c2103'
    '011d2002011d2002011f1f01021f1e0102201d0103211c0003221b0004221a00042318010524170006241600072415000825130009251200'
    '092611000a2610000b260f000c260e00ff0e250eff0f250dff10250cff11250b0011250a0013240900142408001524070016230700172306'
    '0018220600192205011a2104011b2004011c2003021c1f03021e1e02031e1d0203201c0104201b0104211a01052219000522180106231700'
    '072316000724150008241400092413000a2412000b2411000c2410000d240f00000e240e000f240d0010240c0011240b0013230a0013230a'
    '001423090015230800162208001722070118210601192105011a2005021b1f04021b1f04021d1e03031d1d03031e1d02041e1c02041f1b02'
    '05201a010521190106211801072217000722160108231500092314000a2313000a2313000b2312000c2311000d231000000f220f0010220e'
    '0011220d0012220c0013220b0013220b0015210a0015210a01162108011720080118200702191f0602191f06021a1e06031a1e05031c1d04'
    '041c1c04041d1c03051d1b03051e1a03061f1902061f1902072018010820170108211601092115010a2115000b2114000b2213000c221200'
    '0d2211000e2210000010210f0011210e0011210e0012210d0013210c0014200c0114200b0115200a01161f0a01171f0902171f0802181e08'
    '03181e07031a1d06031a1d06041b1c05041c1c04051c1b04051d1a04061d1a03071d1903071e1803081e1802081f1702091f16020a201501'
    '0b1f15010b2014010c2113000d2112000e2012000e211100001020100011200f0012200e0013200d0013200d01141f0c01151f0b01151f0b'
    '01161f0a02171e0902171e0903181d0803191d0703191d07041a1c06041b1c05051b1b05051c1b04061c1a04071d1903071d1903081d1803'
    '081e1703091e17020a1f16010a1f15020b1f15010c1f14010d2013000d2013000e2012000f201100001020100011200f00121f0f00131f0e'
    '00141f0d01141f0c01141f0c01151e0c02161e0a02171e0903171d0903181d0803181d0804191c07041a1c06051a1b06051b1b05061b1a05'
    '061c1a04071c1904081c1903081d1803091d1703091e17020a1e16020b1e15020c1e15010c1f14010d1f14000e1f13000e1f12010f1f1200'
    '00111e1100121e1000131e0f00131e0f01131e0e01141d0e02151d0c02151d0c02161d0b03161c0b03171c0a04171c0904181b0905181b08'
    '05191b0706191a07061a1a06071a1906071b1905081b1805091b1804091c17040a1c17030a1c16040b1d16020c1d15020c1d15020d1d1402'
    '0e1d14010e1e13010f1e1300101e120000111e1100121e1000131d1001131d0f01141d0e01141d0e02151c0d02151c0d03161c0b03161c0b'
    '04171b0a04171b0a05171b0905181a0906181a0806191a0707191907071a1906081a1806081a1806091a18050a1b17040a1b17040b1c1603'
    '0b1c16030c1c15030d1c15020d1d14020e1d14010f1d13010f1d1301101e1200'
)
LAN2 = [int(LAN2_HEX[i:i + 8], 16) for i in range(0, len(LAN2_HEX), 8)]


def fir_bank(step):
    """g2d_vsu_calc_fir_coef: the bank for a step with 19 fraction bits."""
    ratio = step >> 16
    whole, part = ratio >> 3, ratio & 7
    return 1 if whole == 0 else 1 + part if whole == 1 else 9 + (part >> 1) if whole == 2 else \
        13 if whole == 3 else 14 if whole == 4 else 15


def taps(word):
    return [((word >> (8 * k)) & 0xff) - (256 if (word >> (8 * k)) & 0x80 else 0) for k in range(4)]


def xorshift(x):
    x ^= (x << 13) & 0xffffffff
    x ^= x >> 17
    x ^= (x << 5) & 0xffffffff
    return x


def frames(seed=SEED):
    """Both frames as (luma, chroma) uint8 arrays of 480 by 640, chroma holding the pairs' bytes in memory order."""
    x = seed
    words_ = np.empty(FW * FH // 4, dtype='<u4')
    for i in range(words_.size):
        x = xorshift(x)
        words_[i] = x
    luma_a = words_.view(np.uint8).reshape(FH, FW).copy()
    i = np.arange(FW)
    for y in range(0, FH, 4):
        luma_a[y] = (i - EXACT[0] + y) & 0xff
    chroma_a = np.empty((FH, FW), dtype=np.uint8)
    for y in range(FH):
        x = xorshift(x)
        chroma_a[y, 0:FW // 2:2], chroma_a[y, 1:FW // 2:2] = x & 0xff, (x >> 8) & 0xff
        for k in range(FW // 4, FW // 2):
            x = xorshift(x)
            chroma_a[y, 2 * k], chroma_a[y, 2 * k + 1] = x & 0xff, (x >> 8) & 0xff
    right = i >= FW // 2
    row_luma = np.where(right, 16 + (i - FW // 2) * 219 // (FW // 2 - 1), FLAT[0]).astype(np.uint8)
    row_chroma = np.where(right, 128, np.where(i & 1, FLAT[2], FLAT[1])).astype(np.uint8)
    return (luma_a, chroma_a), (np.tile(row_luma, (FH, 1)), np.tile(row_chroma, (FH, 1)))


def block(c):
    """A table's coefficients (3 by 3) and constants (3), sign-extended from 13 and 20 bits."""
    std, at = PROGRAMMED[c]
    t = VENDOR[std][at:at + 12]
    sx = lambda v, bits: (v & ((1 << bits) - 1)) - ((1 << bits) if v & (1 << (bits - 1)) else 0)
    coef = np.array([[sx(t[4 * ch + k], 13) for k in range(3)] for ch in range(3)], dtype=np.int64)
    const = np.array([sx(t[4 * ch + 3], 20) for ch in range(3)], dtype=np.int64)
    return coef, const


def csc(c, y, u, v, rule):
    """ARGB8888 words (alpha 0xff) of table c applied to equal-shaped luma and chroma arrays under a rule."""
    floor, rounding = rule.split('/')
    coef, const = block(c)
    y, u, v = (np.asarray(a, dtype=np.int64) for a in (y, u, v))
    if floor == 'raised':
        y = np.maximum(y, 16)
    add = 512 if rounding == 'nearest' else 0
    out = np.full(y.shape, 0xff << 24, dtype=np.int64)
    for ch in range(3):
        value = np.clip((coef[ch, 0] * y + coef[ch, 1] * u + coef[ch, 2] * v + const[ch] + add) >> 10, 0, 255)
        out |= value << (16 - 8 * ch)
    return out.astype(np.uint32)


def crc(words_):
    return zlib.crc32(np.ascontiguousarray(words_, dtype='<u4').tobytes())


def exact_models(frame, c):
    """{rule: CRC-32} of the exact region under each candidate rule."""
    luma, chroma = frame
    y = luma[:, EXACT[0]:EXACT[1]]
    u = np.repeat(chroma[:, 0:1], EXACT[1] - EXACT[0], axis=1)
    v = np.repeat(chroma[:, 1:2], EXACT[1] - EXACT[0], axis=1)
    return {rule: crc(csc(c, y, u, v, rule)) for rule in RULES}


def chroma_row(pairs, chroma_model):
    """One row's per-pixel chroma (640 values) from its 320 pairs' values under a chroma model."""
    p = pairs.astype(np.int64)
    n = p.size
    k = np.arange(n)
    at = lambda j: p[np.clip(j, 0, n - 1)]
    if chroma_model == 'lanczos':
        odd = np.clip((-4 * at(k - 1) + 36 * p + 36 * at(k + 1) - 4 * at(k + 2) + 32) >> 6, 0, 255)
        odd[-1] = p[-1]
    else:
        odd = p
    out = np.empty(2 * n, dtype=np.int64)
    out[0::2], out[1::2] = p, odd
    return out


def conversion_row(frame, y, c, rule, chroma_model='lanczos'):
    """One whole output row of a conversion under a model."""
    luma, chroma = frame
    return csc(c, luma[y], chroma_row(chroma[y, 0::2], chroma_model), chroma_row(chroma[y, 1::2], chroma_model), rule)


def conversion_frame(frame, c, rule, chroma_model='lanczos'):
    return np.stack([conversion_row(frame, y, c, rule, chroma_model) for y in range(FH)])


def scale_line(values, ow, w=FW, chroma=False):
    """One row through the scaler model: values are the row's samples (luma: w; chroma: one component's w / 2)."""
    step = (w << 19) // ow
    if chroma:
        step_reg, bank = step, fir_bank(step >> 1)  # c_hor_step = yhstep: half the luma's 20-bit-fraction register
    else:
        step_reg, bank = step << 1, fir_bank(step)
    v = np.asarray(values, dtype=np.int64)
    n = v.size
    out = np.empty(ow, dtype=np.int64)
    for x in range(ow):
        position = x * step_reg                # 20 fraction bits
        i, phase = position >> 20, (position >> 15) & 31
        t = taps(LAN2[32 * bank + phase])
        acc = sum(t[k] * v[min(max(i - 1 + k, 0), n - 1)] for k in range(4))
        out[x] = min(max((acc + 32) >> 6, 0), 255)
    return out


def scale_model(frame, ow, c, rule=PREDICTED_RULE):
    """The scaled middle row of frame B under the scaler model."""
    luma, chroma = frame
    return csc(c, scale_line(luma[0], ow), scale_line(chroma[0, 0::2], ow, chroma=True),
               scale_line(chroma[0, 1::2], ow, chroma=True), rule)


def background_crc(ow, oh):
    return zlib.crc32(bytes([BACKGROUND]) * (ow * 4 * oh))


def padding_crc(oh, stride, ow):
    return zlib.crc32(bytes([BACKGROUND]) * ((stride - ow * 4) * oh))


def candidates(seed=SEED):
    """For frame A under table 4: {rule: (rectangle CRC-32, exact-region CRC-32)} for every candidate rule."""
    a, _ = frames(seed)
    exact = exact_models(a, 4)
    return {rule: (crc(conversion_frame(a, 4, rule)), exact[rule]) for rule in RULES}


def predicted(seed=SEED):
    """The predicted CRC-32 values of A's rectangle and exact region, each scale's flat colour and middle row, and
    every operation's padding."""
    a, b = frames(seed)
    out = []
    for k, (c, variant, ow, oh, stride, offset, frame) in enumerate(PLAN):
        o = {'padding_crc': padding_crc(oh, stride, ow)}
        if k == 0:
            full = conversion_frame(a, c, PREDICTED_RULE)
            o.update({'rule': PREDICTED_RULE, 'rect_crc': crc(full), 'exact_crc': crc(full[:, EXACT[0]:EXACT[1]])})
        elif frame:
            o['flat_value'] = int(csc(c, FLAT[0], FLAT[1], FLAT[2], PREDICTED_RULE))
            o['middle_row'] = scale_model(b, ow, c)
        out.append(o)
    return out


def words(names, values):
    return {name: f'0x{value:08x}' for name, value in zip(names, values)}


def signed(x):
    return x - (1 << 32) if x & 0x80000000 else x


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
        'rcq_status': f'0x{rcq_status:08x}', 'reset_word': f'0x{reset_word:08x}',
        'status_after': f'0x{status_after:08x}', 'status_launch': f'0x{status_launch:08x}',
        'update_after': f'0x{update_after:08x}', 'mixer_after': f'0x{mixer_after:08x}', 'irq_count': irq_count,
        'iommu_scan': scan, 'output_iova': f'0x{output_iova:08x}', 'source_iova': f'0x{source_iova:08x}',
        'iommu_attached': attached, 'iommu_prefetch': {'bank0': f'0x{prefetch0:08x}', 'bank1': f'0x{prefetch1:08x}'},
        'iommu_entry': {f'bank{b}': words(BANK, bank[4 * b:4 * b + 4]) for b in (0, 1)},
        'iommu_bypass_entry': {'usb': bypass[0], 'g2d': bypass[1]},
        'iommu_fault': words(FAULT, fault), 'attach_us': attach_us,
        'iommu_owner': {'fault_step': fault_step, 'fault_word': f'0x{fault_word:08x}',
                        'fault_read': f'0x{fault_read:08x}', 'refused_word': refused},
        'stray_interrupts': stray, 'stray_finishes': stray_finishes, 'stray_word': f'0x{stray_word:08x}',
        'surface_physical': f'0x{surface:x}', 'directory_physical': f'0x{directory:x}',
        'submit_to_launch_us': (launch - submit) / TICKS_PER_US if launch else None,
        'launch_to_irq_us': (irq - launch) / TICKS_PER_US if irq and launch else None,
        'irq_to_done_us': (done - irq) / TICKS_PER_US if done and irq else None,
        'submit_to_done_us': (done - submit) / TICKS_PER_US if done else None,
    }


def info_record(v):
    p = len(PLATFORM)
    info_magic, info_bytes, domain_entry, module_hz = v[:4]
    if info_magic != INFO_MAGIC:
        return None, 0
    if info_bytes != INFO.size:
        raise ValueError(f'ss_g2d_info is {info_bytes} bytes, not {INFO.size}.')
    entry, after = v[4:4 + p], v[4 + p:4 + 2 * p]
    local = v[4 + 2 * p:4 + 2 * p + len(LOCAL)]
    rest = v[4 + 2 * p + len(LOCAL):]
    requested, domain_on, opened, row_request_us, row_on_us, bit8, settled_supply, verify_at_us, verified_seen_us = rest[:9]
    transition = dict(zip(TRANSITION, rest[9:9 + len(TRANSITION)]))
    version = local[LOCAL.index('version')]
    return {
        'domain_entry': domain_entry, 'module_hz': module_hz,
        'entry': words(PLATFORM, entry), 'after': words(PLATFORM, after), 'local': words(LOCAL, local),
        'intid83_entry': {'enabled': bool(entry[PLATFORM.index('gicd_enable')] & INTID_BIT),
                          'pending': bool(entry[PLATFORM.index('gicd_pending')] & INTID_BIT),
                          'active': bool(entry[PLATFORM.index('gicd_active')] & INTID_BIT)},
        'version': {'ip_version': f'0x{version >> 16:04x}', 'low_fields': f'0x{version & 0xffff:04x}'},
        'open_us': (opened - requested) / TICKS_PER_US if opened else None,
        'transition_commands': transition['commands'],
        'supply': {'verify_at_us': verify_at_us, 'verified_seen_us': verified_seen_us, 'settled_supply': settled_supply,
                   'row_request_us': row_request_us, 'row_on_us': row_on_us, 'pck_bit8': bit8},
    }, requested


def rate(pixels, us):
    return round(pixels / us, 1) if us else None


def conversion(o, raw, frame, rows, table):
    """A conversion's matches: the exact region's rule, the rectangle's model, the sampled rows' chroma models."""
    c = o['csc']
    exact = exact_models(frame, c)
    o['exact_rule'] = next((rule for rule, value in exact.items() if value == raw['exact_crc']), None)
    o['board_rule'] = RULES[raw['rule'] - 1] if 1 <= raw['rule'] <= 4 else None
    o['rule_agrees'] = o['exact_rule'] == o['board_rule']
    o['mismatch'] = dict(zip(RULES, raw['mismatch']))
    o['exact_crc'] = f'0x{raw["exact_crc"]:08x}'
    o['rect_rule'] = next((rule for rule, (value, _) in table.items() if value == raw['rect_crc']), None)
    rule = o['board_rule'] or PREDICTED_RULE
    sampled = []
    for k, y in enumerate(CONV_ROWS):
        got = np.frombuffer(rows[k], dtype='<u4')
        entry = {'row': y}
        for chroma_model in ('lanczos', 'nearest'):
            want = conversion_row(frame, y, c, rule, chroma_model)
            bad = np.nonzero(got != want)[0]
            entry[chroma_model] = {'mismatches': int(bad.size), 'first': int(bad[0]) if bad.size else None}
        entry['chroma_model'] = next((m for m in ('lanczos', 'nearest') if not entry[m]['mismatches']), None)
        sampled.append(entry)
    o['sampled_rows'] = sampled
    o['chroma_model'] = sampled[0]['chroma_model'] if sampled[0]['chroma_model'] == sampled[1]['chroma_model'] else None


def scale(o, raw, frame, row):
    """A scale's checks: flat region, rows, monotone ramp and the ramp's distance from the scaler model."""
    c, ow = o['csc'], o['ow']
    flat = int(csc(c, FLAT[0], FLAT[1], FLAT[2], PREDICTED_RULE))
    got = np.frombuffer(row, dtype='<u4')
    model = scale_model(frame, ow, c)
    ramp = slice(ow // 2 + 9, ow - 4)
    diff = np.abs(got[ramp].view(np.uint8).reshape(-1, 4).astype(int) - model[ramp].view(np.uint8).reshape(-1, 4).astype(int))
    o.update({
        'flat_value': {'board': f'0x{raw["flat_value"]:08x}', 'expected': f'0x{flat:08x}'},
        'flat_mismatch': raw['flat_mismatch'],
        'rows_differ': raw['rows_differ'], 'edge_rows_differ': [bool(raw['edge_rows'] >> k & 1) for k in range(4)],
        'mono_decreases': dict(zip('rgb', raw['mono'])),
        'ramp_max_difference': int(diff[:, :3].max()), 'ramp_alpha_ok': bool((diff[:, 3] == 0).all()),
        'middle_row_equals_model': bool((got == model).all()),
    })
    o['flat_exact'] = raw['flat_mismatch'] == 0 and raw['flat_value'] == flat
    o['rows_identical'] = raw['rows_differ'] == 0
    o['monotone'] = not any(raw['mono'])
    o['within_2'] = o['ramp_max_difference'] <= 2 and o['ramp_alpha_ok']


def decode(payload):
    """A dict of the g2d-convert record; ValueError if the payload is malformed."""
    if len(payload) != SIZE:
        raise ValueError(f'g2d-convert result is {len(payload)} bytes, not {SIZE}.')
    h = HEAD.unpack_from(payload)
    magic, stage, status, open_result, close_result, fault_step, fault_value, ops_run = h[:8]
    seed, main_core, clock_ready, stopped, start_ticks, clock_ticks = h[8:14]
    source_before, source_after, cpu0, cpu1, cpu2, cpu_crc, cpu_exact, cpu_rule = h[14:22]
    if magic != MAGIC:
        raise ValueError('g2d-convert result has a wrong magic.')
    record = {'stage': stage, 'status': STATUS.get(status, status), 'open_result': signed(open_result),
              'close_result': signed(close_result), 'fault_step': fault_step, 'fault_value': f'0x{fault_value:08x}',
              'ops_run': ops_run, 'stopped_after_a': bool(stopped), 'seed': f'0x{seed:08x}', 'main_core': main_core,
              'clock': {'ready': bool(clock_ready), 'line': h[22].split(b'\0', 1)[0].decode('ascii', 'replace'),
                        'ready_after_us': (clock_ticks - start_ticks) / TICKS_PER_US if clock_ticks else None}}
    a, b = frames(seed)
    table = candidates(seed)
    expected_source = zlib.crc32(b''.join(np.ascontiguousarray(p).tobytes() for p in (a[0], a[1], b[0], b[1])))
    record['source'] = {'before_matches': source_before == expected_source,
                        'unchanged': source_after == source_before if status in (1, 6) else None}
    cpu_us = [t / TICKS_PER_US for t in (cpu0, cpu1, cpu2)]
    record['cpu'] = {'rule': cpu_rule, 'runs_us': cpu_us, 'first_us': cpu_us[0], 'best_us': min(cpu_us) if cpu0 else None,
                     'mpixel_per_s_best': rate(FW * FH, min(cpu_us)) if cpu0 else None,
                     'mpixel_per_s_first': rate(FW * FH, cpu_us[0]) if cpu0 else None, 'crc': f'0x{cpu_crc:08x}',
                     'equals_model': cpu_crc == table[PREDICTED_RULE][0] and cpu_exact == table[PREDICTED_RULE][1]}
    info, requested = info_record(INFO.unpack_from(payload, HEAD.size))
    record['info'] = info
    cl = CLOSED.unpack_from(payload, HEAD.size + INFO.size)
    record['closed'] = {'g2d_clk': f'0x{cl[0]:08x}', 'g2d_bgr': f'0x{cl[1]:08x}', 'de_sys_bgr': f'0x{cl[2]:08x}',
                        'pck1_status': f'0x{cl[3]:08x}', 'de_di_open': cl[4]} if info else None
    at = HEAD.size + INFO.size + CLOSED.size
    ops, scale_at = [], ROWS_AT + CONVERSIONS * 2 * ROW
    for k in range(OPS):
        r = OP.unpack_from(payload, at)
        raw = dict(zip(('result', 'csc', 'w', 'h', 'ow', 'oh', 'out_offset', 'out_stride', 'rect_crc', 'exact_crc',
                        'padding_crc', 'alpha_ff', 'alpha_other'), r[:13]))
        raw.update({'mismatch': list(r[13:17]), 'rule': r[17], 'flat_value': r[18], 'flat_mismatch': r[19],
                    'rows_differ': r[20], 'edge_rows': r[21], 'mono': list(r[22:25]), 'kind': r[25],
                    'variant': r[26]})
        c, variant, ow, oh, stride, offset, frame = PLAN[k]
        o = {'run': k < ops_run, 'name': VARIANTS[variant] if not frame else f'scale {ow}x{oh}',
             'result': signed(raw['result']), 'csc': raw['csc'], 'table': CSC_NAMES.get(raw['csc']),
             'ow': raw['ow'], 'oh': raw['oh']}
        o['done'] = done_record(payload, at + OP.size)
        if o['run']:
            o['plan_matches'] = (raw['csc'], raw['variant'], raw['ow'], raw['oh'], raw['out_stride'], raw['out_offset'],
                                 raw['kind'], raw['w'], raw['h']) == (*PLAN[k], FW, FH)
            o['rect_crc'] = f'0x{raw["rect_crc"]:08x}'
            o['untouched'] = raw['rect_crc'] == background_crc(raw['ow'], raw['oh'])
            o['padding_unchanged'] = raw['padding_crc'] == padding_crc(raw['oh'], raw['out_stride'], raw['ow'])
            o['alpha_ff'] = raw['alpha_ff'] == raw['ow'] * raw['oh']
            o['alpha_other'] = f'0x{raw["alpha_other"]:08x}' if raw['alpha_ff'] != raw['ow'] * raw['oh'] else None
            d = o['done']
            us = d['launch_to_irq_us'] if d else None
            o['engine_us'] = us
            o['engine_mpixel_per_s'] = {'input': rate(FW * FH, us), 'output': rate(raw['ow'] * raw['oh'], us)}
            if d:
                # The CPU's share of the operation: the owner's work before the launch and after the interrupt.
                o['cpu_us'] = (d['submit_to_launch_us'] or 0) + (d['irq_to_done_us'] or 0)
            if not frame:
                rows = [payload[ROWS_AT + (2 * k + j) * ROW:ROWS_AT + (2 * k + j + 1) * ROW] for j in range(2)]
                conversion(o, raw, a, rows, table)
            else:
                row = payload[scale_at:scale_at + raw['ow'] * 4]
                scale_at += raw['ow'] * 4
                scale(o, raw, b, row)
        ops.append(o)
        at += OP.size + DONE.size
    record['ops'] = ops
    first = ops[0]
    record['a_predicted'] = bool(first['run'] and first['result'] == 1 and first.get('board_rule') == PREDICTED_RULE and
                                 first['rect_crc'] == f'0x{table[PREDICTED_RULE][0]:08x}')
    record['variants'] = {o['name']: (o.get('rect_rule') or 'unmatched') if o['run'] else 'not run' for o in ops[1:5]}
    scales = [o for o in ops[5:] if o['run']]
    record['scales_predicted'] = len(scales) == 2 and all(o['result'] == 1 and o['flat_exact'] and o['rows_identical']
                                                          and o['monotone'] and o['within_2'] for o in scales)
    record['predicted'] = status == 1 and record['a_predicted'] and record['scales_predicted'] and \
        all(o['padding_unchanged'] and o['alpha_ff'] and o['plan_matches'] for o in ops if o['run']) and \
        bool(record['source']['before_matches'] and record['source']['unchanged'])
    return record
