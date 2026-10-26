# Design: MBUS at boot0's 600 MHz in every image, and G2D at the vendor's 300 MHz

Status: **implemented and host-modeled; not run on a board.** The
[clock audit](../clock-audit.md) found MBUS at 400 MHz in every image that
starts after a return to the boot ROM, against the 600 MHz that boot0 sets,
the vendor keeps and the manual gives as the maximum. The performance policy
([AGENTS.md](../../AGENTS.md), "Implementation") makes the vendor's rate the
default. This note covers the MBUS restore and the G2D clock, which change a
clock only and no voltage. The video engine, the JPEG encoder and the SDIO
Wi-Fi clock stay at their present rates for the reasons under
[Deferred](#deferred-changes).

MBUS clocks the memory-bus fabric between the DRAM controller and the bus
masters that the manual lists in `MBUS_MAT_CLK_GATING_REG` (4.1.6.83): the
three MSI_LITE bridges, STORE, SERDES, VID_IN, NPU, GPU, both video engines,
the display engine and both IOMMUs. The CPU is not among them.

## The boot ROM sets 400 MHz on every return

- **boot0** (`829ea8e3`, routine at `0x57a4c`, part of the DRAM clock setup)
  gates MBUS off with the update bit, resets the NSI and the DRAM controller,
  reprograms PLL_DDR and the DRAM clock, then writes `0x89000000` into the
  cleared source field of `MBUS_CLK_REG` (`0x57b1e`–`0x57b28`): gate on,
  update, source 1 (PERI0_600M), divider 1. MBUS reads `0x81000000`, 600 MHz.
- **Boot ROM** (`a8e31821`, the physical capture of
  `rom-copy-20261016-084055`). The forced-FEL entry at `0x20`, which every
  SunstoneOS image uses to return, sets r0 to 127 (`0x224`) and branches to
  the FEL dispatcher (`0x22c`). With 127 the dispatcher calls the secondary
  clock setup `0x2444` (`0x252c`–`0x2534`), which reaches the MBUS routine
  `0x2084` through `0x2160`. That routine:
  1. clears the gate and sets the update bit, then polls the update bit for
     up to 240,000 counter ticks, 10 ms (literal at `0x2158`);
  2. clears bit 17 of `MSI_LITE0_BGR_REG` (`0x02002594`), asserting
     MSI_LITE0's MBUS reset;
  3. waits 10 µs, clears the divider with the update bit, waits 10 µs and polls;
  4. sets source 4 (PERI0_400M) with the update bit, waits 10 µs and polls;
  5. sets bit 17 again;
  6. sets the gate with the update bit and polls.

  MBUS then reads `0x84000000`, 400 MHz.

The board reads agree. Port 3-3 read `0x81000000` right after each of four
cold DDR starts and `0x84000000` after every image returned to the ROM in
the same lifetimes; port 3-1 read `0x84000000` in a lifetime without boot0.
The SRAM survey before each returning image returns with `bx lr` to the FEL
loop and does not rerun the clock setup.

## The restore repeats the ROM's sequence with boot0's source, from SRAM

`a733_el3_mbus_restore()` in
[`include/a733_el3_mbus.h`](../../include/a733_el3_mbus.h) runs at EL3 in
both EL3 monitors (`platform/a733/monitor.c`,
`apps/storage-read-single-core/monitor.c`), before their security setup, and
in both SMP boots (`platform/a733-smp/boot.c`,
`platform/a733-smp-cached/boot.c`) and the secondary identity probe
(`boot/a733/secondary-identity-probe.c`) before any secondary core is
released. No driver has started an MBUS master at that point.

- **Admission.** It acts only when the masked word (gate, DFS enable, update,
  source and divider) is the ROM's `0x84000000` and MSI_LITE0's gate and both
  resets read released. At `0x81000000` it does nothing, and any other value
  is left alone for the drivers' fabric checks. The ROM's clock setup runs on
  every forced-FEL entry whether or not boot0 ran in the power cycle, so a
  lifetime without boot0, such as port 3-1's read without a DDR start, also
  starts at `0x84000000` and is restored. That is intended: under the
  performance policy 600 MHz is right whenever the bus starts at the ROM's
  400 MHz, and PERI0_600M runs in both cases.
- **Bus masters.** Before any write it requires the masters that an earlier
  image could have left running to be idle. Otherwise it skips the restore
  with no write and returns the code below: MBUS stays at the ROM's 400 MHz,
  the monitor logs the code, and the image continues, so an image that
  returned with the GPU still clocked does not stop every later image of the
  lifetime. The drivers' fabric checks then see 400 MHz and refuse sessions
  that name 600 MHz.
  - GPU (8): module clock gate (CCU `+0xb20` bit 31) and bus gate (`+0xb24`
    bit 0) off, so neither the GPU nor its firmware processor runs;
  - NPU (8): module clock gate (`+0xb00` bit 31) and bus gate (`+0xb04` bit
    0) off;
  - DMA0 (9): with its bus gate and reset released (`+0x704` bits 0 and 16),
    `DMA_STA_REG` (`0x04601030`) zero, every channel idle and the MBUS FIFO
    empty (manual 13.1.7.3);
  - CE (10), which reaches DRAM through MSI_LITE0: with its bus gate and
    reset released (`+0xac4` bits 0 and 16), no task load bit (mask
    `0x8421`, the bits the CE driver requires clear) in `CE_TLR_NS` or
    `CE_TLR_S` (`0x04603010`, `0x04603810`; manual 19.1.7.5).

  The ROM leaves CE clocked and out of reset, so CE is judged by its task
  registers. The GPU and NPU have no documented idle register that is cheap
  to read with their clocks off, so their clocks decide. Board reads after
  NPU images show `+0xb00` = 0 and `+0xb04` = `0x00080000`, so an NPU image
  that stopped normally passes.
- **Window in SRAM.** Every image executes from DRAM with the caches off, and
  during the window MBUS is gated and MSI_LITE0 held in reset. The window is
  therefore [`boot/a733/mbus-window.S`](../../boot/a733/mbus-window.S):
  position-independent, register-only and without a stack. The restore copies
  its 340 bytes (`a733_mbus_window_code[]`) to SRAM at `0x5f000`, invalidates
  the instruction cache and calls it there; the window touches only
  `MBUS_CLK_REG` and `MSI_LITE0_BGR_REG`. `0x5f000`–`0x5ffff` lies in SRAM A2
  outside every launcher (`0x52000`–`0x55fff`), the FEL scratch (`0x71000`),
  the SRAM survey (`0x73000`), the ROM's executed-call frame (below
  `0x70000`) and its FEL stack in the shared SRAM; the forced-FEL return
  clears it. `checks/sram-window-reservation.py` enforces the reservation.
- **Sequence.** The ROM's six steps in the ROM's order, with source 1 in step
  4. The divider is written before the source, as the manual's rule for
  raising a bus clock requires (4.1.4.3). Each update is polled for at most
  10 ms with a read that completes before the deadline; each 10 µs settle
  counts the 24 MHz system counter.
- **Read-back.** MBUS must then read `0x81000000` and MSI_LITE0 its released
  state.
- **Failure.** A wait that times out returns its step (1, 3, 4 or 6); the
  window first releases MSI_LITE0's reset and sets the gate, still from SRAM,
  so the bus runs before it returns to DRAM. A wrong read-back returns 7.
  Only these codes, 1 to 7 (`A733_MBUS_FAILED`), stop the image: the
  monitors finish with status 4, the SMP boots stop with status 2 and detail
  `0x200` plus the code, and the identity probe returns before it starts any
  core.
- **Record.** The returning monitor writes `MBUS <before> <after> <code>` as
  the first log line when the word changed or the restore skipped or failed;
  the code is one hex digit. A FEL read after the image returns cannot show
  the restored rate, because the return reruns the ROM's clock setup.

**DRAM and interconnect ordering.** boot0 changes MBUS with the NSI and the
DRAM controller in reset, as part of DRAM initialization; SunstoneOS cannot
use that sequence with DRAM live. The ROM changes MBUS with DRAM contents live
on every forced-FEL return, executing from ROM with its stack in SRAM, and
images loaded before such returns keep running from the same DRAM afterwards.
That shows the DRAM contents survive the change; it does not show that the
CPU may fetch from DRAM during it, which is why the window runs from SRAM.
MBUS_DFS_EN stays clear, the divider-before-source rule holds, and the NSI
clock, the DRAM clock and PLL_DDR are not touched.

## Sessions name 600 MHz

Every driver that checks the bus fabric compares the live MBUS word with its
session's fabric record, and every driver's parent table already admits
source 1. The drivers stay read-only on MBUS. Sessions built from captures
taken after a ROM return name `0x84000000` and 400,000,000 Hz, and an image
from this revision refuses them.

[`tools/vendor-mbus.py`](../../tools/vendor-mbus.py) holds boot0's word and
rate and rewrites a fabric record that names the ROM's word and rate; it
returns any other record unchanged, as the restore leaves any other register
value alone, so software-only fixtures keep the fabric they model. Every
session producer in the repository passes its record through it:

- the seven application producers that extend a board session
  (`apps/{deinterlace,displayport,g2d,h264-encode,hevc,isp0,jpeg-encode}/producer.py`);
- the three producers that build a session from a board configuration
  (`tools/check-a733-storage-launch.py`, `tools/check-a733-mimus-native.py`,
  `apps/usb-returning/prepare.py`).

The coordinator's set tools that write fabric records (for example
`tools/prepare-review.py` in the NPU sets) live with the frozen sets, outside
the repository, and need the same rule when the sets are rebuilt.

**Frozen bundles that name 400 MHz.** A frozen bundle keeps its own image,
which predates the restore and runs at 400 MHz after a ROM return, so each
still admits its session as frozen. Rebuilding its image from this revision
needs a session that names 600 MHz. These sets name MBUS `0x84000000`:

| Set in `~/.cache/sunstoneos/evidence/` | Bundles |
| --- | --- |
| `board-bundles-20261023`; `board-bundles-20261024/board-bundles-20261024-{4,5,6}`; `board-bundles-20261024-13` | The native USB images `rank-9-usb` through `rank-9-usb-v7` (`fixture-config.json`) |
| `board-bundles-20261025-7` | NPU numerical run with the shared SRAM routed to the NPU |
| `board-bundles-20261025-8` | Three NPU starts in one lifetime |
| `board-bundles-20261025-11`, `-13` | Two interrupt-completed NPU starts |
| `board-bundles-20261025-16`, `-19` | Two ShuffleNetV2 starts |
| `board-bundles-20261025-22` | Returning microSD reads (`config.json`) |

The physical configurations `usb-returning-20261018/physical/config.json` and
`usb-first-fault-20261018/physical-06{3,4}/config.json` name it too.
`board-bundles-20261025-14` already names `0x81000000`.

## G2D runs at 300 MHz

The vendor device tree assigns G2D 300 MHz (`sun60iw2p1.dtsi` lines
3383–3384), and the CCU's first G2D parent is PERI0_300M (`ccu-sun60iw2.c`,
`g2d_parents`). `A733_G2D_MODULE` is now source 0 undivided, 300 MHz, and the
session check, the host producer and the preparer require
`(0, 300000000)` in place of `(0x01000003, 100000000)`. The native parent
check requires PERI0_2X ÷ 4 = 300 MHz and the software and auto gates of
PERI0_2X, PERI0_600M and PERI0_300M (manual 4.1.6.249 bits 27, 25, 21, 20, 11,
9, 5 and 4; status 4.1.6.258), as the microSD driver's PERI0_300M check does.
The supply stays DCDC2 at 800 mV.

## Deferred changes

- **Video engine and JPEG at 624 MHz.** Their bindings derive the rate from
  the inherited PLL_VE0 or PLL_VE1, which no board has read. The clock-audit
  dump reads them; the change follows that result.
- **SDIO Wi-Fi.** The vendor allows up to 208 MHz with SDR104 on SMHC1
  (`sun60i-a733-orangepi-zero3w.dts` lines 1663 and 1670). Reaching it needs
  the module's UHS-I support from CCCR registers `0x13` and `0x14`, the CMD11
  signalling handshake and CMD19 tuning. The radio platform implements none
  of them, and its four-bit 25 MHz profile has not run on a board
  ([radio platform](../radio-platform.md)). The next step is a CCCR read of
  those two registers in the existing enumeration, then a design.

## Budgets and measurements

| Item | Budget | Result |
| --- | --- | --- |
| Restore, AArch64 `-O2` | 512 bytes of text and 512 of read-only data | 352 bytes of text; the window's 340 bytes of data (`checks/el3-mbus.py`) |
| Time at EL3 from the ROM's value | under 100 µs | Three 10 µs settles and four update polls; the window's ROM-state run under Unicorn spans 731 counter ticks, 30 µs, plus the 340-byte copy |
| Time at boot0's value | One register read | One read |
| Accesses from the ROM's value | 2 admission reads, at most 9 master reads, 6 window writes, the update polls, 2 read-backs | As budgeted in every passing case |

## Gate and image comparison

The integration gate built `68c496e5`, and again `3f09ec2c`, with every central target and the ten
OS variants, and passed the kernel, eight-core soak, mimus SMP,
shared-helper and progress checks; the index, PMIC-tree and power-domain-tree
checks pass at the documentation commit that follows. The parent `e3d5bf48`
passed the same gate. Evidence is in
`~/.cache/sunstoneos/evidence/clock-audit-mbus-20261025/`.

Of 64 images built on both revisions, 30 are byte-identical and 5 QEMU
models differ only in debug information. The other 29 add only
`a733_el3_mbus_restore`, `a733_mbus_window_code` and, in the returning
monitor, `log_mbus`:

| Images | `.text` | `.rodata` |
| --- | --- | --- |
| Ten OS variants and the supervised SMP image | +320 to +512 bytes | +336 to +352 bytes |
| Four board application images | +352 bytes | +336 to +344 bytes |
| Five SMP diagnostic and device images | 0, inside section padding | +336 bytes |
| Eight returning-monitor images (kernel check, memory qualification, mimus, USB, probes) | +768 bytes | +336 to +352 bytes |
| Storage maintenance image | +2,048 bytes, section alignment | +344 bytes |

Two images built as separate projects compare the same way: the read-only
storage single-core image (`apps/storage-read-single-core`) grows by 256
bytes of text and 320 of read-only data, and the G2D application by 344 and
328.

## Checks

- `checks/el3-mbus.py OUTPUT`:
  - builds `checks/el3-mbus.c` with address and undefined-behavior sanitizers
    and runs 22 cases of the admission, the master checks and the read-back:
    the ROM's state with DMA0 in reset and out of reset; seven states the
    restore leaves alone; a clocked or bus-gated GPU and NPU, a busy DMA0
    channel or FIFO, and a CE task in either world, each refused with no
    window; DMA0 and CE in reset, which pass; a window failure; and an
    ignored source write; and the rule that only codes 1 to 7 stop the image;
  - assembles `boot/a733/mbus-window.S`, requires it to be free of
    relocations and its bytes to equal the header's array, and runs those
    bytes under Unicorn at `0x5f000` with only that page, the two registers
    and an unmapped stack pointer: the ROM's write order with the update bit
    clearing on the first, tenth and thousandth read, the 10 µs settles, and a
    stuck update bit at each of the four waits, bounded at 240,000 ticks with
    the bridge released and the gate on;
  - pins the sixteen ROM and boot0 words that the header cites.
- `checks/a733-monitor-are.py` runs the returning monitor's instructions,
  with the window executed from the mapped SRAM page: from the ROM's value it
  requires the six MBUS writes before the BL31 security writes and the log
  line `MBUS 0x84000000 0x81000000 0`; with the GPU clocked or DMA0 busy it
  requires the log line `MBUS 0x84000000 0x84000000` with code 8 or 9, no
  MBUS write, the security writes and a normal start. Its other 32 cases are
  unchanged.
- `checks/a733-smp-cached.py`, which the SMP image checks share, models
  boot0's value, so the restore makes no write there;
  `checks/secondary-local-probe.py` admits the restore's two reads.
- `checks/sram-window-reservation.py` reads the window's address and size
  from the header and fails when any linker script's region or location,
  any `ldr sp` stack, any SRAM region table of a Python tool or any
  hexadecimal literal outside the owning files overlaps the page
  `0x5f000`–`0x5ffff`. The DDR start (`tools/run-ddr.py`,
  `boot/a733/ddr-entry.S`) loads boot0 over the page before any image runs
  and is listed as allowed with that reason. It passes over 45 SRAM layouts
  and fails on an injected launcher region, region table entry or stack.
- `checks/board-bundles.py` passes; the row numbers with
  `RESERVED_POSITIONS` (19 and 20) must count from 1 without a gap.
- G2D: a session regenerated with the updated producer from the earlier model
  fixture, with the fabric record of a ROM return, names G2D at 300 MHz and
  MBUS at 600 MHz (`~/.cache/sunstoneos/evidence/g2d-clock-audit-20261025-2/`).
  On the image built from `68c496e5`, `checks/g2d-power-native.py` passes its
  four cases, writing `0x80000000` (PERI0_300M, divider 1) to the G2D clock;
  it models the PERI0 gate words both boards read, because the pinned task
  model enables only the 400 MHz output's gates. `checks/g2d-modes-native.py`
  passes its 39 validator cases and 10 RCQ images with that session and
  refuses the earlier 100 MHz session at its first case. The success, scale
  and blend checks replay states pinned to the 2026-10-20 image by hash and
  are not rerun.

## Board qualification is frozen and has not run

`~/.cache/sunstoneos/evidence/board-bundles-20261025-mbus-qual-2/` holds, for
ports 3-3 and 3-1, the kernel check of bundle 2 rebuilt from `3f09ec2c`
(`kernel-check-counter-option`, `image.elf` `1bfe345e…`, which links the
restore and its log line) and the clock-audit dump (frozen as bundle 21; bundle 27 since the merge with bringup), prepared by
`tools/prepare-board-bundles.py`, with its runbook. Each port runs, in one
lifetime with power-removal recovery staged:

1. cold DDR start;
2. `tools/mbus-dram-pattern.py write`: two 4 MiB regions at `0x60000000` and
   `0xc0000000`, each 64-bit word its own address XOR a constant, through
   FEL;
3. the kernel check once, which finds boot0's 600 MHz and logs no MBUS line;
   its return to the ROM sets 400 MHz;
4. the kernel check again, whose log must start with
   `MBUS 0x84000000 0x81000000 0` and pass its 100 rounds;
5. `tools/mbus-dram-pattern.py verify`: both regions read back unchanged
   after two gated windows and two ROM returns;
6. port 3-3 only: the dump, read B of `board-bundles-20261025-clock-audit-2`,
   which must read MBUS `0x84000000` after the return; then one SMP OS
   image.

The memory qualification image (`apps/memtest`) writes and checks its
patterns within one run and its row is not admitted by the bundle tool, so
the pattern that spans the restore is written and read through FEL. The SMP
OS image is not frozen in the set: its returning bundle needs the
native-image refreeze procedure
(`~/.cache/sunstoneos/evidence/refreeze-el3-20261025/README.txt`) rerun for
the native image built at `3f09ec2c`, whose binding script is fixed to the
earlier image and source archive; the set's runbook lists the steps.

Further measurements follow the qualification: the ShuffleNetV2 timing at
492 and 852 MHz, with sessions naming 600 MHz for MBUS, measures the effect on
the NPU's memory-bound time, and the G2D application's output digests must
equal those at 100 MHz.

## Review

The first independent review found the window running from DRAM, the claim
about lifetimes without boot0, the missing bus-master checks, the G2D mask's
PERI0_480M bits, the missing storage single-core comparison and identity-probe
restore, and the bundle gap rule; this revision addresses each. A further
review follows.
