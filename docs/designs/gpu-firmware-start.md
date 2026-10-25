# Starting the GPU firmware from the open-source host driver

Status: **step 1 passed on port 3-1; step 2 implemented, host-checked and
frozen for port 3-1, not yet run on a board.**

The GPU powers on and off on the board in the vendor's state
([the default GPU power path](gpu-vendor-default.md)). This note derives what
the vendor's host driver does between "CORE6 status 8, identity readable"
and "firmware started", compares it with SunstoneOS's start
([docs/gpu-start.md](../gpu-start.md)), and gives the two board steps that
start the firmware: the probe-read with the secure configuration, then the
firmware start itself.

**Sources.** Every step comes from the open-source host driver and its
interface headers: the Imagination DDK as shipped in orangepi
`linux-orangepi` `2ac08e8c`, with the vendor platform glue `rgx_sunxi/`, its
build configuration `binary_sunxi_linux_nulldrmws_release/config_kernel.h`
and the register-access macros of `services/server/include/osfunc.h`. All
are cached in `~/.cache/sunstoneos/sources/gpu-fw-start-ddk-2ac08e8c/`, and
`manifest.json` there gives each file's URL and SHA-256; paths below are
relative to `rogue_km/`. The firmware binary is never disassembled or
decompiled (`LICENSE.powervr` line 14); only the container fields that the
host driver itself parses are used.

**The vendor driver cannot load our firmware.** The vendor driver is DDK
24.2 (`include/pvrversion.h:47–48`) and requests its own unpublished
`rgx.fw.36.56.104.183` (`services/server/devices/rogue/rgxinit.c:1892–1944`).
Its container check requires the firmware version to equal 24.2
(`services/server/devices/rgxfwimageutils.c:828–844`). SunstoneOS runs the
open firmware `rogue_36.56.104.183_v1.fw` (version 1.1, container header
version 3, open-source flag set;
[gpu-firmware-container.json](../vendor-firmware/data/gpu-firmware-container.json)),
whose structure layouts are upstream Linux's. The vendor's register sequence
and platform steps carry over; its structure offsets do not.

## The vendor's sequence

### Firmware image and memory

- The container is the ELF followed by a 4,096-byte `RGX_FW_INFO_HEADER` and
  its layout table (`services/include/rgx_fw_info.h`); the host finds the
  header at size − 4,096 and the table at header + header length
  (`rgxfwimageutils.c:880–983`).
- Only `PT_LOAD` program headers are placed: each goes to the layout entry
  that contains its virtual address, at allocation + (address − base) +
  allocation offset, with the tail up to `memsz` zeroed
  (`rgxfwimageutils.c:102–169, 679–736, 1089–1131`). A MIPS core has no
  coremem sections.
- The boot data (`RGXMIPSFW_BOOT_DATA`, `include/rgx_mips.h:265–274`) gives
  the firmware its stack, register bank and page-table addresses
  (`rgxfwimageutils.c:1108–1130`, `services/server/devices/rogue/rgxsrvinit.c:746–812`).
- The firmware heap is a contiguous GPU-virtual region whose low 32 bits are
  the MIPS address (`services/server/devices/rogue/rgxfwutils.c:692`). The
  MIPS page table has one 4-byte entry per 4 KiB page
  (`include/rgx_mips.h:68, 92–132`; `services/server/devices/rogue/rgxmipsmmuinit.c:216, 675–680`).
  The fixed remaps put boot code at `0xBFC00000`, boot data at `0xBFC01000`
  and exceptions at `0x9FC02000` (`include/rgx_mips.h:211–233`).
- For a MIPS core the start writes no GPU MMU (BIF) register
  (`services/server/devices/rogue/rgxstartstop.c:643–657`).

### Shared structures and the core clock

- The host fills the OS-init and system-init structures and the connection
  control in the firmware's configuration heap (`RGXSetupFwOsData`,
  `rgxfwutils.c:2524–2705`; `RGXSetupFwSysData`, `rgxfwutils.c:1750–2496`).
  System init clears `bFirmwareStarted` and sets the marker to 1
  (`rgxfwutils.c:2322–2323`).
- The core clock reaches the firmware in three places: the system-init
  initial clock (`rgxfwutils.c:2334`), the runtime configuration clock
  (`rgxfwutils.c:2341`) and every timer-correlation record. The vendor
  takes it from `clk_get_rate(clk_core)` (`rgx_sunxi/sysconfig.c:313`) after
  `sunxi_platform_init` has set the highest operating point
  (`rgx_sunxi/sunxi_platform.c:754–760, 922`). The header says the clock is
  used to compute timer ticks (`include/rogue/rgx_fwif_km.h:1340`).

### Register access width

Every 64-bit register access of the vendor start goes through
`RGXWriteReg64` and `RGXReadReg64` (`services/server/devices/rgxlayer_impl_common.c:210–285`),
which use `OSWriteUncheckedHWReg64` and `OSReadUncheckedHWReg64`. In the
Linux kernel build both are two 32-bit accesses
(`services/server/include/osfunc.h:1086–1107`): a write stores the low word
and then the high word; a read combines `readl(+4) << 32` with `readl(+0)`,
whose order C leaves to the compiler and whose source names the high word
first. No vendor path makes a 64-bit load or store to the GPU window.

### The start, in the order the power framework runs it

`RGXPostPowerState` (`services/server/devices/rgxpower.c:1010`) runs after
`sunxiPrePowerState` has opened the clocks and TOP5
(`rgx_sunxi/sunxi_platform.c:342–351`):

1. **Timer correlation** (`rgxpower.c:1030`; `RGXTimeCorrBegin`,
   `services/server/devices/rgxtimecorr.c:662–679` → `_RGXMakeTimeCorrData`,
   lines 271–343). The host reads `RGX_CR_TIMER` (`0x160`, 64-bit), reads it
   again if bit 63 differs from bit 31, and keeps bits 47:0
   (`rgxfwutils.h:347–363`). It reads its own time in ns. It writes a 40-byte
   record {OS ns, 0, CR ticks, K, clock Hz, 0} into slot (sequence + 1) mod
   256 of the GPU utilisation buffer, then a barrier, then the new sequence
   count. K = (256·10⁶ ≪ 20) / clock kHz
   (`services/server/devices/rgxtimecorr.h:77–82, 105–119`): 266,305,015 at
   1,008 MHz.
2. **Secure configuration** (`RGXDoStart`, `rgxpower.c:865–889`). This build
   does not define `SUPPORT_TRUSTED_DEVICE`, so it calls
   `pfnTDSystemSecureConfig` = `sunxi_secure_config`
   (`rgx_sunxi/sysconfig.c:371`): `writel(2, 0x0A0000B0)`, where the source
   names SMC0 offset `0xb0` `SMC_DRM_GPU_HW_RST`, then `writel(0,
   0x01880014)`, the wrapper's `GPU_DRM_CTRL`
   (`rgx_sunxi/sunxi_platform.c:958–979`). It reads neither back, makes both
   writes on every power-on before the first GPU register write, and never
   writes either word anywhere else: the power-off path
   (`sunxiPostPowerState`) does not restore them.
3. **`RGXStart`** (`rgxstartstop.c`), offsets from
   `hwdefs/rogue/km/rgx_cr_defs_km.h`:
   1. `SYS_BUS_SECURE` (`0xA100`) ← 0, read back (lines 855–860);
   2. `SOFT_RESET` (`0x100`, 64-bit) ← full mask, then Rascal and dusts
      released, then only GARTEN held, each read back (`RGXResetSequence`,
      lines 775–816). The file never writes `SOFT_RESET2` (`0x108`);
   3. `SLC_CTRL_MISC` (`0x3800`) read and written with the hash and burst
      bypass (lines 562–590);
   4. the eleven MIPS wrapper writes: register base, three boot remaps and
      the BRN 63553 remap, the address-limit and debug words
      (lines 287–456);
   5. `AXI_ACE_LITE_CONFIGURATION` (`0x38C0`) (lines 710–772);
   6. 3 µs, `SOFT_RESET` ← 0, read back, 3 µs (`DeassertMetaReset`, lines
      819–829).
4. **Wait** (`rgxpower.c:1039–1052`): a barrier, then a poll of the 32-bit
   `bFirmwareStarted` word for 1, with a cache invalidate before each read,
   every 50 µs for `MAX_HW_TIME_US` = 500,000 µs
   (`rgx_sunxi/sysinfo.h:48, 51`; `services/server/common/pvrsrv.c:3240–3241`).
   The host never clears the flag before a later power-on.

The vendor powers the GPU off only after the firmware has acknowledged its
forced-idle and power-off requests (`RGXPrePowerState`, `rgxpower.c:358`;
the power-off request at line 220), and only then stops it with `RGXStop`
(line 188).

## SMC0 +0xb0 is written 2 and left there

Step 1 on port 3-1 read SMC0 `0x0a0000b0` as 0 at entry and again as 0
immediately after writing 2. The word therefore either clears itself or
ignores the write. A self-clearing word fits the vendor's name for it,
`SMC_DRM_GPU_HW_RST`, a GPU reset control, and fits a driver that writes it
on every power-on and never restores it. The A733 user manual places SMC0 at
`0x0A000000`–`0x0A00FFFF` in the memory-controller block and describes none
of its registers. The vendor's register map names bit 0 of `drm_ctrl`
`drm_bypass` (`rgx_sunxi/sunxi_dvfs_ctrl.c:76–84`); the board's entry value
1 is that bit, and the write of 0 clears it.

Step 2 therefore follows the vendor exactly: it writes 2 and then
`drm_ctrl` 0 before the first GPU register write, and writes neither back.
It reads each word back only to record the value; neither read-back is a
condition of the start.

Step 1 wrote both words back to their entry values, SMC0 last, and then
powered the GPU off. If writing 0 to SMC0 `+0xb0` holds the GPU in a reset,
the GPU sat in that reset from the restore until the OFF. That path stays
controlled: the OFF writes only the CCU, the CORE6 power policy unit and
the TOP5 row, never a GPU-window register, and on the board the OFF, the
supply end and the provider park all completed after the restore.

## What SunstoneOS changes for the vendor start

SunstoneOS's start ([gpu-start.md](../gpu-start.md),
[pvr_gpu_start.c](../../drivers/gpu/pvr_gpu_start.c)) was derived from
upstream Linux. It writes `SYS_BUS_SECURE`, the three `SOFT_RESET` values,
`SLC_CTRL_MISC`, the eleven wrapper writes in their order and the AXI value,
waits 3 µs on each side of the release, and sets up no BIF, as the vendor
does. Its container parsing, section placement and boot data match the
DDK's and are stricter. Six items differed; the A733 start now takes the
vendor's choice in each:

| Item | Vendor DDK | A733 start |
| --- | --- | --- |
| Secure configuration | SMC0 `+0xb0` ← 2, `drm_ctrl` ← 0 before `RGXStart`, every power-on, never restored | The same, written by the firmware-start image before the controller's first step |
| Core clock to the firmware | `clk_get_rate`, the configured operating point | `A733_GPU_CORE_HZ` (1,008 MHz) in system init and runtime configuration |
| Timer-correlation record | `RGX_CR_TIMER` read and one record written before the start | The same record in slot 1, sequence 1 |
| Started wait | 32-bit word equal to 1, 0.5 s | The 32-bit word (`a733_gpu_memory_started`), at most 1 s |
| `SOFT_RESET2` (`0x108`) | Never written | Never written: removed from `pvr_gpu_start.c`, 29 steps for the pinned firmware |
| 64-bit register access | Two 32-bit accesses, low word written first, high word read first | The same in the firmware-start image's callbacks |

Dropping `SOFT_RESET2` removes the six accesses that upstream makes at the
three reset stages; a stall at a register the vendor never touches could
only be recovered by a power removal. The reference comparison now checks
that the upstream function makes exactly those six and that every other
access matches ([gpu-start.md](../gpu-start.md)).

The firmware heap size, the structure layouts and the MIPS page-table
geometry stay upstream's: the open firmware is built against them, and the
DDK's 32 MiB heap and its system-init layout (which begins with a MIPS
state block, `include/rogue/rgx_fwif_km.h:1484`) belong to the vendor's
firmware. The runtime fields the DDK also sets (active power latency 10 ms,
HCS deadline, firmware watchdog 2 s, `rgxfwutils.c:2333–2357`) stay zero;
the host source does not say whether the open firmware needs them.

## Board steps

Each step is one bounded, returning image on port 3-1, after a power
removal, the cold DDR start and the frozen provider-readiness checkpoint.

### Step 1: the probe-read with the secure configuration (passed)

The probe-read diagnostic (bundle 17, `apps/gpu-power-id/probe-read.c`) on
the default path powers the GPU on to CORE6 status 8, required within
200 ms. It reads `drm_ctrl`, `irq_ctrl` and SMC0 `+0xb0`, then the identity.
It makes the two secure-configuration writes in the vendor's order, each
read back, reads the identity again, and writes both words back to their
entry values. It then powers the GPU off, ends its supply sample and parks
the provider.

It ran on port 3-1 in `board-session-20261025-14` from set
`board-bundles-20261025-18` (`s15-gpq-run`) and completed:

- CORE6 reached status 8 10,341 µs after the ON began;
- `drm_ctrl` read 1 at entry, 0 after the write of 0, and 1 after the restore;
- `irq_ctrl` read 0;
- SMC0 `+0xb0` read 0 at entry and 0 after the write of 2;
- both identities read `36.56.104.183`, `GPU PROBE window serror_pending=0`;
- the OFF, the supply end and the provider park completed, with
  `COMPLETE GPU probe read: identity 36.56.104.183 before and after the
  secure configuration, restored, OFF and provider parked`.

The run, from monitor entry to exit, took 19.9 ms.

### Step 2: the firmware start

The firmware-start image (`apps/gpu-power-id/fw-start.c`, build and check
option `--fw-start`, bundle row 19) starts the pinned open firmware with
the vendor's prerequisites. It is frozen for port 3-1 in set
`board-bundles-20261025-20`, after the provider-readiness checkpoint, in a
lifetime of its own (`board-session-20261025-16`):

1. Before any power change it prepares the firmware. Code (102,400 bytes),
   data (20,480 bytes) and the 16 KiB MIPS page table lie in write-back
   image RAM; the nineteen shared resources take the first 45 pages of the
   image's Normal-NC block at `0x41c00000`, which the image zeroes. System
   init and the runtime configuration carry the 1,008 MHz core clock. The
   queues are formatted and the 29-step start plan is built. A refusal ends
   the run before the ON.
2. It powers the GPU on to CORE6 status 8 within 200 ms, as step 1 does,
   checks the wrapper, SMC0 and three GPU register pages as identity
   Device-nGnRnE mappings, and reads the identity, which must be
   `36.56.104.183`, then `drm_ctrl` and SMC0 `+0xb0`.
3. It starts only while the 1 s start bound and a 100 ms report fit before
   the 1.5 s session deadline, which also keeps the start's end 500 ms
   before the monitor's 2 s recovery. Otherwise it returns status 7 with
   nothing written.
4. It reads `RGX_CR_TIMER` and writes one timer-correlation record into
   slot 1, then sequence 1.
5. It writes SMC0 `+0xb0` = 2 and `drm_ctrl` = 0, records both read-backs
   and never writes either back.
6. It runs the 29 steps of `pvr_gpu_start.c`: publication (cache clean of
   the write-back firmware memory), a check that the started word is 0,
   then the vendor's register sequence, every 64-bit register as two
   32-bit accesses, and a wait of at most 1 s for the 32-bit started word.
   During the wait it yields one scheduler tick after each 50 µs of reads.
7. It reports the controller's result, the started word, the marker and
   start timestamp, system data's flags and power state, the connection
   states, the executed command count, and read-backs of `SOFT_RESET` and
   the eleven MIPS wrapper registers.

The image leaves the GPU powered with the firmware running, and the
lifetime ends there with a power removal. The vendor stops the firmware and
powers the GPU off only after the firmware's own forced-idle and power-off
handshake, which needs the command queue and interrupt service that this
image does not carry; a reset or OFF under running firmware is outside the
vendor's flow. The firmware's MIPS page table maps only the image's own
firmware and shared pages, so whatever it writes after the image returns
stays inside them.

Every GPU, wrapper and SMC0 access is one 32-bit access, made after the
progress record at result offset 128 (magic `GPUFWS01`) names it and after
a `GPU FWS reading` or `writing` log line.

Predictions: the started word becomes 1 within 0.5 s, the vendor's bound;
the run prints `COMPLETE GPU firmware start: started word 1 after the timer
record and the secure configuration, GPU left running for the power
removal`.

Controlled failure states:

- a start whose started word is not 1 by the deadline (error 2), is already
  set before the start (error 4) or holds a value other than 0 or 1
  (error 5) reports the shared state and the registers and fails with
  `GPU firmware start: started word not 1, GPU left running`; the firmware
  processor stays released;
- an identity other than `36.56.104.183` fails before any write, with the
  GPU powered;
- a refused preparation, mapping or time budget fails before any GPU,
  wrapper or SMC0 access;
- a start that would end too near the deadlines returns status 7 with
  nothing written.

A register access that never completes stops the CPU; the monitor does not
return, and only a power removal recovers the board. The progress record
then names the access in flight, but it survives only on a return.

## Design gate

**Cost.** The start runs once per GPU power-on. The timer record adds two
32-bit reads (four after a low-word wrap) and one 40-byte write; the secure
configuration adds two writes and two observation reads; splitting the
fourteen 64-bit register writes and four 64-bit reads of the start into
32-bit halves adds 18 accesses to its 24. The log of a complete run in the host check is 8,047
bytes of the 16,128-byte log area.

**Checks.**

- `checks/pvr-gpu-start.c` and `checks/pvr-gpu-start-reference.py` cover
  the 29-step controller: 36 retained faults, 40 refusals, and the
  reference traces less `SOFT_RESET2`
  (`~/.cache/sunstoneos/evidence/gpu-start-20261025/`).
- `apps/gpu-power-id/check.py --fw-start` runs eleven cases on the linked
  image, with only the ON, the clock, translations, cache maintenance and
  the log modeled; the preparation, the controller and the shared-memory
  adapter run as linked. Every access must be the next of the case's
  expected sequence, with its progress record and log line. The cases are:
  - the complete start, with the CR timer read once and, after a low-word
    wrap, twice;
  - a start that times out, finds the started word already set, or reads 2;
  - a CORE6 that does not reach 8, an unexpected identity, a wrong SMC0
    mapping, a write-back translation of a shared page, and a start
    refused by the deadlines;
  - a seeded mutant of the SMC0 write value.

  The complete case also requires the timer record before the first secure
  write, publication of exactly the three firmware buffers before the first
  GPU register write, the record's CR value, K and clock, and system init's
  clock and marker.
- The GPU service host checks that link the controller and the shared-memory
  adapter pass at the controller change: the backend reference, native and
  emulation checks, the service, memory-model, interrupt and mimus-service
  cases, and the reserved-memory check
  (`~/.cache/sunstoneos/evidence/gpu-fw-start-regress-20261025/`). The
  power-service model's case 1 fails at its fixture's `power_update`, which
  refuses the CORE6 bank; it fails identically before the change.

**Image comparison.** At this branch the power-only (`cec1c309…`),
identity (`9dc33dd8…`), wrapper-read (`1ab2f6d4…`) and probe-read
(`8a1c1b8a…`) images are byte-identical to those of set
`board-bundles-20261025-18`; the probe-read image is the one that passed
step 1.
