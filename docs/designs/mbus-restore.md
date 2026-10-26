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

## The restore repeats the ROM's sequence with boot0's source

`a733_el3_mbus_restore()` in
[`include/a733_el3_mbus.h`](../../include/a733_el3_mbus.h) runs at EL3,
before the security setup, in both EL3 monitors
(`platform/a733/monitor.c`, `apps/storage-read-single-core/monitor.c`) and
both SMP boots (`platform/a733-smp/boot.c`, `platform/a733-smp-cached/boot.c`).
No driver has started an MBUS master at that point.

- **Admission.** It acts only when the masked word (gate, DFS enable, update,
  source and divider) is the ROM's `0x84000000` and MSI_LITE0's gate and both
  resets read released. At `0x81000000` it does nothing. Any other value is
  left alone, so a lifetime without boot0 keeps its inherited state and the
  drivers' fabric checks decide.
- **Sequence.** The ROM's six steps in the ROM's order, with source 1 in step
  4. The divider is written before the source, as the manual's rule for
  raising a bus clock requires (4.1.4.3). Each update is polled for at most
  10 ms with a read that completes before the deadline; each 10 µs settle
  counts the 24 MHz system counter.
- **Read-back.** MBUS must then read `0x81000000` and MSI_LITE0 its released
  state.
- **Failure.** A wait that times out returns its step (1, 3, 4 or 6); a wrong
  read-back returns 7. Before returning it releases MSI_LITE0's reset and sets
  the gate, so the bus runs and the next ROM return starts from the ROM's
  usual state. The monitors then finish with status 4 and the SMP boots stop
  with status 2 and detail `0x200` plus the step.
- **Record.** The returning monitor writes `MBUS <before> <after> <result>`
  as the first log line when the word changed. A FEL read after the image
  returns cannot show the restored rate, because the return reruns the ROM's
  clock setup.

**DRAM and interconnect ordering.** boot0 changes MBUS with the NSI and the
DRAM controller in reset, as part of DRAM initialization; SunstoneOS cannot
use that sequence with DRAM live and does not need to. The ROM changes MBUS
with DRAM contents live on every forced-FEL return, and images loaded before
such returns keep running from the same DRAM afterwards, so the ROM's
sequence is the reference for a live change. MBUS_DFS_EN stays clear, the
divider-before-source rule holds, and the NSI clock, the DRAM clock and
PLL_DDR are not touched. The CPU keeps executing from DRAM during the gated
window because it reaches DRAM through the NSI, not MBUS (4.1.6.83).

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
| Restore code, AArch64 `-O2` | 512 bytes | 508 bytes (`checks/el3-mbus.py`) |
| Time at EL3 from the ROM's value | under 100 µs | Three 10 µs settles and four update polls; the model's ROM-state case spans 738 counter ticks, 31 µs |
| Time at boot0's value | One register read | One read |
| Accesses from the ROM's value | 6 writes, at most 4 polls of reads, 2 read-backs | As budgeted in every passing model case |

## Gate and image comparison

The integration gate built `01ec4800` with every central target and the ten
OS variants, and passed the kernel, eight-core soak, mimus SMP,
shared-helper and progress checks; its index check failed only on the new
check, which the next commit indexes. The parent `e3d5bf48` passed the same
gate. Evidence, including the per-image comparison, is in
`~/.cache/sunstoneos/evidence/clock-audit-mbus-20261025/`.

Of 64 images built on both revisions, 30 are byte-identical and 5 QEMU
models differ only in debug information. The other 29 gain `.text` only,
in one of two new functions:

| Images | `.text` change | Added |
| --- | --- | --- |
| Eleven SMP images and OS variants | +512 bytes (one OS variant +448) | `a733_el3_mbus_restore` |
| Four board application images | +508 bytes | `a733_el3_mbus_restore` |
| Five SMP diagnostic and device images | 0, inside section padding | `a733_el3_mbus_restore` |
| Six returning-monitor images (kernel check, memory qualification, mimus, probes) | +768 bytes | the restore inlined and `log_mbus` |
| Two USB returning images | +1,024 bytes | the same |
| Storage maintenance image | +2,048 bytes, section alignment | `a733_el3_mbus_restore` |

`.rodata` grows by 8 or 16 bytes in four returning images (the log text).
The G2D application, a separate project, grows by 500 bytes of `.text`, and
its `.rodata` shrinks by 24 bytes.

## Checks

- `checks/el3-mbus.py OUTPUT` builds `checks/el3-mbus.c` with address and
  undefined-behavior sanitizers and runs fifteen cases: the ROM's state with
  the update bit clearing on the first, tenth and thousandth read; seven
  states the restore leaves alone; a stuck update bit at each of the four
  waits, each bounded at 10 ms with the bridge released and the gate on; and
  a source write that does not take effect. It pins the sixteen ROM and boot0
  words that the header cites.
- `checks/a733-monitor-are.py` runs the returning monitor's instructions; its
  new case starts from the ROM's value and requires the six MBUS writes
  before the BL31 security writes and the log line
  `MBUS 0x84000000 0x81000000 0`. Its other 32 cases are unchanged.
- `checks/a733-smp-cached.py`, which the SMP image checks share, models
  boot0's value, so the restore makes no write there.
- `checks/board-bundles.py` passes with the row numbering that allows gaps,
  and `checks/audit-consolidate-apps-a-h-equivalence.py` passes.
- G2D: a session regenerated with the updated producer from the earlier
  model fixture, with the physical fabric record of a ROM return, names G2D at
  300 MHz and MBUS at `0x81000000`, 600 MHz
  (`~/.cache/sunstoneos/evidence/g2d-clock-audit-20261025/`).
  `checks/g2d-power-native.py` passes its four cases on the image built from
  this revision, writing `0x80000000` (PERI0_300M, divider 1) to the G2D
  clock; it now models the PERI0 gate words both boards read, because the
  pinned task model enables only the 400 MHz output's gates.
  `checks/g2d-modes-native.py` passes its 39 validator cases and 10 RCQ
  images with that session, and refuses the earlier 100 MHz session at its
  first case. The success, scale and blend checks replay states pinned to
  the 2026-10-20 image by hash and are not rerun.

## Board verification

The refrozen clock-audit dump (`board-bundles-20261025-clock-audit-2`, port
3-3) reads MBUS right after the cold DDR start and again after a returning
image's return. A returning image built from this revision then logs
`MBUS 0x84000000 0x81000000 0`. The ShuffleNetV2 timing at 492 and 852 MHz,
with sessions naming 600 MHz for MBUS, measures the effect on the NPU's
memory-bound time. The G2D application's output digests must equal those at
100 MHz.

## Review

An independent review follows this note. It should check the ROM and boot0
readings, the claim that the CPU is not an MBUS master, the failure path's
final state, the session change, and the G2D gate bits.
