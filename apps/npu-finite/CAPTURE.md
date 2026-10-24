# Physical prerequisites for a finite NPU run

This offline capture specification supplies the
register observations needed by the [returning host](host.py) and the
[startup sequencer](../../drivers/npu/a733_npu_startup.c). The coordinator must
bind observations and ownership evidence to one board, powered lifetime and
frozen image before admitting a board run. No register values below are asserted
to have been observed.

The numerical image uses 492 MHz at 800 mV, an operating point present in every
listed NPU speed bin. It needs no SID read. This numerical-only image does not initialize thermal sensing. Source identities and manual sections are in
[NPU platform](../../docs/npu-platform.md#sources-and-checks),
[NPU PLL](../../docs/npu-pll.md#source-basis) and
[pins](../../docs/pins.md#pinned-sources).

## Returning observation sequence

Use the already qualified FEL `readl` transport described in
[GPU entry](../gpu-finite/ENTRY.md), with the addresses below. Its original
capture script contains a different address list; do not execute it unchanged.
The transport preserves 128 bytes at `0x71000..0x7107f`, below the NPU-dependent
shared SRAM beginning at `0x74000`. It writes only this scratch range for the
register capture. It does not enable clocks or power domains.

1. The coordinator obtains the existing per-port ownership and checks the live
   ROM identity. Save and synchronize the 128-byte scratch backup.
2. Capture the RTC, CCU, R_CCU and R_PIO words below. Preserve full words, ordered
   addresses, transaction output and the current identity in the record.
3. Read domain 4 policy and status only if this capture observed all three
   domain-level gates open. If a gate is closed or unavailable, omit both PPU
   reads and record that domain coherence is unknown. Do not open a gate.
4. Restore scratch through the existing sealed-source path and verify all 128
   bytes in the same ROM instance. Stop and retain ownership on the first
   transport, identity, parse or restoration failure; do not retry accesses.
5. Before native launch, use the established cache-off helper and obtain a fresh
   launch survey. FEL `readl` can change the ROM instruction-cache state.

Keep the transport's 30-second transaction bound. This capture has 31
unconditional words and two conditional PPU words. Existing current-lifetime
secondary-OFF evidence can be reused through uninterrupted ownership; a new
secondary-power capture is unnecessary unless that continuity was lost.
No NPU engine (`0x03600000`), GPU engine, USB controller, GIC, TWI controller or
SID register belongs to this capture.

## Configuration words

For a mask and expected value, acceptance means `(word & mask) == expected`.
`CCU` is `0x02002000`. All addresses in the tables are absolute.

| Address | Field and predicate |
| --- | --- |
| `0x07090160` | DCXO: mask `0x10000002`, expected `0x10000002`; frequency code bits 15:14 maps to 24, 19.2, 26, 24 MHz |
| `0x02002000` | PLL_REF: mask `0xf8000000`, expected same; N=bits15:8+1 in 80..116; M=bits22:16+1; DCXO Hz × N = 24,000,000 × M |
| `0x02002500` | AHB fabric: record mask `0x0300001f` for typed boot configuration |
| `0x02002580` | NSI fabric: record mask `0x8700001f`; bit31 set; bits28:27 clear |
| `0x02002588` | MBUS fabric: record mask `0x8700001f`; bit31 set; bits28:27 clear |
| `0x020020a0` | PERI0: mask `0xfe000002`, expected `0xfe000000`; N=bits15:8+1 in 53..105 |
| `0x020020a8` | PERI0 pattern: bit31 clear |
| `0x020020ac` | PERI0 pattern: bit27 clear |
| `0x02003908` | Selected PERI0 branches: software-enable and No-Auto bits, decoded below |
| `0x02003988` | Selected PERI0 branches: effective gate bits, decoded below |
| `0x02002a80` | Video encoder: bit31 clear; selector bits26:24 at most 6 |
| `0x02002a88` | Video decoder: bit31 clear; selector bits26:24 at most 6 |
| `0x02002a8c` | Video bus gates/reset: mask `0x00050005`, expected zero |
| `0x02002b00` | NPU module: bit31 clear; selector bits26:24 at most 6 |
| `0x02002b04` | NPU bus/reset: mask `0x000f0001`, expected `0x00080000`; SRAM reset released, other resets asserted, bus gate closed |
| `0x02002c00` | DRAM0: mask `0x8f010000`, expected `0x80000000`; separate DDR PLL, gate open, no update pending |
| `0x02002c0c` | DRAM bus uses PHY PLL: bit16 set |
| `0x02003b10` | Clock-management mode: bit0 clear |
| `0x02003b14` | Clock-management mode: bit0 clear |
| `0x02003b1c` | Clock-management mode: bit0 clear |
| `0x020022a0` | PLL_NPU: mask `0xb8000000` must be zero; bits outside `0xf970ff02` must also be zero; capture inherited bit24 and require it unchanged throughout startup |
| `0x020022a4` | PLL_NPU pattern: bit0 clear |
| `0x020022a8` | PLL_NPU pattern: bit31 clear |
| `0x020022ac` | PLL_NPU bias: mask `0xff100000`, expected `0x30000000` |
| `0x020022b0` | PLL_NPU tuning: mask `0x001f0000`, expected `0x000b0000` |
| `0x020025c0` | NPU domain-level AHB gate: bit6 set |
| `0x020025e0` | NPU domain-level MBUS gate: bit18 set |
| `0x070101ac` | PPU bus clock: bit0 set |
| `0x07025000` | PL0/PL1 mux: mask `0xff`, expected `0x22` |
| `0x07025014` | PL0/PL1 drive codes: bits3:0 and 7:4, each 0..3 |
| `0x07025024` | PL0/PL1 pull codes: bits1:0 and 3:2, each 0..2 |

The fabric's source selector is bits26:24; the divider is bits4:0 + 1.
The AHB mask retains only its two selector bits. Supported parent rates in MHz
are AHB `{0:24, 3:600}`, NSI `{0:24, 2:800, 3:600, 4:480}` and MBUS
`{0:24, 1:600, 3:480, 4:400}`. Reject other selectors. Compute each integer
rate as parent Hz / divider, and bind the masked word and rate to the typed
boot bytes. The native sequencer checks them again before its first PLL write.

For every selected PERI0 parent, form the union `B` of the following masks.
Require `(CCU+0x1908) & (B | B<<16) == (B | B<<16)` and
`(CCU+0x1988) & (B<<16) == B<<16`.

| Parent MHz | Mask B | Divisor D for `24 × N == MHz × D` |
| --- | --- | --- |
| 600 | `0xa00` | `(bits22:20 + 1) × 2` |
| 400 | `0x806` | `(bits22:20 + 1) × 3` |
| 800 | `0x400` | `bits18:16 + 1` |
| 480 | `0x180` | `bits4:2 + 1` |

PERI0 predicates apply only when a fabric word selects a PERI0 parent. Capturing
these accessible CCU words unconditionally keeps the observation sequence fixed.
The decoder is [`peri0()`](../../drivers/npu/a733_npu_startup.c); reference
normalization is [`a733_clock_ref24_valid()`](../../include/a733_clock_reference.h).

Only after all three domain-level gates above were observed open, read
`0x07064000` (domain 4 policy) and `0x07064008` (status). Their low nibbles must
agree at OFF (0) or ON (8). These are PPU control-block reads; they do not access
the NPU engine. The [domain owner](../../drivers/power/a733_power_domains.c)
rechecks the same gates and coherence before native startup.

## Supply and pin authority

Raw pin settings do not authenticate ownership. The platform evidence must
grant exclusive PL0/PL1 authority, exclude competing S_TWI0 users and preserve
the provider's APBS0/APBS1 clock ownership. Bind actual function, pull and drive
codes into the PMIC configuration and authorize only those pins in its handoff.
PL_DATA is neither needed nor evidence of an output latch while pins use TWI.

Use the existing returning PMIC provider to observe DCDC2 in the same powered
lifetime. Its snapshot verifies the AXP device/page and repeated selector
readings. DCDC2 enable register `0x10` bit1 must be set; selector register `0x13`
masked by `0x7f` must equal `0x1e`, which decodes to 800,000 µV. Other enable
bits and selector bit7 are not required by the NPU supply predicate. Record raw
bytes and decoded voltage. This establishes the programmed supply configuration.
Analogue voltage and settling remain unmeasured. Stability also requires the provider's settle
interval and exclusion of supply writers through NPU admission.

The qualified returning [PMIC bundle](../board-bundles-power/bundle6.c) can
provide observations, but its frozen inputs need separate admission. It also
verifies DCDC4, expects both complete byte pairs to equal `0xff9e`, assumes both
pins have function/pull/drive `{2,1,1}`, and starts the existing provider and
return monitor. Those constants originated on another board. Their validity
cannot be inferred from ROM identity or a successful CPU workload. Do not
declare that bundle admitted until its own current inputs and returning image
have been reviewed; a differing byte pair is not permission to change voltage.

The required artifact chain is: frozen returning snapshot image and matching
source/object closure; its current platform, pin and clock admission; normal
return and PMIC snapshot record; uninterrupted supply/pin ownership; typed NPU
boot bytes; and the finite image's power evidence digest. The finite image
retains its own native supply verification. Neither earlier-board snapshots nor
an offline plan can satisfy `dcdc2-stable-800mv` or
`authenticated-pmic-pin-inheritance` in [host admission](host.py).
