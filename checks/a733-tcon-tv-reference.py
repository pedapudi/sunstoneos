#!/usr/bin/env python3
"""TV0 portion of the single composed HDMI source comparison."""
from pathlib import Path


def verify_tv(record, manual):
    text = Path(manual).read_text()
    for anchor in ("TV_SRC_CTL_REG", "VDBIST_MODE", "PIXELDEPTH_MODE",
                   "TV_FIFO_UNDER_FLOW", "TCON_TV0_HDMIPHY_CCU_CK_SEL"):
        assert anchor in text, anchor
    last = {a: v for a, width, v, time in record["writes"] if width == 4}
    base, top = 0x05730000, 0x05510000
    if record["mode"] == 4:
        w, h, ht, vt, hs, hb, vb = 1280, 720, 1650, 750, 40, 220, 20
    else:
        w, h, ht, vt, hs, hb, vb = 1920, 1080, 2200, 1125, 44, 148, 36
    expected = {
        0x40: (7, 1), 0x90: (0x800ffff0, 0x80000000 | min(vt-h-5, 31) << 4),
        0x98: (0x8001ffff, vt*2), 0x9c: (0x7fff7fff, (w-1) << 16 | (h-1)),
        0xa0: (0xffff7fff, (ht-1) << 16 | (hs+hb-1)),
        0xa4: (0x7fff, vb+4), 0xa8: (0x0fff0fff, (hs-1) << 16 | 4),
        0x88: (0x0f000000, 0x07000000), 0x8c: (0x0f000000, 0x0f000000),
        0x340: (7, 1),
    }
    for offset, (mask, value) in expected.items():
        assert last[base+offset] & mask == value, hex(offset)
    assert last[top] & 0xa == 0
    assert last[top+0x20] & 0x10100000 == 0x10100000
    assert last[base] & 0x80000031 == 0x80000000
    assert last[base+0xa4] & 0xffff8000 == 0x5a5a8000
    # Explicit source/manual deltas: no legacy reserved enable, interlace,
    # duplicate VT, CEU or filled-data writes; eight-bit diagnostic instead of6.
    forbidden = {base+0x94, base+0xb0, base+0x100, base+0x304, base+0x308, base+0x30c}
    assert not forbidden.intersection(last)
    return {"vic": record["mode"], "manual_fields": len(expected),
            "exceptions": ["GCTL reserved enable preserved", "BASIC4 reserved VT preserved",
                           "removed interlace/CEU fields omitted", "manual eight-bit pattern",
                           "deferred TV_EN", "manual documented masks"]}
