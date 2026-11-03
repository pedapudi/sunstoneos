# Design: MBUS at boot0's 600 MHz in every image, and G2D at the vendor's 300 MHz

Status: **implemented and host-modeled; MBUS at 600 MHz after EL3 entry qualified on the board** (both boards, checked DRAM pattern preserved, [memory-dma status](../status/memory-dma.md)). The dedicated frozen qualification set and the G2D clock are not run on a board. Evidence: [mbus-restore.jsonl](../evidence/mbus-restore.jsonl).

Every image that starts after a return to the boot ROM would find MBUS at 400 MHz, against the 600 MHz that boot0 sets, the vendor keeps and the manual gives as the maximum. The performance policy ([AGENTS.md](../../AGENTS.md), "Implementation") makes the vendor's rate the default. This design covers the MBUS restore and the G2D clock, which change a clock only and no voltage. The video engine, the JPEG encoder and the SDIO Wi-Fi clock stay at their rates for the reasons under [Deferred changes](#deferred-changes).

MBUS clocks the memory-bus fabric between the DRAM controller and the bus masters that the manual lists in `MBUS_MAT_CLK_GATING_REG` (4.1.6.83): the three MSI_LITE bridges, STORE, SERDES, VID_IN, NPU, GPU, both video engines, the display engine and both IOMMUs. The CPU is not among them.

## The boot ROM sets 400 MHz on every return

- **boot0** (`829ea8e3`, routine at `0x57a4c`, part of the DRAM clock setup) gates MBUS off with the update bit, resets the NSI and the DRAM controller, reprograms PLL_DDR and the DRAM clock, then writes `0x89000000` into the cleared source field of `MBUS_CLK_REG` (`0x57b1e`–`0x57b28`): gate on, update, source 1 (PERI0_600M), divider 1. MBUS reads `0x81000000`, 600 MHz.
- **Boot ROM** (`a8e31821`, the capture `rom-copy-20261016-084055`). The forced-FEL entry at `0x20`, which every SunstoneOS image uses to return, sets r0 to 127 (`0x224`) and branches to the FEL dispatcher (`0x22c`). With 127 the dispatcher calls the secondary clock setup `0x2444` (`0x252c`–`0x2534`), which reaches the MBUS routine `0x2084` through `0x2160`. That routine:
  1. clears the gate and sets the update bit, then polls the update bit for up to 240,000 counter ticks, 10 ms (literal at `0x2158`);
  2. clears bit 17 of `MSI_LITE0_BGR_REG` (`0x02002594`), asserting MSI_LITE0's MBUS reset;
  3. waits 10 µs, clears the divider with the update bit, waits 10 µs and polls;
  4. sets source 4 (PERI0_400M) with the update bit, waits 10 µs and polls;
  5. sets bit 17 again;
  6. sets the gate with the update bit and polls.

  MBUS then reads `0x84000000`, 400 MHz. The SRAM survey before each returning image returns with `bx lr` to the FEL loop and does not rerun the clock setup.

## The restore repeats the ROM's sequence with boot0's source, from SRAM

`a733_el3_mbus_restore()` in [`include/a733_el3_mbus.h`](../../include/a733_el3_mbus.h) runs at EL3 in both EL3 monitors (`platform/a733/monitor.c`, `apps/storage-read-single-core/monitor.c`), before their security setup, and in both SMP boots (`platform/a733-smp/boot.c`, `platform/a733-smp-cached/boot.c`) and the secondary identity probe (`boot/a733/secondary-identity-probe.c`) before any secondary core is released. No driver has started an MBUS master at that point.

- **Admission.** It acts only when the masked word (gate, DFS enable, update, source and divider) is the ROM's `0x84000000` and MSI_LITE0's gate and both resets read released. At `0x81000000` it does nothing, and any other value is left alone for the drivers' fabric checks. The ROM's clock setup runs on every forced-FEL entry whether or not boot0 ran in the power cycle, so a lifetime without boot0 also starts at `0x84000000` and is restored. That is intended: 600 MHz is right whenever the bus starts at the ROM's 400 MHz, and PERI0_600M runs in both cases.
- **Bus masters.** Before any write it requires the masters that an earlier image could have left running to be idle. Otherwise it skips the restore with no write and returns the code below: MBUS stays at 400 MHz, the monitor logs the code, and the image continues, so an image that returned with the GPU still clocked does not stop every later image of the lifetime. The drivers' fabric checks then see 400 MHz and refuse sessions that name 600 MHz.
  - GPU (code 8): module clock gate (CCU `+0xb20` bit 31) and bus gate (`+0xb24` bit 0) off.
  - NPU (8): module clock gate (`+0xb00` bit 31) and bus gate (`+0xb04` bit 0) off.
  - DMA0 (9): with its bus gate and reset released (`+0x704` bits 0 and 16), `DMA_STA_REG` (`0x04601030`) zero, every channel idle and the MBUS FIFO empty (manual 13.1.7.3).
  - CE (10), which reaches DRAM through MSI_LITE0: with its bus gate and reset released (`+0xac4` bits 0 and 16), no task load bit (mask `0x8421`) in `CE_TLR_NS` or `CE_TLR_S` (`0x04603010`, `0x04603810`; manual 19.1.7.5).

  The ROM leaves CE clocked and out of reset, so CE is judged by its task registers. The GPU and NPU have no documented idle register that is cheap to read with their clocks off, so their clocks decide.
- **Window in SRAM.** Every image executes from DRAM with the caches off, and during the window MBUS is gated and MSI_LITE0 held in reset. The window is [`boot/a733/mbus-window.S`](../../boot/a733/mbus-window.S): position-independent, register-only and without a stack. The restore copies its 340 bytes (`a733_mbus_window_code[]`) to SRAM at `0x5f000`, invalidates the instruction cache and calls it there; the window touches only `MBUS_CLK_REG` and `MSI_LITE0_BGR_REG`. `0x5f000`–`0x5ffff` lies in SRAM A2 outside every launcher (`0x52000`–`0x55fff`), the FEL scratch (`0x71000`), the SRAM survey (`0x73000`), the ROM's executed-call frame (below `0x70000`) and its FEL stack in the shared SRAM; the forced-FEL return clears it. `checks/sram-window-reservation.py` enforces the reservation.
- **Sequence.** The ROM's six steps in the ROM's order, with source 1 in step 4. The divider is written before the source, as the manual's rule for raising a bus clock requires (4.1.4.3). Each update is polled for at most 10 ms; each 10 µs settle counts the 24 MHz system counter. MBUS must then read `0x81000000` and MSI_LITE0 its released state.
- **Failure.** A wait that times out returns its step (1, 3, 4 or 6); the window first releases MSI_LITE0's reset and sets the gate, still from SRAM, so the bus runs before it returns to DRAM. A wrong read-back returns 7. Only the codes 1 to 7 (`A733_MBUS_FAILED`) stop the image: the monitors finish with status 4, the SMP boots stop with status 2 and detail `0x200` plus the code, and the identity probe returns before it starts any core.
- **Record.** The returning monitor writes `MBUS <before> <after> <code>` as the first log line when the word changed or the restore skipped or failed; the code is one hex digit. A FEL read after the image returns cannot show the restored rate, because the return reruns the ROM's clock setup.

**DRAM and interconnect ordering.** boot0 changes MBUS with the NSI and the DRAM controller in reset, as part of DRAM initialization; SunstoneOS cannot use that sequence with DRAM live. The ROM changes MBUS with DRAM contents live on every forced-FEL return, executing from ROM with its stack in SRAM. That shows the DRAM contents survive the change; it does not show that the CPU may fetch from DRAM during it, which is why the window runs from SRAM. MBUS_DFS_EN stays clear, the divider-before-source rule holds, and the NSI clock, the DRAM clock and PLL_DDR are not touched.

## Sessions name 600 MHz

Every driver that checks the bus fabric compares the live MBUS word with its session's fabric record, and every driver's parent table admits source 1. The drivers stay read-only on MBUS. Sessions built from captures taken after a ROM return name `0x84000000` and 400,000,000 Hz, and an image with the restore refuses them.

[`tools/vendor-mbus.py`](../../tools/vendor-mbus.py) holds boot0's word and rate and rewrites a fabric record that names the ROM's word and rate; it returns any other record unchanged, so software-only fixtures keep the fabric they model. Every session producer in the repository passes its record through it: the seven application producers that extend a board session (`apps/{deinterlace,displayport,g2d,h264-encode,hevc,isp0,jpeg-encode}/producer.py`) and the three producers that build a session from a board configuration (`tools/check-a733-storage-launch.py`, `tools/check-a733-mimus-native.py`, `apps/usb-returning/prepare.py`). The coordinator's set tools that write fabric records live with the frozen sets, outside the repository, and need the same rule when the sets are rebuilt.

A frozen bundle keeps its own image, which runs at 400 MHz after a ROM return, so each still admits its session as frozen. Rebuilding its image from the current tree needs a session that names 600 MHz. The frozen sets that name `0x84000000` are listed in `mbus-restore-board-qualification`.

## G2D runs at 300 MHz

The vendor device tree assigns G2D 300 MHz (`sun60iw2p1.dtsi` lines 3383–3384), and the CCU's first G2D parent is PERI0_300M (`ccu-sun60iw2.c`, `g2d_parents`). `A733_G2D_MODULE` is source 0 undivided, 300 MHz, and the session check, the host producer and the preparer require `(0, 300000000)`. The native parent check requires PERI0_2X ÷ 4 = 300 MHz and the software and auto gates of PERI0_2X, PERI0_600M and PERI0_300M (manual 4.1.6.249 bits 27, 25, 21, 20, 11, 9, 5 and 4; status 4.1.6.258), as the microSD driver's PERI0_300M check does. The supply stays DCDC2 at 800 mV.

## Deferred changes

- **Video engine and JPEG at 624 MHz.** Both video PLLs are disabled in the clock dump. The decoder's cold start programs PLL_VE0 ([video PLL design](video-pll.md)); the encoder needs a cold start first.
- **SDIO Wi-Fi.** The vendor allows up to 208 MHz with SDR104 on SMHC1 (`sun60i-a733-orangepi-zero3w.dts` lines 1663 and 1670). Reaching it needs the module's UHS-I support from CCCR registers `0x13` and `0x14`, the CMD11 signalling handshake and CMD19 tuning. The radio platform implements none of them, and its four-bit 25 MHz profile has not run on a board ([radio platform](../radio-platform.md)). The next step is a CCCR read of those two registers in the existing enumeration, then a design.

## Budgets and checks

| Item | Budget | Result |
| --- | --- | --- |
| Restore, AArch64 `-O2` | 512 bytes of text and 512 of read-only data | 352 bytes of text; the window's 340 bytes of data |
| Time at EL3 from the ROM's value | under 100 µs | 30 µs in the window's modeled run, plus the 340-byte copy |
| Time at boot0's value | One register read | One read |

- `checks/el3-mbus.py OUTPUT` builds `checks/el3-mbus.c` with sanitizers and runs the admission, master-check and read-back cases; assembles `boot/a733/mbus-window.S`, requires it to be free of relocations and equal to the header's array, and runs it under Unicorn at `0x5f000` (including a stuck update bit at each of the four waits); and pins the sixteen ROM and boot0 words that the header cites.
- `checks/a733-monitor-are.py` runs the returning monitor's instructions: from the ROM's value it requires the six MBUS writes before the BL31 security writes and the log line `MBUS 0x84000000 0x81000000 0`; with the GPU clocked or DMA0 busy it requires the log line with code 8 or 9, no MBUS write and a normal start.
- `checks/a733-smp-cached.py` models boot0's value, so the restore makes no write there; `checks/secondary-local-probe.py` admits the restore's two reads.
- `checks/sram-window-reservation.py` fails when any linker script region, `ldr sp` stack, Python SRAM region table or hexadecimal literal outside the owning files overlaps `0x5f000`–`0x5ffff`. The DDR start (`tools/run-ddr.py`, `boot/a733/ddr-entry.S`) loads boot0 over the page before any image runs and is allowed with that reason.
- `checks/board-bundles.py`: the row numbers with `RESERVED_POSITIONS` (19 and 20) count from 1 without a gap.
- `checks/g2d-power-native.py` and `checks/g2d-modes-native.py` pass with a session that names G2D at 300 MHz and refuse the 100 MHz session.

## Open work

1. Run the frozen qualification set on ports 3-3 and 3-1 in one lifetime each: cold DDR start, write the pattern, two kernel checks, verify the pattern, and on port 3-3 the clock dump and one SMP OS image (the plan, predictions and bundle locations are in `mbus-restore-board-qualification`). The SMP OS image needs the native-image refreeze procedure rerun.
2. Measure the ShuffleNetV2 timing at 492 and 852 MHz with sessions naming 600 MHz for MBUS, and compare the G2D application's output digests with those at 100 MHz.
3. Move the video and SDIO clocks as described under Deferred changes.
