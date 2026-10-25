# Starting the GPU firmware from the open-source host driver

Status: **step 1 implemented and host-checked; step 2 designed.** The probe-read
diagnostic below is frozen for port 3-1. The firmware start itself is the
next change; nothing in this note has run on a board.

The GPU now powers on and off on the board in the vendor's state
([the default GPU power path](gpu-vendor-default.md)). This note derives what
the vendor's host driver does between "CORE6 status 8, identity readable"
and "firmware started", compares it with SunstoneOS's existing start
([docs/gpu-start.md](../gpu-start.md)), and plans the two board steps.

**Sources.** Every step comes from the open-source host driver and its
interface headers. They are the Imagination DDK as shipped in orangepi
`linux-orangepi` `2ac08e8c`, cached in
`~/.cache/sunstoneos/sources/gpu-fw-start-ddk-2ac08e8c/` with each file's
URL and SHA-256 in `manifest.json` (paths below are relative to
`rogue_km/`), and the vendor platform glue `rgx_sunxi/` with its build
configuration `config_kernel.h`. The firmware binary is never disassembled
or decompiled (`LICENSE.powervr` line 14); only the container fields the
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

### The start, in the order the power framework runs it

`RGXPostPowerState` (`services/server/devices/rgxpower.c:1010`) runs after
`sunxiPrePowerState` has opened the clocks and TOP5
(`rgx_sunxi/sunxi_platform.c:342–351`):

1. **Timer correlation** (`rgxpower.c:1030`; `RGXTimeCorrBegin`,
   `services/server/devices/rgxtimecorr.c:662–679` → `_RGXMakeTimeCorrData`,
   lines 271–343). The host reads `RGX_CR_TIMER` (`0x160`, 64-bit), reads it
   again if bit 63 differs from bit 31, and keeps bits 47:0. It reads its
   own time in ns. It writes a 40-byte record {OS ns, 0, CR ticks, K, clock
   Hz, 0} into the next of 256 slots of the GPU utilisation buffer, then a
   barrier, then the sequence count. K = (256·10⁶ ≪ 20) / clock kHz
   (`services/server/devices/rgxtimecorr.h:77–82, 105–119`): 266,305,015 at
   1,008 MHz.
2. **Secure configuration** (`RGXDoStart`, `rgxpower.c:865–889`). This build
   does not define `SUPPORT_TRUSTED_DEVICE`, so it calls
   `pfnTDSystemSecureConfig` = `sunxi_secure_config`
   (`rgx_sunxi/sysconfig.c:371`): `writel(2, 0x0A0000B0)` (SMC0
   `SMC_DRM_GPU_HW_RST`), then `writel(0, 0x01880014)` (wrapper
   `GPU_DRM_CTRL`) (`rgx_sunxi/sunxi_platform.c:960–979`), with no read-back,
   on every power-on, before the first GPU register write.
3. **`RGXStart`** (`rgxstartstop.c`), offsets from
   `hwdefs/rogue/km/rgx_cr_defs_km.h`:
   1. `SYS_BUS_SECURE` (`0xA100`) ← 0, read back (lines 855–860);
   2. `SOFT_RESET` (`0x100`, 64-bit) ← full mask, then Rascal and dusts
      released, then only GARTEN held, each read back (lines 787–815);
   3. `SLC_CTRL_MISC` (`0x3800`) read and written with the hash and burst
      bypass (lines 562–590);
   4. the eleven MIPS wrapper writes: register base, three boot remaps and
      the BRN 63553 remap, the address-limit and debug words
      (lines 287–456);
   5. `AXI_ACE_LITE_CONFIGURATION` (`0x38C0`) (lines 710–772);
   6. 3 µs, `SOFT_RESET` ← 0, read back, 3 µs (lines 819–829).
4. **Wait** (`rgxpower.c:1039–1052`): a barrier, then a poll of the 32-bit
   `bFirmwareStarted` word for 1, with a cache invalidate before each read,
   every 50 µs for `MAX_HW_TIME_US` = 500,000 µs
   (`rgx_sunxi/sysinfo.h:48, 51`; `services/server/common/pvrsrv.c:3240–3241`).
   The host never clears the flag before a later power-on.

## What SunstoneOS already does, and what it lacks

SunstoneOS's start ([gpu-start.md](../gpu-start.md),
[pvr_gpu_start.c](../../drivers/gpu/pvr_gpu_start.c)) was derived from
upstream Linux and matches the vendor's register sequence write for write.
Both write `SYS_BUS_SECURE`, the three `SOFT_RESET` values,
`SLC_CTRL_MISC`, the eleven wrapper writes in their order and the AXI value,
wait 3 µs on each side of the release, and set up no BIF. Its container parsing, section placement and
boot data match the DDK's and are stricter.

It differs or lacks in five places:

| Item | Vendor DDK | SunstoneOS today | Change |
| --- | --- | --- | --- |
| Secure configuration | SMC0 `+0xb0` ← 2, `drm_ctrl` ← 0 before `RGXStart`, every power-on | Not written | Add, through a platform callback, before the `SYS_BUS_SECURE` write |
| Core clock to the firmware | `clk_get_rate`, the configured operating point | 400 MHz passed by the callers, while the core runs at 1,008 MHz | Pass `A733_GPU_CORE_HZ` to system init and runtime configuration |
| Timer-correlation record | `RGX_CR_TIMER` read and a record written before the start | No read; the utilisation buffer is zeroed, so the firmware sees sequence 0 and K = 0 | Read the timer and publish one record, sequence 1, before the start |
| Started wait | 32-bit word equal to 1, 0.5 s | One byte, caller's deadline | Wait on the 32-bit word; deadline 1 s, inside the monitor's 2 s |
| `SOFT_RESET2` (`0x108`) | Never written by DDK 24.2 for this core | Written as upstream does (`pvr_gpu_start.c:116–120`) | Keep upstream's; drop it first if the start stalls at reset |

The firmware heap size, the structure layouts and the MIPS page-table
geometry stay upstream's: the open firmware is built against them, and the
DDK's 32 MiB heap and its system-init layout (which begins with a MIPS
state block, `include/rogue/rgx_fwif_km.h:1484`) belong to the vendor's
firmware. The runtime fields the DDK also sets (active power latency 10 ms,
HCS deadline, firmware watchdog 2 s, `rgxfwutils.c:2333–2357`) stay zero
until the start shows whether the open firmware needs them; the host source
does not say.

## Board steps

Each step is one bounded, returning image on port 3-1, after a power
removal, the cold DDR start and the frozen provider-readiness checkpoint.

### Step 1: the probe-read with the secure configuration

The probe-read diagnostic (bundle 17, `apps/gpu-power-id/probe-read.c`) on
the default path. It powers the GPU on to CORE6 status 8, required within
200 ms. It reads `drm_ctrl`, `irq_ctrl` and SMC0 `+0xb0`, which no board
has read, then the identity. It makes the two secure-configuration writes
in the vendor's order, each read back, reads the identity again, and writes
both words back to their entry values. It then powers the GPU off, ends its
supply sample and parks the provider. Changes from the probe-read that
stalled on `board-session-20261025-1`: it now runs on the default path, it
requires CORE6 ON before any GPU-window access, and it ends with the OFF and
the park of the power-only image, so the lifetime can continue.

Predictions: both identities `36.56.104.183`; SMC0 `+0xb0` reads 2 and
`drm_ctrl` 0 after the writes; both restored; `GPU PROBE window
serror_pending=0`; `COMPLETE GPU probe read: identity 36.56.104.183 before
and after the secure configuration, restored, OFF and provider parked`.

Controlled failure states:

- an ON that does not reach CORE6 8 stops with no GPU-window access
  (`GPU ON deadline`);
- a mismatched identity or restore stops after the restore with the GPU on
  (`GPU probe read: identity or restore not as expected`);
- a synchronous abort in the write window is taken through the restore
  vectors, which write both words back before the fault report;
- a window that would end within 500 ms of the monitor's 2 s deadline is
  refused with status 7 and nothing written.

An access that never completes stops the CPU; the monitor does not return,
and only a power removal recovers the board.

Deadline: the client's session deadline bounds the run; the ON wait is
200 ms; the write window is bounded at 10 ms and starts only while 500 ms
remain before the monitor's 2 s recovery.

### Step 2: the firmware start (planned)

The image powers the GPU on as in step 1, then prepares and starts the
firmware with the existing controller and the changes above, in this order:

1. firmware placement and the MIPS tables;
2. system and OS init with the core clock at 1,008 MHz;
3. one timer-correlation record from an `RGX_CR_TIMER` read;
4. the secure configuration;
5. the 35 steps of `pvr_gpu_start.c`;
6. a wait of at most 1 s on the started word. After the start it reports the started word,
the boot-data and register words the host can read, and the firmware's
first host-visible state, then powers the GPU off. The firmware itself is
never read beyond its shared host structures.

Predictions: the started word becomes 1 within 0.5 s, as the vendor's
bound; the firmware writes its first status into its shared structures.

Controlled failure states: a start that times out leaves the firmware
processor released and the GPU powered. The image then reports the step,
the MIPS wrapper words, `SOFT_RESET` and the started word through host
reads, and powers the GPU off through the controller's OFF, which needs no
firmware. A register access that never completes stops the CPU, as in step
1.

## Design gate

**Cost.** Neither step is a hot path: each runs once per GPU power-on. The
probe-read adds fifteen 32-bit accesses and three snapshots of 35 words to
the power-only image; the timer record adds two 64-bit reads and one
40-byte write per power-on.

**Checks.** The probe-read has thirteen host-check cases
(`apps/gpu-power-id/check.py --probe-read`):

- the complete path, with OFF, supply end and park;
- an ON that does not reach CORE6 8, and an OFF that fails after the window;
- three identity and restore mismatches and a synchronous abort in the
  window;
- the mapping, time-budget and window refusals and an admission refusal;
- two table mutants.

**Image comparison.** At `0f8e4406`, the power-only (`cec1c309…`) and
identity (`9dc33dd8…`) images are byte-identical to those that passed on
port 3-1 in `board-bundles-20261025-17`; only the probe-read image changes.
