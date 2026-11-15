#!/usr/bin/env python3
"""Host side of g2d-convert: the decoder of its result and the models of its six operations
(docs/g2d-os.md#packed-conversion-stage).

tools/board.py loads this module with --decoder. decode() reads the result that ss_finish returns (struct
g2d_convert_result in apps/g2d-convert/main.c, which embeds struct ss_g2d_info, struct ss_g2d_closed and six struct
ss_g2d_done from include/sunstone_g2d.h) and names every recorded word.

Frames. frames() rebuilds the stage's frames from the seed as the application does. Frame P is stage 9's luma-sweep
frame from the new seed: random luma with every fourth row replaced by (x - 8 + y) mod 256, one chroma pair per row in
the left half and one pair per two pixels in the right half; it is stored as YUYV and as UYVY. Frame R is constant
along each row, with luma 7y, U 64 + 5y and V 200 + 3y (mod 256) in row y; it is scaled to 224 by 250. Frame Q is the
seed's words.

The model. convert_model() reads each 4-byte pair in the order of the format code (0x20 YUYV, 0x21 UYVY, 0x22 YVYU,
0x23 VYUY: the vendor names read from the most significant byte), scales each component through the scaler and applies
the BT.601 limited table (tools/g2d-csc-tables.py, TABLES) with luma kept and the result clamped. The scaler: the
horizontal pass with the vendor's Lanczos banks at each output's position x * step (20 fraction bits) rounded to the
nearest of 32 phases (stage 9), taps at positions -1 to +2 with the edges repeated, rounded to nearest and clamped;
the chroma pairs with the half step and the half step's bank; then the vertical pass the same way with the linear bank
(the vendor's packed setting) or, with the Lanczos vertical variant, the Lanczos bank of the vertical step, for luma
and chroma alike (the scaler has one vertical bank). An unscaled row's last pixel takes its own chroma pair (stage 7).
The prediction of every operation is this model's output; the decoder also names which candidate matches: the byte
order (the application's counts), the chroma interpolation in two sampled rows, the vertical bank and phase rounding
of the frame-R column, and for frame Q the bank, the pass order (vertical first, or the horizontal sum kept whole) and
the phase rounding. The edge rows and columns are reported separately.

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
OP = struct.Struct('<24I')
DONE = struct.Struct('<9I8I2I10I5I6I7Q8I')
OPS = 6
FW, FH = 640, 480
SW, SH = 224, 224
RH = 250                        # frame R's output height
ROW = FW * 4
COLUMN = 112
ROWS_AT = HEAD.size + INFO.size + CLOSED.size + OPS * (OP.size + DONE.size)
COLUMNS_AT = ROWS_AT + 2 * 2 * ROW
SCALED_AT = COLUMNS_AT + 2 * RH * 4
SIZE = SCALED_AT + 2 * SW * SH * 4
MAGIC, INFO_MAGIC, DONE_MAGIC = 0x56433247, 0x54443247, 0x44443247
STATUS = {1: 'done', 2: 'refused', 3: 'close failed', 4: 'run failed', 5: 'no surface'}
FAULT = ('bank_status0', 'bank_status1', 'l1_status0', 'l1_status1', 'l2_status0', 'l2_status1',
         'micro_status', 'micro_low', 'micro_high', 'micro_data')
BANK = ('enable', 'int_status', 'l1_status', 'l2_status')
TICKS_PER_US = 24
INTID_BIT = 1 << (83 % 32)
SEED = 0xbb67ae85
EXACT = (8, 312)
CONV_ROWS = (100, 333)
BACKGROUND = 0x5a
CSC = 1                         # SS_G2D_CSC_BT601_LIMITED
UNSCALED, RAMP, RANDOM = 0, 1, 2
# apps/g2d-convert/main.c plan: the format, the frame, its kind, the output size, stride and offset, the variant.
PLAN = ((0x20, 'p', UNSCALED, 640, 480, 2624, 0x000000, 0), (0x21, 'pu', UNSCALED, 640, 480, 2624, 0x000000, 0),
        (0x20, 'r', RAMP, 224, 250, 960, 0x140000, 0), (0x20, 'r', RAMP, 224, 250, 960, 0x180000, 1),
        (0x20, 'q', RANDOM, 224, 224, 960, 0x1c0000, 0), (0x20, 'q', RANDOM, 224, 224, 960, 0x200000, 1))
NAMES = ('P YUYV unscaled', 'P UYVY unscaled', 'R 250 linear', 'R 250 Lanczos', 'Q 224 linear', 'Q 224 Lanczos')
RULES = ('kept', 'raised')
PREDICTED_RULE = 'kept'

# tools/g2d-csc-tables.py's tables in A733_G2D_CSC_* order (checks/g2d-csc-tables.py compares this copy with it).
TABLES = {1: [1192, 0, 1634, -227712, 1192, -401, -832, 139264, 1192, 2066, 0, -283008],
          2: [1024, 0, 1436, -183296, 1024, -352, -731, 139136, 1024, 1815, 0, -231808],
          3: [1192, 0, 1836, -253568, 1192, -218, -546, 79232, 1192, 2163, 0, -295424],
          4: [1024, 0, 1613, -205952, 1024, -192, -479, 86400, 1024, 1900, 0, -242688]}
# The vendor block each table programmed before the tables were derived, as (array, first word).
VENDOR_BLOCKS = {1: ('601', 0), 2: ('601', 24), 3: ('709', 0), 4: ('709', 24)}
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


def sx(value, bits):
    return (value & ((1 << bits) - 1)) - ((1 << bits) if value & (1 << (bits - 1)) else 0)


def block(c, source='derived'):
    """A table's coefficients (3 by 3) and constants (3), sign-extended from 13 and 20 bits: the derived table, or the
    vendor block it replaced."""
    if source == 'derived':
        t = TABLES[c]
    else:
        std, at = VENDOR_BLOCKS[c]
        t = VENDOR[std][at:at + 12]
    coef = np.array([[sx(t[4 * ch + k], 13) for k in range(3)] for ch in range(3)], dtype=np.int64)
    const = np.array([sx(t[4 * ch + 3], 20) for ch in range(3)], dtype=np.int64)
    return coef, const


def csc(c, y, u, v, rule=PREDICTED_RULE, source='derived'):
    """ARGB8888 words (alpha 0xff) of table c applied to equal-shaped luma and chroma arrays: luma kept or raised to 16
    before the table."""
    coef, const = block(c, source)
    y, u, v = (np.asarray(a, dtype=np.int64) for a in (y, u, v))
    if rule == 'raised':
        y = np.maximum(y, 16)
    out = np.full(y.shape, 0xff << 24, dtype=np.int64)
    for ch in range(3):
        value = np.clip((coef[ch, 0] * y + coef[ch, 1] * u + coef[ch, 2] * v + const[ch]) >> 10, 0, 255)
        out |= value << (16 - 8 * ch)
    return out.astype(np.uint32)


def crc(words_):
    return zlib.crc32(np.ascontiguousarray(words_, dtype='<u4').tobytes())


# Packed frames. frames() rebuilds frame P as stage 9's luma-sweep representation, (luma, chroma) arrays of 480 by
# 640 with chroma holding each pair's U and V bytes in memory order, and the four stored frames as bytes.
ORDERS = ('yuyv', 'uyvy', 'yvyu', 'vyuy')
# The byte positions of Y0, U, Y1 and V in a pair for each order (main.c order_bytes), the order of codes 0x20 to 0x23.
POSITIONS = ((0, 1, 2, 3), (1, 0, 3, 2), (0, 3, 2, 1), (1, 2, 3, 0))
# g2d_scal.c linearcoefftab32: phase p has taps 0, 64 - 2p, 2p, 0, lowest byte first.
LINEAR = [(2 * p) << 16 | (64 - 2 * p) << 8 for p in range(32)]


def frames(seed=SEED):
    """Frame P as (luma, chroma) arrays, and the source region's four frames as 480 by 1280 uint8 arrays: P as YUYV and
    as UYVY, R and Q."""
    x = seed
    words_ = np.empty(FW * FH // 4, dtype='<u4')
    for i in range(words_.size):
        x = xorshift(x)
        words_[i] = x
    luma = words_.view(np.uint8).reshape(FH, FW).copy()
    i = np.arange(FW)
    for y in range(0, FH, 4):
        luma[y] = (i - EXACT[0] + y) & 0xff
    chroma = np.empty((FH, FW), dtype=np.uint8)
    for y in range(FH):
        x = xorshift(x)
        chroma[y, 0:FW // 2:2], chroma[y, 1:FW // 2:2] = x & 0xff, (x >> 8) & 0xff
        for k in range(FW // 4, FW // 2):
            x = xorshift(x)
            chroma[y, 2 * k], chroma[y, 2 * k + 1] = x & 0xff, (x >> 8) & 0xff
    yuyv = np.empty((FH, 2 * FW), dtype=np.uint8)
    yuyv[:, 0::4], yuyv[:, 1::4], yuyv[:, 2::4], yuyv[:, 3::4] = luma[:, 0::2], chroma[:, 0::2], luma[:, 1::2], \
        chroma[:, 1::2]
    uyvy = np.empty_like(yuyv)
    uyvy[:, 0::4], uyvy[:, 1::4], uyvy[:, 2::4], uyvy[:, 3::4] = yuyv[:, 1::4], yuyv[:, 0::4], yuyv[:, 3::4], \
        yuyv[:, 2::4]
    rows = np.arange(FH, dtype=np.int64)
    ramp = np.empty((FH, 2 * FW), dtype=np.uint8)
    ramp[:, 0::2] = ((7 * rows) & 255)[:, None]
    ramp[:, 1::4] = ((64 + 5 * rows) & 255)[:, None]
    ramp[:, 3::4] = ((200 + 3 * rows) & 255)[:, None]
    q = np.empty(2 * FW * FH // 4, dtype='<u4')
    for k in range(q.size):
        x = xorshift(x)
        q[k] = x
    return (luma, chroma), {'p': yuyv, 'pu': uyvy, 'r': ramp, 'q': q.view(np.uint8).reshape(FH, 2 * FW).copy()}


def unpack(stored, order):
    """A stored packed frame's planes read in an order: luma (h by w), U and V (h by w / 2), as int64 arrays."""
    b = POSITIONS[ORDERS.index(order)]
    pairs = stored.reshape(stored.shape[0], -1, 4).astype(np.int64)
    luma = np.empty((stored.shape[0], 2 * pairs.shape[1]), dtype=np.int64)
    luma[:, 0::2], luma[:, 1::2] = pairs[:, :, b[0]], pairs[:, :, b[2]]
    return luma, pairs[:, :, b[1]], pairs[:, :, b[3]]


def resample(a, m, step_reg, bank, phase='nearest', shift=6):
    """One scaler pass along axis 0 of a: m outputs at positions x * step_reg (20 fraction bits) rounded to the nearest
    of 32 phases (or truncated), 4 taps at positions -1 to +2 with the edges repeated, the sum rounded to nearest and
    shifted down by shift bits and clamped, or kept whole when shift is 0."""
    x = np.arange(m, dtype=np.int64) * step_reg
    q = (x + (1 << 14)) >> 15 if phase == 'nearest' else x >> 15
    i, p = q >> 5, q & 31
    t = np.array([taps(bank[k]) for k in range(32)], dtype=np.int64)[p]
    n = a.shape[0]
    acc = sum(t[:, k].reshape((-1,) + (1,) * (a.ndim - 1)) * a[np.clip(i - 1 + k, 0, n - 1)] for k in range(4))
    return np.clip((acc + (1 << (shift - 1))) >> shift, 0, 255) if shift else acc


def scale_component(plane, ow, oh, hreg, hbank, vreg, vbank, order='h-then-v', phase='nearest'):
    """The scaler model for one component: horizontal then vertical, each rounded and clamped; or vertical first; or
    the horizontal sum kept whole into the vertical pass ('wide')."""
    if order == 'v-then-h':
        mid = resample(plane, oh, vreg, vbank, phase)
        return resample(mid.T, ow, hreg, hbank, phase).T
    first = 0 if order == 'wide' else 6
    mid = resample(plane.T, ow, hreg, hbank, phase, first).T
    return resample(mid, oh, vreg, vbank, phase, 12 if order == 'wide' else 6)


def convert_model(stored, fmt_order, ow, oh, c=CSC, vertical='linear', order='h-then-v', phase='nearest'):
    """A packed frame converted and scaled under the engine's model: oh by ow ARGB8888 words."""
    luma, u, v = unpack(stored, fmt_order)
    h, w = luma.shape
    hstep, vstep = (w << 19) // ow, (h << 19) // oh
    hbank = LAN2[32 * fir_bank(hstep):][:32]
    cbank = LAN2[32 * fir_bank(hstep >> 1):][:32]
    vbank = LINEAR if vertical == 'linear' else LAN2[32 * fir_bank(vstep):][:32]
    y = scale_component(luma, ow, oh, hstep << 1, hbank, vstep << 1, vbank, order, phase)
    cu = scale_component(u, ow, oh, hstep, cbank, vstep << 1, vbank, order, phase)
    cv = scale_component(v, ow, oh, hstep, cbank, vstep << 1, vbank, order, phase)
    if (ow, oh) == (w, h):
        # An unscaled row's last pixel takes its own pair (docs/g2d-os.md, the first conversion stage).
        cu[:, -1], cv[:, -1] = u[:, -1], v[:, -1]
    return csc(c, y, cu, cv)


def conversion_frame(frame, c, rule=PREDICTED_RULE, chroma_model='lanczos', source='derived'):
    """Frame P's unscaled conversion from its (luma, chroma) arrays (checks/g2d-csc-tables.py uses it)."""
    return csc(c, *conversion_inputs(frame, chroma_model), rule, source)


def conversion_inputs(frame, chroma_model='lanczos'):
    """Every pixel's (Y, U, V) at the table's input for frame P unscaled, as three 480 by 640 arrays."""
    luma, chroma = frame
    def row(pairs):
        p = pairs.astype(np.int64)
        n, k = p.shape[1], np.arange(p.shape[1])
        at = lambda j: p[:, np.clip(j, 0, n - 1)]
        odd = np.clip((-4 * at(k - 1) + 36 * p + 36 * at(k + 1) - 4 * at(k + 2) + 32) >> 6, 0, 255) \
            if chroma_model == 'lanczos' else p.copy()
        odd[:, -1] = p[:, -1]
        out = np.empty((p.shape[0], 2 * n), dtype=np.int64)
        out[:, 0::2], out[:, 1::2] = p, odd
        return out
    return luma.astype(np.int64), row(chroma[:, 0::2]), row(chroma[:, 1::2])


def background_crc(ow, oh):
    return zlib.crc32(bytes([BACKGROUND]) * (ow * 4 * oh))


def padding_crc(oh, stride, ow):
    return zlib.crc32(bytes([BACKGROUND]) * ((stride - ow * 4) * oh))


def interior(words_):
    return crc(words_[2:-2, 2:-2])


def predicted(seed=SEED):
    """Each operation's predicted output under the engine's model, with its CRC-32 values."""
    _, stored = frames(seed)
    out = []
    for fmt, frame, kind, ow, oh, stride, offset, variant in PLAN:
        vertical = 'lanczos' if variant else 'linear'
        model = convert_model(stored[frame], ORDERS[fmt - 0x20], ow, oh, vertical=vertical)
        o = {'model': model, 'rect_crc': crc(model), 'interior_crc': interior(model),
             'padding_crc': padding_crc(oh, stride, ow), 'order': ORDERS[fmt - 0x20]}
        if kind == UNSCALED:
            o['exact_crc'] = crc(model[:, EXACT[0]:EXACT[1]])
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


def unscaled(o, raw, rows, want, frame_p):
    """An unscaled conversion's matches: the board's byte order, the rectangle against the model, the sampled rows'
    chroma models."""
    o['mismatch'] = dict(zip(ORDERS, raw['mismatch']))
    o['board_order'] = ORDERS[raw['order'] - 1] if 1 <= raw['order'] <= 4 else None
    o['predicted_order'] = want['order']
    o['exact_crc'] = f'0x{raw["exact_crc"]:08x}'
    o['exact_predicted'] = raw['exact_crc'] == want['exact_crc']
    sampled = []
    for k, y in enumerate(CONV_ROWS):
        got = np.frombuffer(rows[k], dtype='<u4')
        entry = {'row': y}
        for chroma_model in ('lanczos', 'nearest'):
            model = csc(CSC, *(a[y] for a in conversion_inputs(frame_p, chroma_model)))
            bad = np.nonzero(got != model)[0]
            entry[chroma_model] = {'mismatches': int(bad.size), 'first': int(bad[0]) if bad.size else None}
        entry['chroma_model'] = next((m for m in ('lanczos', 'nearest') if not entry[m]['mismatches']), None)
        sampled.append(entry)
    o['sampled_rows'] = sampled
    o['chroma_model'] = sampled[0]['chroma_model'] if sampled[0]['chroma_model'] == sampled[1]['chroma_model'] else None


def edges(got, model):
    """Mismatched words of an output against a model in its two top and bottom rows, its two left and right columns
    (rows 2 to oh - 3) and its interior."""
    bad = got != model
    return {'top_rows': [int(bad[r].sum()) for r in (0, 1)], 'bottom_rows': [int(bad[r].sum()) for r in (-2, -1)],
            'left_columns': [int(bad[2:-2, c].sum()) for c in (0, 1)],
            'right_columns': [int(bad[2:-2, c].sum()) for c in (-2, -1)], 'interior': int(bad[2:-2, 2:-2].sum())}


def ramp(o, raw, column, want, stored):
    """A frame-R scale: column 112 against the vertical candidates; its edge rows reported separately."""
    got = np.frombuffer(column, dtype='<u4')
    o['nonconstant_rows'] = raw['nonconstant_rows']
    models = {}
    for vertical in ('linear', 'lanczos'):
        for phase in ('nearest', 'trunc'):
            col = convert_model(stored['r'], 'yuyv', SW, RH, vertical=vertical, phase=phase)[:, COLUMN]
            models[f'{vertical}/{phase}'] = col
    o['column_models'] = [name for name, col in models.items() if np.array_equal(col, got)]
    model = want['model'][:, COLUMN]
    bad = np.nonzero(got != model)[0]
    o['column_mismatch_rows'] = [int(r) for r in bad[:16]]
    o['edge_rows'] = {f'row {r}': {'board': f'0x{int(got[r]):08x}', 'model': f'0x{int(model[r]):08x}',
                                   'equal': bool(got[r] == model[r])} for r in (0, 1, RH - 2, RH - 1)}
    o['column_equals_model'] = not bad.size


def random_frame(o, raw, scaled, want, stored):
    """A frame-Q scale: the whole output against the scaler candidates, with the edges reported separately."""
    got = np.frombuffer(scaled, dtype='<u4').reshape(SH, SW)
    o['output_crc'] = f'0x{crc(got):08x}'
    o['output_equals_model'] = bool(np.array_equal(got, want['model']))
    o['edges'] = edges(got, want['model'])
    whole, inner = [], []
    for vertical in ('linear', 'lanczos'):
        for order in ('h-then-v', 'v-then-h', 'wide'):
            for phase in ('nearest', 'trunc'):
                model = convert_model(stored['q'], 'yuyv', SW, SH, vertical=vertical, order=order, phase=phase)
                name = f'{vertical}/{order}/{phase}'
                if np.array_equal(model, got):
                    whole.append(name)
                if np.array_equal(model[2:-2, 2:-2], got[2:-2, 2:-2]):
                    inner.append(name)
    o['output_models'], o['interior_models'] = whole, inner


def decode(payload):
    """A dict of the g2d-convert record; ValueError if the payload is malformed."""
    if len(payload) != SIZE:
        raise ValueError(f'g2d-convert result is {len(payload)} bytes, not {SIZE}.')
    h = HEAD.unpack_from(payload)
    magic, stage, status, open_result, close_result, fault_step, fault_value, ops_run = h[:8]
    seed, main_core, clock_ready, _, start_ticks, clock_ticks = h[8:14]
    source_before, source_after, cpu0, cpu1, cpu2, cpu_crc, cpu_exact, _ = h[14:22]
    if magic != MAGIC:
        raise ValueError('g2d-convert result has a wrong magic.')
    record = {'stage': stage, 'status': STATUS.get(status, status), 'open_result': signed(open_result),
              'close_result': signed(close_result), 'fault_step': fault_step, 'fault_value': f'0x{fault_value:08x}',
              'ops_run': ops_run, 'seed': f'0x{seed:08x}', 'main_core': main_core,
              'clock': {'ready': bool(clock_ready), 'line': h[22].split(b'\0', 1)[0].decode('ascii', 'replace'),
                        'ready_after_us': (clock_ticks - start_ticks) / TICKS_PER_US if clock_ticks else None}}
    frame_p, stored = frames(seed)
    want = predicted(seed)
    expected_source = zlib.crc32(b''.join(stored[k].tobytes() for k in ('p', 'pu', 'r', 'q')))
    record['source'] = {'before_matches': source_before == expected_source,
                        'unchanged': source_after == source_before if status == 1 else None}
    cpu_us = [t / TICKS_PER_US for t in (cpu0, cpu1, cpu2)]
    record['cpu'] = {'runs_us': cpu_us, 'first_us': cpu_us[0], 'best_us': min(cpu_us) if cpu0 else None,
                     'mpixel_per_s_best': rate(FW * FH, min(cpu_us)) if cpu0 else None,
                     'mpixel_per_s_first': rate(FW * FH, cpu_us[0]) if cpu0 else None, 'crc': f'0x{cpu_crc:08x}',
                     'equals_model': (cpu_crc, cpu_exact) == (want[0]['rect_crc'], want[0]['exact_crc'])}
    info, _ = info_record(INFO.unpack_from(payload, HEAD.size))
    record['info'] = info
    cl = CLOSED.unpack_from(payload, HEAD.size + INFO.size)
    record['closed'] = {'g2d_clk': f'0x{cl[0]:08x}', 'g2d_bgr': f'0x{cl[1]:08x}', 'de_sys_bgr': f'0x{cl[2]:08x}',
                        'pck1_status': f'0x{cl[3]:08x}', 'de_di_open': cl[4]} if info else None
    at = HEAD.size + INFO.size + CLOSED.size
    ops = []
    for k in range(OPS):
        r = OP.unpack_from(payload, at)
        raw = dict(zip(('result', 'csc', 'format', 'variant', 'w', 'h', 'ow', 'oh', 'out_offset', 'out_stride',
                        'rect_crc', 'exact_crc', 'interior_crc', 'padding_crc', 'alpha_ff', 'alpha_other'), r[:16]))
        raw.update({'mismatch': list(r[16:20]), 'order': r[20], 'nonconstant_rows': r[21], 'kind': r[22]})
        fmt, frame, kind, ow, oh, stride, offset, variant = PLAN[k]
        o = {'run': k < ops_run, 'name': NAMES[k], 'result': signed(raw['result']), 'ow': raw['ow'], 'oh': raw['oh'],
             'format': f'0x{raw["format"]:02x}', 'vertical': 'lanczos' if raw['variant'] else 'linear'}
        o['done'] = done_record(payload, at + OP.size)
        if o['run']:
            o['plan_matches'] = (raw['csc'], raw['format'], raw['variant'], raw['kind'], raw['ow'], raw['oh'],
                                 raw['out_stride'], raw['out_offset'], raw['w'], raw['h']) == \
                (CSC, fmt, variant, kind, ow, oh, stride, offset, FW, FH)
            o['rect_crc'] = f'0x{raw["rect_crc"]:08x}'
            o['rect_predicted'] = raw['rect_crc'] == want[k]['rect_crc']
            o['interior_crc'] = f'0x{raw["interior_crc"]:08x}'
            o['interior_predicted'] = raw['interior_crc'] == want[k]['interior_crc']
            o['untouched'] = raw['rect_crc'] == background_crc(raw['ow'], raw['oh'])
            o['padding_unchanged'] = raw['padding_crc'] == padding_crc(raw['oh'], raw['out_stride'], raw['ow'])
            o['alpha_ff'] = raw['alpha_ff'] == raw['ow'] * raw['oh']
            o['alpha_other'] = f'0x{raw["alpha_other"]:08x}' if not o['alpha_ff'] else None
            d = o['done']
            us = d['launch_to_irq_us'] if d else None
            o['engine_us'] = us
            o['engine_mpixel_per_s'] = {'input': rate(FW * FH, us), 'output': rate(raw['ow'] * raw['oh'], us)}
            if d:
                # The CPU's share of the operation: the owner's work before the launch and after the interrupt.
                o['cpu_us'] = (d['submit_to_launch_us'] or 0) + (d['irq_to_done_us'] or 0)
            if kind == UNSCALED:
                rows = [payload[ROWS_AT + (2 * k + j) * ROW:ROWS_AT + (2 * k + j + 1) * ROW] for j in range(2)]
                unscaled(o, raw, rows, want[k], frame_p)
                o['predicted'] = o['rect_predicted'] and o['board_order'] == o['predicted_order']
            elif kind == RAMP:
                at_column = COLUMNS_AT + (k - 2) * RH * 4
                ramp(o, raw, payload[at_column:at_column + RH * 4], want[k], stored)
                o['predicted'] = o['rect_predicted'] and o['column_equals_model'] and not raw['nonconstant_rows']
            else:
                at_scaled = SCALED_AT + (k - 4) * SW * SH * 4
                random_frame(o, raw, payload[at_scaled:at_scaled + SW * SH * 4], want[k], stored)
                o['predicted'] = o['rect_predicted'] and o['output_equals_model']
        ops.append(o)
        at += OP.size + DONE.size
    record['ops'] = ops
    record['predicted'] = status == 1 and ops_run == OPS and record['cpu']['equals_model'] and \
        all(o['result'] == 1 and o['predicted'] and o['padding_unchanged'] and o['alpha_ff'] and o['plan_matches']
            for o in ops) and bool(record['source']['before_matches'] and record['source']['unchanged'])
    return record
