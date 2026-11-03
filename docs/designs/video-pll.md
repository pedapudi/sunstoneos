# Design: the video decoder at the vendor's 624 MHz

Status: implemented and host-modeled; not run on a board.

The video engine's PLLs are disabled at FEL entry (record
`video-pll-fel-clock-dump` in [video-pll.jsonl](../evidence/video-pll.jsonl)),
and the [clock audit](../clock-audit.md) found that the decoder and encoder
bindings otherwise admit only a PLL already running at the session's rate. The
decoder's cold start therefore programs PLL_VE0 to the vendor's 624 MHz. The
decoder serves the H.264, HEVC and JPEG decode applications. The encoder, which
also serves JPEG encoding, keeps its inherited start (see
[Encoder](#encoder)).

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
  divider stays at 1 because the PLL reaches 624 MHz exactly.
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
  (4.1.4.1) orders the same steps with the factors written before the enable.
  The kernel enables the PLL at its reset factors and then retunes it
  (`ccu_nm_set_rate`); both orders end in the same control word, and method 2
  needs one lock instead of two, as the [NPU startup](../npu-pll.md) does.
- **Order of the module clock.** `enable_cedar_hw_clk_module` opens the AHB,
  MBUS and bus gates and then the module clock last (`cedar_ve.c`
  lines 450–472), with the parent that `clk_set_parent` chose at probe
  (line 1789).
- **Supply.** VDD-VE is on DCDC2, together with VDD-SYS, VDD-DRAM, VDD-NPU and
  the HDMI, UFS and COMBO 0.8 V nets. Board schematic v1.2 shows this twice:
  the PMIC sheet (sheet 6, "06_POWER AXP318") draws DCDC2's output with the
  net labels VDD-SYS, VDD-VE, VDD-COMB0, VDD08-HDMI, VDD-DRAM and VDD08-UFS,
  and DCDC5's with VDD-CPUL; the power-sequence table on sheet 2 lists VDD-VE
  under DCDC2 at 0.8 V. The vendor U-Boot device tree's `ve-supply` names DCDC5
  (`u-boot-current.dts` lines 1949 and 4197–4204), which contradicts both
  sheets; the board kernel's device tree has no `ve-supply`. One row of the
  sheet 2 table lists VDD-CPUL on DC-DC3, which contradicts sheet 6 (DCDC5 to
  VDD-CPUL); sheet 6, the PMIC wiring, governs. SunstoneOS takes DCDC2: the
  H.264, HEVC and JPEG decode admissions require the DCDC2 lease at 800 mV
  (`apps/jpeg/admission.c:15`, `apps/hevc/admission.c:20`,
  `apps/h264/admission.c:24`). DCDC2 also feeds DRAM, so no CPU-L operating
  point constrains the video engine. Performance-mode work must keep DCDC2 at
  800 mV or more while a video session runs; DCDC5 carries no such floor.

## Decoder cold start

A decoder configuration with a cold start from the boot ROM's FEL state, the
module word `0x81000000` and `module_hz` 624,000,000 programs PLL_VE0. Any
other configuration shares a running PLL and writes none of its registers. Only
the decoder programs a PLL, so the code names PLL_VE0 and nothing else.

- **Clock ownership.** A programmed PLL is an exclusive scope. The binding's
  scopes leave PLL_VE0 out of the shared set, and
  `a733_video_platform_exclusive` returns `VE_DEC | PLL_VE0`, which the H.264,
  HEVC and JPEG decode applications pass to the clock registry. Another
  PLL_VE0 consumer, such as an NPU that selects it, then cannot hold it.
- **Entry state.** The first register pass admits PLL_VE0 in one of three
  states:
  - disabled: enable, lock enable, LOCK and the output gate clear, the
    lock-level fields (bits 7:5) zero, and no other bit set but the LDO, the
    factors and reserved bit 24;
  - enabled at the vendor factors with its output closed, lock enable and
    LOCK either way, which a start that failed after its enable write leaves;
    lock enable is cleared and set and the lock qualified again, so a failure
    never needs a power cycle;
  - running the vendor profile with LOCK and its output open, which an earlier
    image in the same power lifetime leaves; it is adopted without a PLL write.

  Every other state refuses before any write. The gate controls and pattern
  words must read as on the board, and the module gate must be closed.
- **Position.** The table starts after the power domain is on, the reset is
  released and the bus gates are open, and ends before the module gate opens,
  matching the vendor's order for the module clock.
- **Start**, one poll under the register lock. Each write is a
  read-modify-write with readback, built on the previous readback; a field
  that already holds its value is not written. LOCK is written as zero and the
  readback ignores it; bit 24 keeps its read value.
  1. Module selector and divider: `0x01000000` into `+0xa88` bits 26:24 and
     4:0, gate closed.
  2. For a disabled PLL: LDO (bit 30), factors `0x00204d00` into N, P0 and M1,
     enable (bit 31), lock enable (bit 29). For a PLL left enabled: lock
     enable cleared (skipped when already clear), then set.
- **Wait**, the next poll, without the register lock. The exclusive PLL_VE0
  token makes the decoder the only writer of the word, as the PLL_NPU token
  does for the NPU startup. The lock wait is the one the CPU clock and the NPU
  startup use (`include/a733_pll_lock.h`): each read starts at least 3 µs after
  the previous read ended, three reads in a row must show LOCK, at most 100
  reads, then 20 µs after the third. Every read must equal the expected word
  apart from LOCK, so a changed field or another writer ends the wait. Times
  are whole microseconds, so each gap is 4 µs on the counter.
- **Open**, the next poll under the register lock: the word must read enabled
  and locked, the output gate opens, and the readback must be `0xf9204d00` with
  the board's bit 24. The module gate then opens, and every later clock check
  requires that exact PLL word.
- **Faults.** A refused step, a missing LOCK or a deadline fails the unit and
  retains every token. PLL_VE0 then stays enabled with its output closed and
  the module gate stays closed; the next start admits that state.
- **Native access.** The decoder's write allowlist is a pure rule,
  `a733_video_platform_write_policy`, which `native_write` applies with native
  reads. It accepts PLL_VE0 with the owned bits `0xf870ff02` only while none of
  PLL_VE0's consumers runs on it: the encoder's module clock at mux 0, the
  decoder's at mux 1 and the NPU's at mux 4 (`ccu-sun60iw2.c` lines 965–1015)
  must each have the gate closed or select another source. It accepts the
  decoder's selector and divider only while the decoder's module gate is
  closed. The SMP boots make the CCU's PLL registers non-secure before EL1 runs
  (`include/a733_el3_security.h`, write 14).
- **Sessions.** The HEVC, H.264 encoder and JPEG encoder session producers
  default to module `0x01000000` at 624,000,000 Hz. The decoder's mux 1 is
  PLL_VE0, and the encoder's is PLL_VE1, the vendor's parent for each.

### Shared lock wait

The CPU clock (`drivers/clock/a733_cpu_clock.c`), the NPU startup
(`drivers/npu/a733_npu_startup.c`) and the video unit count LOCK with
`a733_pll_lock_count`; the NPU startup and the video unit spin with
`a733_pll_lock_wait`. The CPU clock polls instead of spinning, so it bounds its
wait by its 70 ms lock timer and ignores locked reads that come sooner than the
gap after the last counted one. The NPU startup spaces each read from the
previous read. The helper is a header, so no build list references it.

## Encoder

The encoder binding starts only from an inherited state: power domain 3 on, its
reset released and its module gate open at the session's rate. On a board
nothing establishes that state, so the encoder cannot start at any rate until
it has a cold start like the decoder's. That start would program PLL_VE1, the
encoder's vendor parent, with the same lock wait; it needs PLL_VE1 (`+0x240`)
and the `+0xa80` selector and divider in the encoder's write allowlist.

## Qualified figures

| Figure | Value | Record |
| --- | --- | --- |
| PLL word after open | `0xf9204d00` (N=78, P0=3, 624 MHz) | [video-pll.jsonl](../evidence/video-pll.jsonl) `video-pll-host` (host model) |
| Cold start accesses, once per power lifetime | 5 writes (6 with the LDO off), at most 109 reads | same |
| Lock wait without the register lock | at most about 420 µs | same |
| FEL entry words | PLL_VE0 `0x41305a00`, PLL_VE1 `0x41205600`, both disabled | `video-pll-fel-clock-dump` |

## Checks

`checks/video-pll.py` compares the profile with the pinned manual, kernel clock
driver, device trees, operating-point table, `cedar_ve.c`, U-Boot and kernel
device trees and the schematic text, including the order of the vendor's
enable, lock wait, lock-enable toggle and module clock gates, and with the port
3-3 dump words. It builds and runs two models with address and
undefined-behavior sanitizers and seventeen driver mutants that the cold model
must refuse, checks the three session producers' defaults, and compiles the
H.264, HEVC and JPEG applications' `native.c` with their configured flags and
warnings as errors (`--app-compile-commands` names the configured builds). It
also compiles the changed drivers for AArch64. `checks/a733-video-cold.c` is
the cold-start model, `checks/a733-video-platform.c` the inherited-start model.

## Open work

1. PLL table alone: a FEL bundle on port 3-3 writes the table into PLL_VE0 with
   readback, reads LOCK until three reads in a row show it, opens the output,
   reads `0xf9204d00`, then writes back `0x41305a00` and reads it. The VE power
   domain stays off. This qualifies the factors, LOCK and the write order.
2. Decode at 624 MHz: the JPEG decode application's cold start on port 3-3,
   comparing the decoded picture with the host decoder and reading the PLL
   word after the run. It needs the physical evidence records that its session
   requires ([JPEG application](../jpeg-application.md)); none exist.
3. Encoder: a cold start for PLL_VE1, then the H.264 and JPEG encoder
   applications with frame timing.

The prediction for these runs is recorded as `video-pll-board-plan`.
