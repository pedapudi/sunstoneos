#!/usr/bin/env python3
"""Compare the composed runtime trace with unchanged primary source bytes.

Run once after the host fixture. This does not execute an additional controller
model. The TV0 checker is imported into the same source-comparison pass.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re


def block(text, name):
    match = re.search(r"\b" + re.escape(name) + r"\s*\[[^;]*?=\s*\{(.*?)\n\};", text, re.S)
    assert match, name
    return match.group(1)


def fields(header, offset):
    match = re.search(r"union REG_" + f"{offset:04X}" + r"_t\s*\{.*?struct\s*\{(.*?)\}\s*sun60i;", header, re.S)
    assert match, hex(offset)
    result, shift = {}, 0
    for name, bits in re.findall(r"u32\s+(\w+)\s*:\s*(\d+)", match.group(1)):
        width = int(bits)
        result[name] = (((1 << width)-1) << shift, shift)
        shift += width
    assert shift == 32
    return result


def verify(record, root, dts):
    low = root / "bsp/drivers/drm/sunxi_device/hardware/lowlevel_hdmi20"
    pll = (low / "phy_top.c").read_text()
    header = (low / "phy_top.h").read_text()
    rate = 74250 if record["mode"] == 4 else 148500
    reference = record.get("phy_reference_hz", 24000000)
    assert reference in (24000000, 26000000)
    row = re.search(r"\{\s*" + str(rate) + r"\s*,([^}]+)\}", block(pll, "sun60i_phypll_"+str(reference//1000000)+"m"))
    assert row
    values = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", row.group(1))]
    assert len(values) == 4
    factors = values[0]
    calculated = reference * (((factors >> 8) & 255)+1) // ((((factors >> 16) & 127)+1) * (((factors >> 1) & 1)+1))
    expected_rate = rate*1000 if reference == 24000000 else (73937500 if rate == 74250 else 147875000)
    assert calculated == expected_rate
    events = record["writes"]
    # Initial TOP values are observed from preflight writes, before the PLL
    # factor sequence. This comparison makes no claim about their provenance.
    split = next(i for i, e in enumerate(events) if e[0] == 0x03000164)
    current = {a-0x05600000: v for a, width, v, time in events[:split]
               if width == 4 and 0x05600000 <= a < 0x05600040}
    expected = []
    cfg = pll[pll.index("cfg_pll:"):]
    cfg = cfg[:cfg.index('hdmi_inf("[top phy]')]
    assignments = re.findall(r"phy_reg->reg_([0-9a-fA-F]+)(?:\.sun60i\.(\w+)|\.dwval)\s*=\s*([^;]+);", cfg, re.S)
    assert len(assignments) == 12
    for offset, name, rhs in assignments:
        off = int(offset, 16)
        if name:
            mask, shift = fields(header, off)[name]
            if "pll_cfg.pll_value" in rhs:
                value = factors & mask
            else:
                assert rhs.strip() == "0x1", rhs
                value = 1 << shift
            current[off] = (current.get(off, 0) & ~mask) | value
        else:
            which = {"pll_cfg.ldo_value": 1, "pll_cfg.pll_patern0": 2,
                     "pll_cfg.pll_patern1": 3}[rhs.strip()]
            current[off] = values[which]
        expected.append((0x05600000+off, 4, current[off]))
    current[0x20] |= fields(header, 0x20)["pll_output_gate"][0]
    expected.append((0x05600020, 4, current[0x20]))
    actual = [(a, width, v) for a, width, v, time in events[split+1:]
              if width == 4 and 0x05600020 <= a <= 0x05600030]
    assert actual == expected, (actual, expected)

    snps = (low / "phy_snps.c").read_text()
    row = re.search(r"\{\s*" + str(rate) + r",\s*\{\s*\{([^}]+)\}", block(snps, "sun60i_mpll_rep0"))
    assert row
    mpll = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", row.group(1))]
    drive = re.search(r"snps_phy\s*=\s*<\s*25000\s+185625\s+(0x\w+)\s+(0x\w+)\s+(0x\w+)", dts)
    assert drive
    expected_internal = list(zip((6, 0x10, 0x11, 0x19, 0x0e, 9), mpll + [int(x, 16) for x in drive.groups()]))
    image, internal, internal_reads = {}, [], []
    for a, width, value, time in events:
        if width != 1:
            continue
        image[a] = value
        if a == 0x05523026:
            if value == 0x10:
                internal.append((image[0x05523021], image[0x05523022] << 8 | image[0x05523023]))
            elif value == 1:
                internal_reads.append(image[0x05523021])
    assert internal == expected_internal
    assert internal_reads == [x[0] for x in expected_internal]

    native = (root / "drivers/gpu/drm/bridge/synopsys/dw-hdmi.c").read_text()
    coeff = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", block(native, "csc_coeff_rgb_full_to_rgb_limited"))]
    assert len(coeff) == 12 and "u32 csc_scale = 1;" in native
    ordered = []
    for column in range(4):
        for row in range(3):
            value = coeff[row*4+column]
            address = 0x05524102+row*8+column*2
            ordered += [(address+1, value & 255), (address, value >> 8)]
    actual_coeff = [(a, v) for a, width, v, time in events if width == 1 and 0x05524102 <= a <= 0x05524119]
    assert actual_coeff == ordered
    assert image[0x05524101] & 0xf3 == 0x41
    assert image[0x05524004] & 1 == (record["range"] == 0)
    assert image[0x0552101b] & 12 == (0 if record["range"] == 0 else 8)
    assert coeff[0] * 0 // 8192 + coeff[3] // 2 == 16
    assert coeff[0] * 255 // 8192 + coeff[3] // 2 == 235
    return {"vic": record["mode"], "nominal_pixel_khz": rate, "calculated_pixel_hz": calculated,
            "top_pll_source_writes": len(expected), "verified_phy_words": len(internal),
            "csc_byte_writes": len(actual_coeff), "nominal_csc_endpoints": [16, 235]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sources", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--manual", type=Path, required=True)
    parser.add_argument("--board-dts", type=Path, required=True)
    parser.add_argument("--trace", type=Path, required=True)
    args = parser.parse_args()
    rows = json.loads(args.manifest.read_text())
    for item in rows:
        path = args.sources.parents[1] / item["archive"]
        assert hashlib.sha256(path.read_bytes()).hexdigest() == item["sha256"], path
    spec = importlib.util.spec_from_file_location("tv_reference", Path(__file__).with_name("a733-tcon-tv-reference.py"))
    tv = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tv)
    traces = [json.loads(line) for line in args.trace.read_text().splitlines()]
    assert {(r["mode"], r["range"]) for r in traces} == {(4, 0), (16, 1)}
    result = [{"hdmi": verify(r, args.sources, args.board_dts.read_text()),
               "tv": tv.verify_tv(r, args.manual)} for r in traces]
    print(json.dumps({"result": "pass", "pinned_files": len(rows), "profiles": result}, indent=2))


if __name__ == "__main__":
    main()
