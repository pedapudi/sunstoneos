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
- **Rate.** `setup_ve_freq` (`cedar_ve.c` lines 351–366) disables the module
  clocks, calls `clk_set_rate(ve_clk, 624 MHz)` and enables them again. The
  module clock passes the rate to its parent (`CLK_SET_RATE_PARENT`), and the
  divider stays at 1 because the PLL can reach 624 MHz exactly.
  `ccu_nm_find_best` walks N upward from 53 and the output divider P0 from 1
  and keeps the first exact rate: N=78 and P0=3, a 1,872 MHz oscillator
  divided by 3. The manual gives 1.872 GHz as PLL_VE's typical oscillator
  (UM 4.1.3.2). The control word's factor fields are `0x00204d00`.
- **Enable.** The kernel's PLL enable helper (`ccu_gate.c` lines 157–211)
  sets the LDO, enable and lock enable, waits for LOCK, then opens the output
  gate. Its lock wait (`ccu_common.c` lines 47–64) reads the word, waits
  3 µs, and repeats until three reads in a row show LOCK, at most 100 reads,
  then waits 20 µs. A rate change clears and sets lock enable again before
  the same wait (`ccu_nm.c` lines 327–334). The manual's enable method 2
  (4.1.4.1) orders the same steps with the factors written before the
  enable.
- **Order of the module clock.** `enable_cedar_hw_clk_module` opens the AHB,
  MBUS and bus gates and then the module clock last (`cedar_ve.c`
  lines 450–472), with the parent that `clk_set_parent` chose at probe
  (line 1789).
- **Supply.** VDD-VE is on DCDC2, together with VDD-SYS, VDD-DRAM, VDD-NPU and
  the HDMI, UFS and COMBO 0.8 V nets. The board schematic v1.2 shows this
  twice: the PMIC sheet (sheet 6, "06_POWER AXP318") draws DCDC2's output
  with the net labels VDD-SYS, VDD-VE, VDD-COMB0, VDD08-HDMI, VDD-DRAM and
  VDD08-UFS, and DCDC5's with VDD-CPUL; the power-sequence table on sheet 2
  lists VDD-VE under DCDC2 at 0.8 V. The vendor U-Boot device tree's
  `ve-supply` names DCDC5 (`u-boot-current.dts` lines 1949 and 4197–4204),
  which contradicts both sheets; the board kernel's device tree has no
  `ve-supply`. The schematic is the wiring, so SunstoneOS takes DCDC2: the
  H.264, HEVC and JPEG decode admissions require the DCDC2 lease at 800 mV
  (`apps/jpeg/admission.c:15`, `apps/hevc/admission.c:20`,
  `apps/h264/admission.c:24`). DCDC2 also feeds DRAM, so no CPU-L operating
  point constrains the video engine. The performance-mode work must keep
  DCDC2 at 800 mV or more while a video session runs; DCDC5 carries no such
  floor.

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
writes none of its registers. Only the decoder programs a PLL, so the code
names PLL_VE0 and nothing else.

- **Clock ownership.** A programmed PLL is an exclusive scope. The binding's
  scopes leave PLL_VE0 out of the shared set, and
  `a733_video_platform_exclusive` returns `VE_DEC | PLL_VE0`, which the H.264,
  HEVC and JPEG decode applications pass to the clock registry. Another
  PLL_VE0 consumer, such as an NPU that selects it, then cannot hold it.
- **Entry state.** The cold start's first register pass admits PLL_VE0 in one
  of three states:
  - disabled: enable, lock enable, LOCK and the output gate clear, the
    lock-level fields (bits 7:5) zero, and no other bit set but the LDO, the
    factors and reserved bit 24;
  - enabled at the vendor factors with its output closed, LOCK either way,
    which a start that failed in its lock wait leaves; its lock is qualified
    again, so a failure never needs a power cycle;
  - running the vendor profile with LOCK and its output open, which an earlier
    image in the same power lifetime leaves; it is adopted without a PLL
    write.

  Every other state refuses before any write. The gate controls and pattern
  words must read as on the board, and the module gate must be closed.
- **Position.** The table starts after the power domain is on, the reset is
  released and the bus gates are open, and ends before the module gate opens,
  matching the vendor's order for the module clock.
- **Start**, one poll under the register lock. Each write is a
  read-modify-write with readback, built on the previous readback; a field
  that already holds its value is not written. LOCK is written as zero and
  the readback ignores it; bit 24 keeps its read value.
  1. Module selector and divider: `0x01000000` into `+0xa88` bits 26:24 and
     4:0, gate closed.
  2. For a disabled PLL: LDO (bit 30, already on in the dumped word), factors
     `0x00204d00` into N, P0 and M1, enable (bit 31), lock enable (bit 29).
     For a PLL left enabled: lock enable cleared, then set.
- **Wait**, the next poll, without the register lock. The exclusive PLL_VE0
  token makes the decoder the only writer of the word, as the PLL_NPU token
  does for the NPU startup. The lock wait is the one the CPU clock and the NPU
  startup use (`include/a733_pll_lock.h`): each read starts at least 3 µs
  after the previous read ended, three reads in a row must show LOCK, at most
  100 reads, then 20 µs after the third. Every read must equal the expected
  word apart from LOCK, so a changed field or another writer ends the wait.
  Times are whole microseconds, so each gap is 4 µs on the counter.
- **Open**, the next poll under the register lock: the word must read
  enabled and locked, the output gate opens, and the readback must be
  `0xf9204d00` with the board's bit 24. The module gate then opens as
  before. Every later clock check requires that exact PLL word.
- **Faults.** A refused step, a missing LOCK or a deadline fails the unit and
  retains every token. PLL_VE0 then stays enabled with its output closed,
  and the module gate stays closed; the next start admits that state.
- **Native access.** The decoder's write allowlist is a pure rule,
  `a733_video_platform_write_policy`, which `native_write` applies with
  native reads. It accepts PLL_VE0 with the owned bits `0xf870ff02` only
  while none of PLL_VE0's consumers runs on it. Those consumers are the
  encoder's module clock at mux 0, the decoder's at mux 1 and the NPU's at
  mux 4 (`ccu-sun60iw2.c` lines 965–1015); each must have its gate closed or
  select another source.
  It accepts the decoder's selector and divider only while the decoder's
  module gate is closed. The SMP boots make the CCU's PLL registers
  non-secure before EL1 runs (`include/a733_el3_security.h`, write 14).
- **Sessions.** The HEVC, H.264 encoder and JPEG encoder session producers
  default to module `0x01000000` at 624,000,000 Hz. The decoder's mux 1 is
  PLL_VE0, and the encoder's is PLL_VE1, the vendor's parent for each.

The board kernel instead enables the PLL at its reset factors and then
retunes it (`ccu_nm_set_rate`). Both orders end in the same control word;
the manual's method 2 needs one lock instead of two, and the
[NPU startup](../npu-pll.md) already uses it.

## The encoder needs a cold start first

The encoder binding starts only from an inherited state: power domain 3 on,
its reset released and its module gate open at the session's rate. On a board
nothing establishes that state, so the encoder cannot start at any rate until
it has a cold start like the decoder's. That start would program PLL_VE1,
the encoder's vendor parent, with the same lock wait; it needs PLL_VE1
(`+0x240`) and the `+0xa80` selector and divider in the encoder's write
allowlist.

## Budgets

- **Accesses.** From the dumped word the table makes five writes (six when
  the LDO is off) and at most 109 reads through the binding, once per power
  lifetime, before the first decode. The native write rule adds four reads
  per PLL write and two for the selector write.
- **Time.** The wait spins without the register lock for at most about
  420 µs: 100 reads 4 µs apart and the 21 µs settle. Start, wait and open
  take three polls of the cold start instead of one.
- **Rate.** The decoder runs at 624 MHz, the vendor's rate at the board's
  800 mV. No earlier rate has run on a board, so there is no before
  measurement.

## The lock wait is shared

The CPU clock (`drivers/clock/a733_cpu_clock.c`), the NPU startup
(`drivers/npu/a733_npu_startup.c`) and the video unit count LOCK with
`a733_pll_lock_count`; the NPU startup and the video unit spin with
`a733_pll_lock_wait`. The CPU clock polls instead of spinning, so it bounds
its wait by its 70 ms lock timer and ignores locked reads that come sooner
than the gap after the last counted one, as before. Its traces are
unchanged. The NPU startup now spaces each read from the previous read
instead of from its first, so a late read no longer lets the next reads come
back to back. Its host model's register reads and writes are unchanged, and
its waits are a few microseconds longer. The helper is a header, so no build
list changes.

## Checks

`checks/video-pll.py` compares the profile with the pinned manual, kernel
clock driver, device trees, operating-point table, `cedar_ve.c`, U-Boot and
kernel device trees and the schematic's text, including the order of the
vendor's enable, lock wait, lock-enable toggle and module clock gates, and
with the port 3-3 dump words. It then builds and runs two models with
address and undefined-behavior sanitizers and fourteen driver mutants that
the cold model must refuse. It checks the three session producers' defaults.
It compiles the H.264, HEVC and JPEG applications' `native.c` with their
configured flags and warnings as errors, and the changed drivers for
AArch64.

- `checks/a733-video-cold.c` keeps its seven earlier cases with unchanged
  output and adds fifteen. From the dumped FEL state it programs PLL_VE0 in
  the table's order, skipping the LDO that is already on. It also writes the
  LDO when it is off, keeps a clear bit 24 clear, runs on the 26 MHz board
  and restarts the count when LOCK drops once. It relocks a PLL left enabled
  with its output closed, with LOCK clear or set. It adopts a running vendor
  PLL without a PLL write. It refuses before any write a PLL running at other
  factors, a clear gate control, and entry words with enable, lock enable or
  a lock level set. With no LOCK, or with a changed word during the wait, it
  stops with the PLL enabled and the output and module gates closed. Every
  cold-start write passes the native write rule and stays in its owned bits;
  the rule is also checked against each PLL_VE0 consumer. The lock-wait reads
  must come without the register lock. The model clock advances 1 µs per
  eight reads of it, so only the binding's explicit waits can space the
  reads.
- The fourteen mutants: entry with enable or lock enable admitted, the lock
  levels admitted, bit 24 forced on, enable before factors, LOCK written
  back, no whole-word check per LOCK read, a LOCK drop ignored, reads 2 µs
  apart, a 17 µs settle, a 50-read limit, the encoder or NPU consumer
  ignored, and the lock wait under the register lock.
- `checks/a733-video-platform.c`, the inherited-start model, writes the same
  traces as before the change. `checks/cpu-clock-check.py` passes with the
  shared constants. The NPU startup host model passes in all five modes with
  the same register reads and writes.

The result is in
`~/.cache/sunstoneos/evidence/video-pll-20261025/check-r2/result.json`.

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
