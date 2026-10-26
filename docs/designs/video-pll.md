# Design: the video decoder at the vendor's 624 MHz

Status: **implemented and host-modeled; not run on a board.** The
[clock audit](../clock-audit.md) found that SunstoneOS never programs the
video engine's PLLs: the decoder and encoder bindings admit only a PLL that is
already running at the session's rate. The port 3-3 clock dump shows that
nothing starts them, so no video session could start on a board at any rate.
This note makes the decoder's cold start program PLL_VE0 to the vendor's
624 MHz. The decoder serves the H.264, HEVC and JPEG decode applications. The
encoder, which also serves JPEG encoding, keeps its inherited start; the
[encoder section](#the-encoder-needs-a-cold-start-first) gives the reason.

## Reference: the vendor runs both units at 624 MHz from their own PLL

- **Operating point.** The VE table of `sun60iw2p1-cpu-vf.dtsi`
  (lines 2142–2189) lists 624 MHz at 800 mV for every speed bin, vf0400 at
  lines 2169–2172 included. `ve_plat_sun60iw2.c` takes the bin's row as the
  default rate and falls back to `VE_DVFS_MAX_FREQ`, 624 (line 78).
- **Parents.** The device tree (`sun60iw2p1.dtsi` lines 847–898) gives the
  decoder `CLK_PLL_VE0` with `CLK_VE_DEC`, and the encoder `CLK_PLL_VE1` with
  `CLK_VE_ENC0`. `cedar_ve.c` sets that parent at probe
  (`clk_set_parent(ve_clk, pll_ve)`). In both module clocks' parent lists
  (`ccu-sun60iw2.c` lines 965–981) that PLL is mux value 1, so the vendor's
  module word is `0x01000000` before the gate: mux 1, divider 1.
- **Rate.** `setup_ve_freq` (`cedar_ve.c` lines 350–366) disables the module
  clocks, calls `clk_set_rate(ve_clk, 624 MHz)` and enables them again. The
  module clock passes the rate to its parent (`CLK_SET_RATE_PARENT`), and the
  divider stays at 1 because the PLL can reach 624 MHz exactly.
  `ccu_nm_find_best` walks N upward from 53 and the output divider P0 from 1
  and keeps the first exact rate: N=78 and P0=3, a 1,872 MHz oscillator
  divided by 3. The manual gives 1.872 GHz as PLL_VE's typical oscillator
  (UM 4.1.3.2). The control word's factor fields are `0x00204d00`.
- **Enable.** The kernel's PLL enable helper (`ccu_gate.c` lines 157–211)
  sets the LDO, enable and lock enable, waits for LOCK, then opens the output
  gate. Its lock wait (`ccu_common.c`) needs three consecutive LOCK reads
  3 µs apart, at most 100 reads, then 20 µs. The manual's enable method 2
  (4.1.4.1) orders the same steps with the factors written before the
  enable.

## The board leaves both PLLs disabled

The clock dump of port 3-3 at FEL entry
(`~/.cache/sunstoneos/evidence/board-session-20261025-24/port-3-3/clock-audit-b/`)
reads:

| Register | Word | Meaning |
| --- | --- | --- |
| PLL_VE0, CCU `+0x220` | `0x41305a00` | Disabled; LDO on; N=91, P0=4, which would give 546 MHz; reserved bit 24 set |
| PLL_VE1, `+0x240` | `0x41205600` | Disabled; N=87, P0=3, which would give 696 MHz; bit 24 set |
| PLL_VE0 pattern words, `+0x228` and `+0x22c` | `0`, `0x30000000` | Spread spectrum off |
| PLL gate controls `+0x1918`, status `+0x1998` | `0x00030003`, `0x00030000` | Both VE PLL software gates enabled, No-Auto set, effective status on |
| VE_DEC and VE_ENC0 module words, `+0xa88` and `+0xa80` | `0`, `0` | Gates closed, mux 0, divider 1 |

The manual gives `0x4030_5A00` as PLL_VE0's reset value; the board adds
reserved bit 24, as it does for PLL_NPU ([NPU PLL](../npu-pll.md)).

## The decoder's cold start runs the enable table

A decoder configuration with a cold start from the boot ROM's FEL state, the
module word `0x81000000` and `module_hz` 624,000,000 programs PLL_VE0. Any
other configuration keeps the earlier behavior: it shares a running PLL and
writes none of its registers.

- **Clock ownership.** A programmed PLL is an exclusive scope. The binding's
  scopes leave PLL_VE0 out of the shared set, and
  `a733_video_platform_exclusive` returns `VE_DEC | PLL_VE0`, which the H.264,
  HEVC and JPEG decode applications pass to the clock registry. Another
  PLL_VE0 consumer, such as an NPU that selects it, then cannot hold it.
- **Entry state.** The cold start's first register pass admits PLL_VE0
  disabled: enable, lock enable, LOCK and the output gate clear, the lock-level
  fields (bits 7:5) zero, and no other bit set but the LDO, the factors and
  reserved bit 24. It also admits PLL_VE0 already
  running the vendor profile with LOCK and its output open, which an earlier
  image in the same power lifetime leaves; that PLL is adopted without a
  write. Every other enabled state refuses before any write. The gate
  controls and pattern words must read as on the board, and the module gate
  must be closed.
- **Position.** The table runs after the power domain is on, the reset is
  released and the bus gates are open, and before the module gate opens. This
  is the state in which `setup_ve_freq` sets the rate.
- **Table.** The table runs in one call under the register lock, as the
  kernel's runs under its spinlock; a poll per step would space the LOCK
  reads a scheduler tick (1 ms) apart. Each write is a read-modify-write with
  readback, built on the previous readback. LOCK is written as zero and the
  readback ignores it; bit 24 keeps its read value.
  1. Module selector and divider: `0x01000000` into `+0xa88` bits 26:24 and
     4:0, gate closed.
  2. LDO, bit 30.
  3. Factors `0x00204d00` into N, P0 and M1.
  4. Enable, bit 31.
  5. Lock enable, bit 29.
  6. LOCK: reads at least 3 µs apart until three in a row show LOCK, at most
     100 reads; then 20 µs.
  7. Output gate, bit 27. The readback must be `0xf9204d00` with the board's
     bit 24.

  The module gate then opens as before. Every later clock check requires
  that exact PLL word.
- **Faults.** A refused step or a missing LOCK fails the unit and retains
  every token. PLL_VE0 then stays enabled with its output closed, and the
  module gate stays closed.
- **Native access.** The decoder's write allowlist adds PLL_VE0 with the
  owned bits `0xf870ff02` and the module selector and divider of `+0xa88`.
  Both are accepted only while the decoder's module gate reads closed, so no
  write retunes a clock the decoder runs on.
  The SMP boots make the CCU's PLL registers non-secure before EL1 runs
  (`include/a733_el3_security.h`, write 14).
- **Sessions.** The HEVC session producer defaults to module `0x01000000`
  at 624,000,000 Hz.

The board kernel instead enables the PLL at its reset factors and then
retunes it (`ccu_nm_set_rate`). Both orders end in the same control word;
the manual's method 2 needs one lock instead of two, and the
[NPU startup](../npu-pll.md) already uses it.

## The encoder needs a cold start first

The encoder binding starts only from an inherited state: power domain 3 on,
its reset released and its module gate open at the session's rate. On a board
nothing establishes that state, so the encoder cannot start at any rate until
it has a cold start like the decoder's. The PLL table lives in the shared
video unit (`a733_video_unit_pll_prepare`) and selects PLL_VE1 for the
encoder's mux 1. An encoder cold start can call it, after adding PLL_VE1
(`+0x240`) and the `+0xa80` selector and divider to the encoder's write
allowlist. Its session producers then default to the vendor point.

## Budgets

- **Time.** The table adds six writes and at most 106 reads once per power
  lifetime, before the first decode; the native write's checks add two reads
  per write, 118 reads in all. The LOCK wait spins under the register lock
  for at most about 320 µs: 100 reads 3 µs apart and the 20 µs settle.
- **Rate.** The decoder runs at 624 MHz, the vendor's rate at the board's
  800 mV. No earlier rate has run on a board, so there is no before
  measurement.

## Checks

`checks/video-pll.py` compares the profile with the pinned manual, kernel
clock driver, device trees, operating-point table and `cedar_ve.c`, and with
the port 3-3 dump words. It then builds and runs two models with address and
undefined-behavior sanitizers, and compiles the changed drivers for AArch64
with warnings as errors.

- `checks/a733-video-cold.c` keeps its seven earlier cases with unchanged
  output and adds seven. From the dumped FEL state it programs PLL_VE0 in the
  table's order, with LOCK reads at least 3 µs apart and the output opening
  only 20 µs after three LOCK reads in a row. It does the same on the
  26 MHz board, and restarts the count when LOCK drops once. It adopts a
  running vendor PLL without a PLL write. It refuses a PLL running at other
  factors and a clear gate control before any write. With no LOCK it stops
  after 100 reads with the PLL enabled and the output and module gates
  closed. The registry refuses a second owner's shared PLL_VE0. In these
  cases the model clock advances 1 µs per eight reads of it, so only the
  binding's explicit waits can space the reads; mutants with 2 µs spacing,
  an 18 µs settle or a count that ignores a LOCK drop fail.
- `checks/a733-video-platform.c`, the inherited-start model, writes the same
  traces as before the change.

The result is in
`~/.cache/sunstoneos/evidence/video-pll-20261025/check/result.json`.

## Board plan

1. **PLL table alone.** A FEL bundle on port 3-3 writes the table into
   PLL_VE0 with readback, reads LOCK until three reads in a row show it, opens
   the output and reads `0xf9204d00`. It then writes back the dumped
   `0x41305a00` and reads it. The VE power domain stays off, so no consumer
   sees the clock. This qualifies the factors, LOCK and the write order.
2. **Decode at 624 MHz.** The JPEG decode application's cold start on port
   3-3, with the decoded picture compared against the host decoder and the
   PLL word read after the run. It needs the physical evidence records that
   its session requires ([JPEG application](../jpeg-application.md)); none
   exist yet, so this run cannot be prepared today.
3. **Encoder.** After an encoder cold start exists, the H.264 and JPEG
   encoder applications with frame timing.
