# Bring-up gaps against the accepted goal

The [accepted goal](../goal.md) makes SunstoneOS a complete FreeRTOS-based
operating system for the Orange Pi Zero 3W (Allwinner A733). It is complete when
the following all exist:

- reproducible bootable images;
- recovery and update procedures;
- the mimus speech recognizer running natively;
- physical qualification of every required hardware function;
- representative concurrent workloads;
- warm recognition in at most 0.05 times the audio duration.

This report lists what stands between the repository and those requirements. It
covers branch `audit/integration` at commit `70a4e63`. Each gap cites an audit
finding identifier, such as `storage-6`. The identifier names the audit record
that holds the full evidence, so the other reports in this directory can be
cross-referenced.

## Terms and constraints used throughout

**Status levels.** The project records four levels of support:

- *implemented*: code exists;
- *software-tested*: host checks pass against modeled hardware;
- *physically working*: the function has worked on the board;
- *qualified*: the function meets its acceptance criteria on the board.

Almost every device path is implemented and software-tested. No device path is
qualified. On the board, all eight CPU cores have reached their first FreeRTOS
task and returned to the boot ROM. Cores 0 to 5 are Cortex-A55 and cores 6 and 7
are Cortex-A76 ([hardware matrix](../hardware.md)).

**Boot vocabulary.**

- *FEL* is the Allwinner boot-ROM USB mode. In FEL mode the host writes an image
  into board RAM and starts it. Every SunstoneOS image runs this way today.
- *boot0* is the vendor first-stage loader on the microSD card. It initializes
  DDR and loads one payload, called the monitor, to address `0x47000000`.
- *EL3* is the highest Arm exception level. SunstoneOS images enter at EL3, set
  up the interrupt controller and caches, and drop to EL1 to run FreeRTOS.
- A *launch contract* is a sealed record that the host writes into board RAM
  before it starts an image. The contract holds deadlines, watchdog settings
  and FEL-return settings. The image refuses to run, or *admission fails*,
  unless the record passes its checks.
- A *supervised image* runs under such a contract for a fixed lifetime of
  2 to 180 seconds and then returns to the boot ROM.

**Board access.** One board is connected, on host USB port 3-1 in FEL mode. If
it loses its USB connection it cannot be reconnected until further notice. The
[handoff](../handoff.md) also records that hardware access is unavailable and
that no board test is queued. In this report, *offline* work means code, host
models, cross-compilation and documentation. *Board* work means any step that
runs on the board. *Accessory* work needs a device that is not attached: an
OV5640 camera module, a DSI panel, a microphone, a Bluetooth headset, a USB
device for the host port or a PCIe endpoint.

**Project policies that shape the fixes.** [AGENTS.md](../../AGENTS.md) and the
goal require minimal designs, fixed board configuration and abstractions only
for a concrete consumer. Resources are retained after any failure whose
hardware outcome is uncertain. Persistent writes to a card require established
recovery and preservation of the existing card contents. The recommendations
below follow these policies.

## Summary by requirement

| Goal requirement | Current state | Main gaps | Offline work available |
| --- | --- | --- | --- |
| Integrated bootable image | About 35 separate per-device images; a few partial compositions | `bringup-gaps-1`, `build-system-2` | Integrated target, static interrupt table, packaging target |
| Reproducible images | Image bytes depend on build paths; toolchain and some inputs unpinned | `bringup-gaps-5`, `build-system-6`, `build-system-9`, `x-duplication-8`, `checks-harness-10`, `checks-harness-11` | All of it |
| Persistent boot | Card package boots a monitor that records state and parks | `cpu-startup-2`, `storage-6`, `bringup-gaps-3`, `bringup-gaps-2`, `supervisor-apps-9` | Loader design and host models; exclusion research |
| Recovery and update | Backup-first single-range writer exists; no layout or procedure | `bringup-gaps-4`, `storage-7`, `build-system-9` | Layout, procedure, planning tool |
| Multicore scheduling | First-task entry on all eight cores passed on the board | `smp-2`, `bringup-gaps-10` | Eight-core models; native diagnostic phase |
| Hardware classes | Implemented and software-tested; nothing qualified | 27 findings, listed in the hardware section | Most fixes; qualification needs board or accessory |
| Concurrent workloads | Partial compositions only; every image ends at a fixed deadline | `bringup-gaps-1`, `bringup-gaps-2`, `npu-9` | Integrated image and resident lifetime |
| Live audio input | Finite microphone capture and Bluetooth headset path, both unqualified; no microphone attached | `audio-2`, `bringup-gaps-9`, `bluetooth-5`, `bluetooth-6`, `bluetooth-7`, `buses-pcie-4` | Fixes and models |
| Native mimus at 0.05x | 2.2x on one A55 core at 494 MHz; two workers | `mimus-perf-1`, `clock-power-pins-5`, `mimus-perf-5`, `security-misc-1`, `gpu-2`, `npu-7`, `npu-8` | Warm path, clock reporting, modeled clock change |

## 1. Integrated bootable image

### Devices are separate images that cannot be linked together (`bringup-gaps-1`)

**What exists.** The build defines about 35 supervised per-device executables.
Each is excluded from the default build and enabled by its own option. Some
fixed compositions already combine devices:

- the mimus board image combines native mimus, USB0, the USB2 host, a keyboard
  and USB storage;
- an accelerator image combines GPU compute and an NPU graph;
- other images combine GPU with HDMI, I2S with mimus, and Bluetooth with mimus.

**What is missing.** No image combines the board composition with the devices
that bypass it. The board composition covers USB0, the USB2 host, radio, the
crypto engine and Type-C. The devices that bypass it are I2S, HDMI, camera, GPU
and NPU. Three mechanisms prevent combination:

- Each device adds its interrupt with a `platform.patch` file that inserts one
  interrupt number into the shared handler in
  [platform.c](../../platform/a733-smp-cached/platform.c). Patches for
  different devices cannot be combined into one handler.
- The board composition exposes one link-time application hook,
  `board_application_start` in
  [application.h](../../apps/board/application.h). Twenty-three files define it,
  so any two board applications produce duplicate symbols.
- Eleven images, including I2S, HDMI, camera, GPU render and NPU runtime,
  define their own `a733_smp_diagnostic_start` and never use the board
  composition.

The handler stops the system with status 4 when an interrupt has no
dispatcher. That stop is the documented fail-closed path, and it must stay.

**Offline now.**

1. Add one fixed integrated target built from the mimus board image. Add I2S
   receive first, because live audio is the next consumer.
2. Replace the per-device interrupt patches with one static table of
   interrupt number, owning core and handler in the shared platform file. Keep
   the per-interrupt core check and the status-4 exit for numbers outside the
   table.
3. Replace the single start hook with a fixed list of named start functions.
   Each function keeps its application's ordering (pins before providers, start
   after binding) and its session validation.
4. Add a link check that interrupt numbers are unique, every handler is linked
   and no symbol is duplicated. Extend the modeled interrupt fixtures to deliver
   each listed interrupt through the table.

Keep the existing per-device images unchanged so their recorded evidence stays
valid.

**Needs the board.** Qualification of the integrated image.

**Depends on.** A resident lifetime (`bringup-gaps-2`) for any run longer than
60 seconds, and sustained multicore scheduling (section 4).

### No target produces a bootable package (`build-system-2`)

**What exists.** [prepare-cold-entry.py](../../tools/prepare-cold-entry.py)
converts the cold-entry monitor to the flat payload format.
[prepare-microsd-boot.py](../../tools/prepare-microsd-boot.py) wraps that payload
with the pinned boot0 into a 20 MiB card container. The boot0 binary is pinned
in [sources.lock.json](../../sources.lock.json).

**What is missing.**

- The vendor packer, `dragonsecboot`, is pinned only by a hash constant inside
  the tool. It is absent from the lock file, so the fetch tool cannot supply it.
- No CMake target produces a container.
- The FreeRTOS images link at `0x41000000` and lack the header that boot0
  requires, so the packer rejects them. That header is the `monitor` tag at
  offset 4, the load word `0x47000000` at offset `0x2c` and a branch at
  offset 0.

**Offline now.** Add a lock entry for the packer. Make the packaging tool check
both boot0 and the packer against their lock entries. Optionally wrap the two
tools in one build-directory target that never writes a device. A target that
packages a FreeRTOS image must wait for the stage-2 loader (section 2).
`CMakePresets.json` is optional cleanup.

### Reproducible images

The goal requires images that can be rebuilt bit-for-bit from the repository and
pinned sources. Six findings block that. All of the work is offline.

**Build paths in image bytes (`bringup-gaps-5`).** `configASSERT` in
[FreeRTOSConfig.h](../../include/FreeRTOSConfig.h) passes `__FILE__` to the panic
routine. No target maps file prefixes. Absolute worktree, cache and build paths
therefore land in loadable read-only data, and the same commit built in two
directories produces different images. Clang and LLD come from locally extracted
Ubuntu packages that the lock does not pin. The existing build-id hashes input
contents, compiler name and version, so it is independent of paths. It
identifies the inputs and is not a digest of the image.

- Fix: add one shared option list with `-ffile-prefix-map` for the repository
  root, the build directory and the source cache root. The cache root covers
  FreeRTOS, FreeRTOS-Plus-TCP and TinyUSB.
- Fix: pin the Clang and LLD package archives with hashes.
- Check: build one image in two directories and compare the loadable segments.
- Apply this to new builds only; recorded images keep their recorded hashes.

**Cache paths that the fetch tool does not produce (`build-system-6`).** The
board build, and every image derived from it, reads the Cadence PHY source from
a hard-coded `$HOME` path. The lock entry for the same file stores it under a
different directory. The deinterlace build reads inputs that have no lock entry
at all.

- Fix: add a `SUNSTONE_SOURCE_CACHE` variable and a helper that resolves each
  input through its lock archive path.
- Fix: add lock entries for the deinterlace inputs.
- Fix: update the checks that hard-code the old layout, such as
  [pcie-pd-rail.py](../../checks/pcie-pd-rail.py).

**Unverified external inputs and no build recipe for the recovery tool
(`build-system-9`).** Configuration accepts a FreeRTOS directory if one file
exists in it. It does not check TinyUSB, newlib or mimus against the lock, and it
does not check the compiler version. The four `sunxi-tools` patches have no
consumer in the repository. The patched `sunxi-fel` binary is the FEL recovery
tool, and only a cached copy of it exists.

- Fix: verify the compiled files of each external tree against pinned hashes.
- Fix: fail configuration when Clang is older than version 21.
- Fix: add a host-only script that builds `sunxi-fel` from the locked
  `sunxi-tools` archive with the four patches in one fixed order. The script
  compares the result with the recorded hash and never runs the tool.

**A second build system outside CMake (`x-duplication-8`).** Seven Python
scripts build eight returning and finite images, such as the RTC, thermal, NPU,
USB and GPU finite images. They take compile flags from dated evidence
directories and rewrite an old worktree path in them. They then link prebuilt
objects from those directories with units compiled from current headers. The
shared struct `a733_platform_locks` changed layout on 2026-10-20. A rebuild can
therefore join objects that disagree on that layout.

- Fix: add one CMake platform library for returning images and a
  `sunstone_returning_image` function.
- Fix: move the shared closure, tool-hash and evidence-freeze logic into one
  helper.
- Keep `apps/rtc-returning`. The [RTC application record](../rtc-application.md)
  documents it as the read-only finite client; add a path reference to it there.

**No index or runner for checks (`checks-harness-10`).** Of 537 tracked Python
checks, 268 are named in no other tracked file. Nothing maps a changed source
file to the checks that cover it, although the
[validation guide](../validation.md) requires that selection.

- Fix: add a checked-in index built from each check's existing scope docstring
  and dependency lists.
- Fix: add a runner for the pure-host subset. That subset excludes checks that
  need an image, USB access or cached evidence.

**Incomplete host requirements (`checks-harness-11`).**
[host-requirements.txt](../../checks/host-requirements.txt) lists only unicorn,
pyusb and pyelftools. Fourteen checks import capstone, including the cached SMP
model that many other checks load. Other checks import cryptography, numpy and
PyYAML.

- Fix: pin capstone 5.0.9, which is the installed version, and the other three
  packages.
- Optionally move the NPU tool dependencies into their own requirements file.

## 2. Persistent boot

### The cold boot chain stops at a parking recorder (`cpu-startup-2`, `storage-6`, `bringup-gaps-3`)

These three findings describe one gap from the CPU, storage and system
viewpoints.

**What exists.**

- The [card packager](../microsd-boot-package.md) accepts one flat monitor
  payload of at most 4,176,896 bytes, loaded at `0x47000000`.
- The only real payload is the [cold-entry recorder](../cold-entry.md)
  ([cold-entry.S](../../boot/a733/cold-entry.S)), a 4,164-byte monitor. It
  checks CPU state, records a report in RAM and parks in a wait loop.
- Host models of the actual boot0 parser and eight AArch64 entry cases pass.
- Nothing has booted from a card on the board.

**What is missing.** No component loads FreeRTOS or the mimus models from the
card. A FreeRTOS image cannot simply replace the monitor:

- FreeRTOS images link at `0x41000000` with 2 MiB of code and 14 MiB of RAM.
  The runtime link script allows up to 15 MiB, which exceeds the 4 MiB monitor
  window.
- Images enter at EL3 and require a sealed launch contract at `0x42004000`
  written by the host.
- The mimus build reserves three asset slots at `0x44000000`, `0x44800000` and
  `0x45000000`, about 17 MiB in total. The actual assets are about 14.2 MB: two
  model files and one recorded test WAV. A 16 MiB arena at `0x46000000` ends
  where the monitor window begins.

**Prerequisites that are open research.** The
[cold-entry record](../cold-entry.md) requires two facts before any application
authority is constructed. Remote processors and DMA engines that could write
memory must be excluded, and memory must be published. Three items are
unresolved:

- **E902 core.** No stop procedure is documented. The record says that reversing
  the shipped monitor's start writes would not establish a safe shutdown or
  memory drain. The cold payload omits the shipped monitor, so the core may
  never start. That must be shown from the manual and evidence before it is
  relied on.
- **SCP ownership.** The [boot record](../boot.md) leaves the system control
  processor's ownership undecided.
- **Memory overlaps.** boot0 stages data at `0x42e00000`, and the record notes a
  conflict at `0x48000000` with the device memory pool. Both need an owner
  before a loader copies images.

**Design constraints for the loader.**

- *Second card item.* boot0 takes the monitor load address from payload offset
  `0x2c`. The cold-entry checks require the U-Boot and OP-TEE pointers that
  boot0 writes into the header to be zero. Nothing shows that the pinned boot0
  loads a second package item to a usable address. That must be checked
  against the boot0 parser model before a second item is used.
- *Card reads.* The existing polled card read path runs inside FreeRTOS
  applications through the shared clock, pin, rail and PMIC providers. A stage-2
  loader running at EL3 with caches off needs a fixed-board subset of that
  bring-up.
- *Cache maintenance.* The loader must clean caches after copying and before it
  enters the OS at EL3.
- *Launch record.* The host contract stands for facts the host proved: watchdog
  and FEL qualification, exclusive ownership and the lease. The loader must not
  fill the same fields itself. A cold boot needs its own contract version with
  its own admission rules. The resident contract below is the candidate.
- *Recovery.* The monitor's recovery is a warm reset into the ROM at `0x20`.
  The watchdog-driven FEL path also depends on an RTC FEL flag and a redirect
  flag that only the FEL launchers write. Whether the ROM at `0x20` enters FEL
  after a cold boot without those flags is unverified.

**Offline now.**

- Research and document the E902 and SCP exclusion from the manual and the
  recorded monitor evidence.
- Design a minimal stage-2 loader that reads one fixed card layout, checks
  SHA-256 hashes, copies the OS and models to their linked addresses, builds the
  resident launch record and enters the OS at EL3.
- Keep the recorder's admission checks as the loader's entry gate.
- Extend [cold-entry.py](../../checks/cold-entry.py),
  [boot0-monitor-handoff.py](../../checks/boot0-monitor-handoff.py) and
  [microsd-boot-package.py](../../checks/microsd-boot-package.py) to model the
  card read, the copy and the entry.
- Extend the package validation with a manifest record that names entry
  address, extent and image hash.

**Needs the board.** Any physical observation requires writing a card. That
requires established recovery and preservation of both cards
([goal](../goal.md), section 4). The handoff assigns cold-entry and standalone
boot to the primary agent.

### No resident lifetime exists (`bringup-gaps-2`, `supervisor-apps-9`)

**What exists.** Every image runs under a contract with a fixed end:

| Contract variant | Lifetime | Watchdog and FEL fields |
| --- | --- | --- |
| Generic cached admission ([admission.c](../../platform/a733-smp-cached/admission.c)) | 4 s watchdog lease | Required, FEL flag `0x5aa5a55a` |
| Supervised board ([admission.c](../../apps/board/admission.c)) | 60 s; 20 s for G2D, DisplayPort, deinterlace and ISP0 | Required to be zero |
| Armed board and storage maintenance | Session deadline below the recovery bound | Required |
| Supervised mimus | 180 s | Required to be zero |
| Device launch ([launch.c](../../platform/a733-device-launch/launch.c)) | 2 s diagnostic window, 4 s watchdog | Required |

The shared interrupt handler stops the system once the counter passes the
diagnostic deadline. Board images also stop their services when
`board_live()` turns false at the session deadline. The armed board path reloads
the watchdog only when radio progress advances, and each reload is capped at the
session deadline. The single-core USB diagnostic runtime has a resident mode,
but it is FEL-entered and single-core.

**What is missing.** A resident contract version for the SMP runtime with its
own magic number and version. It needs:

- no terminal diagnostic deadline and no FEL redirect;
- a liveness source independent of any one workload;
- the interrupt-path deadline check applied only to the supervised versions,
  while the per-core startup deadline checks stay;
- resident equivalents for `board_live()`, the service deadline and the
  monitor's sample-length limit.

**Constraints.** In supervised builds the deadline is the only bounded-failure
mechanism, because the watchdog fields are zero. An armed resident contract
depends on watchdog-expiry recovery, and that trial is deferred
([bring-up sequence](../bringup-sequence.md)). A contract with neither a deadline
nor a qualified watchdog has no bounded failure. Add the resident contract
together with its producer, the stage-2 loader, as the concrete-consumer rule
requires.

**Offline now.** Admission cases for both contract versions in the existing
admission checks. A `qemu-smp` model run past 60 modeled seconds in which
watchdog renewal continues while services progress and stops when one stalls.

**Needs the board.** The watchdog-expiry recovery trial.

## 3. Recovery and update

**What exists.**

- The [storage update service](../storage-update.md) writes one host-granted
  sector range. It denies writes by default, backs up the range first and
  re-authorizes every write command. It reads each write back and binds the
  backup hash to the grant.
- [storage-update.py](../../tools/storage-update.py) provides backup, prepare and
  restore flows.
- Physical FEL recovery through the BOOT and GND pads is documented in the
  [board wiring record](../board-wiring.md).
- The service is not physically admitted, and no card write has ever been
  performed.

### No card layout and no install, update or rollback procedure (`bringup-gaps-4`)

No document defines which card sectors SunstoneOS owns, how to install it while
preserving existing contents, how to update it or how to restore it. The
smallest compliant step is documentation, marked unimplemented:

- a fixed layout: boot0 at byte 8,192, the monitor package at byte 16,793,600
  and one OS and model extent;
- an install procedure with a mandatory host backup of every affected extent.
  Sector ownership comes from a trusted allocation record and is never inferred
  from apparently free space;
- update and restore procedures built on the existing backup and restore flows
  and on FEL recovery.

Two-slot images with generation headers and automatic fallback are worth adding
only if unattended updates become a requirement. Leaving sector 24,576, the
U-Boot backup sector, unpopulated matches the upstream producer.

### No tool turns a container into update grants (`storage-7`)

The update tool needs one contiguous candidate equal to the granted range,
plus a hand-written allocation record. The packaged container has two meaningful
extents with zero-filled gaps between them, and the gaps must not be treated as
free. A grant covers one range per session, so the two extents need two
backup and write sessions.

- Fix: add a `plan` subcommand that reads the container manifest and the
  identified card record and emits per-session inputs with explicit sector
  bounds.
- Fix: skip extents whose bytes already match the backup.
- Fix: record what the backup shows at sector 24,576 instead of assuming a stock
  backup package is present there.
- Ordering the writes does not protect the card here. The container's boot0 is
  the unmodified stock boot0, so writing the package first already changes what
  that boot0 loads. Recovery relies on the restore flow.

**Offline now.** The layout and procedure documents, the planning subcommand and
a host check that feeds the synthetic container through it. The check asserts
two extents with correct sectors and no write to the gaps or to sector 24,576.

**Needs the board.** Every card write, after recovery is established.

**Related.** `build-system-9` covers rebuilding the FEL recovery tool. `usb-6`
covers uncontrolled ROM returns during USB transfers.

## 4. Multicore scheduling (`smp-2`, `bringup-gaps-10`)

**What exists.**

- **Board result.** All eight cores reached their first FreeRTOS task and
  returned. The full returning workload, with task bodies, timer interrupts and
  inter-core interrupts, passes only in software.
- **Scheduling design.** The system tick runs on core 0 only. Other cores are
  driven by software-generated interrupts (SGIs) from core 0.
- **Native diagnostic.** It pins 16 tasks, two per core. It requires every core
  to receive at least one SGI.
- **mimus.** The adapter on the eight-core configuration passes jobs through two
  queues between core 0 and one worker task pinned to core 1.
- **QEMU.** The emulated SMP target runs queues, mutexes and migration, but with
  two cores only.

**What is missing.** No check, modeled or physical, covers:

- eight-participant queue or mutex traffic;
- contention on the kernel locks;
- unpinned migration across all cores;
- time-sliced preemption on every core;
- a long-running soak.

These gaps can hide lock starvation, lost wakeups, wrong-core yields and
register corruption on migration between A55 and A76 cores. The
[SMP record](../smp.md) and the handoff already list this as outstanding.

**Offline now.**

1. Extend `qemu-smp` to eight cores: a CPU_ON loop, an eight-core configuration
   variant, `-smp 8` in [check-smp.py](../../tools/check-smp.py), and exception
   stacks in the link script sized for eight cores. Then run the existing
   kernel, context and runtime cases. QEMU indexes cores by affinity level 0
   and models only Cortex-A55. The run therefore tests the generic scheduler
   only. It covers neither the A733 topology, which indexes cores by affinity
   level 1, nor A55-to-A76 migration.
2. Add a second phase to one consolidated native diagnostic. The pinned
   workload is copied in `platform/a733-smp`, `a733-smp-cached` and
   `a733-smp-observe`. The phase adds a cross-core queue ring, a shared mutex
   with checked invariants, unpinned tasks that migrate across cores 0 to 7
   with full register checks, and a timed soak. Run it first in the instruction
   model, which reports an A76 identity on cores 6 and 7.
3. Extend the host models with tick loss, tick jitter and SGI loss.

**Needs the board.** After recovery is available, the primary agent runs one
bounded test, the recorded full returning workload, after refreshing board
identity and admission. A sustained resident scheduler also needs the resident
contract (section 2), because the cached diagnostic platform is a finite,
deadline-bounded image.

**Blocks.** Multi-worker mimus, concurrent workloads and the resident OS.

## 5. Hardware classes

Only classes with confirmed gaps appear below. "Accessory" names the missing
device.

| Class | Findings | Needs board | Needs accessory |
| --- | --- | --- | --- |
| CPU clock, voltage, thermal | `clock-power-pins-5`, `mimus-perf-1`, `security-misc-1` | Yes | No |
| DMA | `dma-iommu-2` | Yes | No |
| USB device and host | `usb-6`, `usb-8` | Yes | USB device for the host port |
| Wi-Fi | `radio-net-6`, `radio-net-9`, `radio-net-10` | Yes | Access point |
| Bluetooth | `bluetooth-5`, `bluetooth-6`, `bluetooth-7`, `buses-pcie-4` | Yes | Headset |
| I2S audio | `audio-2` | Yes | Microphone |
| Display | `display-3` | Yes | DSI panel |
| Camera | `camera-1`, `camera-2`, `camera-3`, `camera-10` | Yes | OV5640 module |
| GPU | `gpu-2` | Yes | No |
| NPU | `npu-7`, `npu-8`, `npu-9` | Yes | No |
| Video codecs | `video-8`, `video-9`, `video-10` | Yes | Test streams only |
| UART and PCIe | `buses-pcie-4`, `buses-pcie-5` | Yes | PCIe endpoint |

### CPU clock, voltage and thermal control

**CPU frequency (`clock-power-pins-5`, `mimus-perf-1`).**

*What exists.* The A55 cores run at 494 MHz, set by boot0. That rate is decoded
from registers as 26 MHz × 38 ÷ 2; no independent frequency measurement exists.

*What is missing.*

- No code programs the CPU, big-core or DSU clocks, or the CPU rails. The DSU is
  the shared L3 and interconnect unit of the CPU cluster.
- The clock registry has no CPU PLL scope.
- The PMIC provider ([a733_pmic.c](../../drivers/power/a733_pmic.c)) is read-only
  by contract and has no profiles for DCDC5 or DCDC3. The board device tree
  feeds cores 0 to 5 and the DSU from DCDC5, and cores 6 and 7 from DCDC3. The
  rails already exist as rail-ledger identities.
- The clock report in [recorded.cc](../../apps/mimus/recorded.cc) is compiled
  out of native images. It decodes the post-divider P as `1 << P`, while Linux
  uses `P + 1`; the two agree only for P of 0 or 1.
- `configCPU_CLOCK_HZ` claims 1 GHz but nothing uses it.
- The [hardware matrix](../hardware.md) labels "Thermal and frequency control"
  implemented, although no frequency control exists.

*The operating-point table is already cached.* The vendor table is at
`~/.cache/sunstoneos/sources/npu-platform/sun60iw2p1-cpu-vf.dtsi`: A55 table at
line 20, A76 table at line 623 and DSU table at line 1365. Voltages depend on
the chip's speed bin, which is read from eFuse through `vf_mapping_table`. The
board's 26 MHz reference selects the `opp-microvolt-26m-*` columns. Under those
columns:

- 1014 MHz runs at 800 mV for bins vf0000, vf0100 and vf0200;
- 1794 MHz needs 1000 mV on vf0000 and 1050 mV on vf0200;
- vf0100 stops at 1612 MHz.

Clearing the CPU divider while keeping the PLL at 988 MHz gives at most a
2.0-times speedup for compute-bound stages. 1.8 GHz gives at most 3.6 times.
Memory-bound stages scale less.

*Offline now.*

1. Read-only reporting of the CPU, big-core and DSU clocks in every image,
   including native ones, with the linear divider decode.
2. Record the operating-point table and the bin-selection rule; add a
   read-only eFuse speed-bin read.
3. Add DCDC5 and DCDC3 decode profiles, citing the PMIC datasheet for their
   selector ranges.
4. Model a fixed operating-point change in a host check. The change goes
   through the PMIC provider and an exclusive rail-ledger change mode that
   covers both the cores and the DSU on DCDC5. The sequence raises the voltage,
   reads it back, waits for the ramp at 250 µV/µs, switches the PLL through a
   safe mux and verifies lock.
5. Correct the hardware matrix and `configCPU_CLOCK_HZ`.

*Needs the board.* Every divider, PLL or voltage write can hang the board, and a
hung board cannot be reconnected. These writes need explicit user approval and
the supervised admission path. Even the divider-only step first needs a
confirmed rail voltage of at least 800 mV.

*Depends on.* A live thermal policy, described next.

**Thermal policy (`security-misc-1`).** The thermal driver
([a733_thermal.c](../../drivers/thermal/a733_thermal.c)) programs alarm and
critical thresholds and enables their interrupts. Every consumer only copies
those events into USB evidence. Nothing reduces load, stops work or powers off.
The mimus image has no thermal sensor owner. The [thermal record](../thermal.md)
lists this as required work.

- *Offline:* add a minimal policy task that owns the thermal instance through
  the same exclusive handoff and threshold admission used by `apps/monitor`.
  The thermal and monitor applications enable all five alarm and critical
  sensor bits, while the returning thermal image enables none, so the policy
  needs one fixed choice. On alarm, the policy holds back the extra mimus
  worker or duty-cycles work. On critical, it stops admission, records a
  terminal fault and requests PMIC power-off as a retained request whose
  completion is uncertain. A host check drives recorded code sequences and
  confirms that throttled work still earns watchdog progress.
- *Board:* physical thermal acquisition and interrupt delivery must be qualified
  first. A policy driven by unqualified readings cannot be trusted.

### DMA (`dma-iommu-2`)

The DMA controller contract keeps the DMA interrupt (INTID 143) masked. The
service worker polls once per 1 ms tick while any channel is active, so a
cyclic audio stream wakes it 1000 times per second. The
[DMA record](../dma.md) lists interrupt delivery as required. The cost is
unmeasured and probably small. The main benefit is lower completion latency.

- *Offline:* add a single-owner interrupt path. The interrupt service routine
  notifies the worker, and all register work stays in the existing poll. Keep
  polling as a slower fallback and keep the claim checks. Model an injected
  interrupt in the existing DMA service check.
- *Board:* delivery qualification. DMA consumer security admission is also
  unresolved project-wide (handoff).

### USB device and host

**Malformed requests reset the board (`usb-6`).** The USB0 diagnostics protocol
in [diagnostic.c](../../drivers/usb/diagnostic.c) calls the panic routine, which
returns the board to the ROM, on a bad header magic or an oversized length. It
cannot resynchronize or cancel, and the log command returns only the first
2,048 bytes of a log of up to 16,127 bytes. In a mimus session, one host
timeout during model upload can discard the uploaded models.

- *Offline:* reply with an error status instead of panicking. Add a vendor
  control request that clears partial request and response state, including the
  completion and recovery flags, and flushes the vendor FIFOs. An untrusted
  length field cannot be relied on, so a bad magic still needs that flush. Add
  a log offset to the log command. Test with the existing TinyUSB stub model.

**No USB host detach or reattach (`usb-8`).** The xHCI command set in
[a733_xhci.c](../../drivers/usb/a733_xhci.c) lacks Disable Slot and Stop
Endpoint. The TinyUSB adapter treats device close, endpoint close, a bulk
endpoint stall and unmount as permanent loss. A device can be used repeatedly
while it stays attached. A control-endpoint stall during the mass-storage LUN
query is already recovered. Unplugging, replugging or a hub port change still
ends host service until reboot.

- *Offline:* add an orderly detach used only when completion is certain. It
  stops each endpoint, disables the slot, clears the context entry and
  reinitializes that slot's rings from the retained memory. Cover hub child
  slots. Add mass-storage reset recovery before declaring failure. Uncertain
  cases stay permanently retained. VBUS removal does not prove that a
  self-powered hub lost power, so a new Type-C generation alone does not make a
  hub topology reusable.
- *Accessory:* a USB keyboard or storage device on the host port.

### Wi-Fi

**Slow one-bit SDIO bus (`radio-net-6`).** The radio runs at 400 kHz on one data
line with CPU FIFO access. The 391,000-byte firmware needs at least 7.82 s of
bus time at that rate ([board radio record](../board-radio.md)). That exceeds the
4-second watchdog window, so cold startup cannot fit inside it.

- *Offline:* first add four-bit width at default speed: CCCR register `0x07` and
  the controller width bit, with the clock limit raised to 25 MHz. Reuse the
  divider and timing logic in [sunxi_mmc.c](../../drivers/storage/sunxi_mmc.c)
  rather than writing a second copy. Add 50 MHz high speed through CCCR `0x13`
  next. Add internal DMA last, because it needs the unresolved DMA security
  admission. Batching FIFO moves must keep one guarded check of ownership,
  current task and deadline per batch.
- *Board:* one identity read at four-bit 25 MHz, compared with the 400 kHz
  result.

**Link loss is terminal (`radio-net-9`).** A deauthentication or beacon loss
moves the link to DOWN, which the network layer converts into a fault that
retains the whole radio, Bluetooth included. Reconnect requires a healthy link.
The [radio session record](../radio-session.md) gives the blocker: port closure
needs the station index that the disconnect handler has already invalidated.

- *Offline:* first establish from firmware or upstream source whether the
  disconnect indication frees the station's keys and port state. Only then
  define a clean-loss state: link DOWN, data idle, no security operation, no
  outstanding Wi-Fi command or confirmation deadline. Reconnect from that state
  through the existing phases.
- *Board and accessory:* qualification with a real access point.

**HT receive and A-MSDU are dropped (`radio-net-10`).** The connection profile
is 20 MHz legacy without HT. Receive records marked for reordering or A-MSDU
are counted as unsupported diagnostics and dropped. This matters only once HT
is enabled after the bus is faster. Before enabling HT, deliver reorder frames
in arrival order or through a small per-TID window, and split A-MSDU
subframes. Document that the applied AMSDU firmware patch has no effect under
the current profile.

### Bluetooth

**Link slots are never reclaimed (`bluetooth-5`).** Each connection attempt,
including rejected unknown peers, consumes one of four ACL link slots for the
controller's lifetime ([bt_acl.c](../../drivers/bluetooth/bt_acl.c)). A headset
that disconnects and pages again, or a fifth page from any peer, faults the
stack. The behavior matches its documented scope; reconnection is missing
functionality.

- *Offline:* reclaim a slot only after a confirmed Disconnection Complete, once
  every owner has released the generation. Those owners are ACL, L2CAP,
  security and SCO. Late events for the old handle are currently detected
  through the CLOSED slot itself. Reuse therefore needs a separate table of
  retired handles. Extend the incoming-connection check.

**One erroneous SCO packet ends capture (`bluetooth-6`).** The controller
reports damaged eSCO payloads. Capture stops on the first flagged packet or
mSBC sequence error, by documented policy. Normal 2.4 GHz interference,
including Wi-Fi on the same combo chip, may make repeated sessions fail.

- *Offline:* add an opt-in bounded concealment mode beside the strict default.
  It records each lost frame, conceals it and relocks the decoder on the next
  valid header. Recognition keeps refusing lossy captures unless the mode is
  enabled.
- *Accessory:* measure the real loss rate with a headset before choosing the
  loss bound.

**Peer data during local disconnect faults L2CAP (`bluetooth-7`).** While a
local ACL disconnect is pending, inbound ACL data on that link faults L2CAP.
No current application calls the disconnect, and power removal between launches
is policy.

- *Offline:* treat such frames as discardable and release them. Add a host check
  for peer data during a local disconnect. Defer an application-level HFP,
  RFCOMM, L2CAP and ACL close sequence until a multi-session consumer exists.

**UART throughput** for the headset link is covered under UART below
(`buses-pcie-4`).

### I2S audio (`audio-2`)

The transcription build captures mono 16 kHz audio in finite 4 KiB periods.
At each period boundary the driver stops the receive request, drains DMA and
submits the next period. Only the receive FIFO covers that gap, about 3.9 ms.
The DMA worker detects completion by 1 ms polling, so the expected margin is
about 2.4 ms after a worst-case one-tick delay. The
[I2S stream record](../i2s-stream.md) lists this margin as a qualification risk;
no overrun has been demonstrated.

- *Board and accessory:* record the measured bridge slack against the deadline
  during physical qualification. The driver already records it per period.
- *Offline, optional:* to remove every bridge, use the existing one-shot
  `a733_i2s_capture` contract with a hardware-linked chain of 4 KiB pages. A
  160,000-byte utterance needs up to 40 pages. Check descriptor capacity and
  the pool page budget first. Raising the 4096-byte period limit would break
  the pool's whole-page ownership model.

### Display (`display-3`)

The DSI scanout application never powers the display engine domain (PCK
domain 1) or the VO0 domain (PCK domain 9). It therefore fails when started from
a cold domain, while HDMI, DisplayPort and G2D power domain 1 themselves. The
deinterlace application adopts an already powered domain, as documented.

- *Offline:* document domain-ON as a DSI entry precondition. Optionally add the
  `a733_de_sys_power` step that G2D uses. Generalize it for domain 9 only after
  the VO0 power sequence is checked against the pinned `pck600_domains.c`.
- *Accessory:* no DSI panel is selected, so other DSI gaps come first.

### Camera

No OV5640 module is attached and no camera test has run. Four gaps block the
first acceptance result in the [camera record](../camera.md): one color-bar
frame with verified dimensions and ordering, then repeated stop and start.

- **Reset line (`camera-3`).** The native binding requires a separate reset
  GPIO. The 22-pin connector provides one control line per camera, PE6 on
  CAM1. Add an explicit power-down-only module profile selected by a flag in the
  boot record. The profile claims 14 pins instead of 15 and skips the reset
  state while keeping the power-down timing. An SCCB software reset after
  identification is an addition beyond Linux, which uses it only when there is
  no power-down line.
- **Color bars cannot be selected (`camera-2`).** The binding passes
  `color_bars=0` in
  [a733_camera_native.c](../../drivers/camera/a733_camera_native.c), and no
  record field selects the pattern. Add a test-pattern field to the boot record
  and bind it in [prepare.py](../../apps/camera/prepare.py), updating the record
  version and layout checks. Add an NV16 decoder and bar check in
  [observe.py](../../apps/camera/observe.py). Take the bar layout from the
  OV5640 source or datasheet.
- **One frame per boot (`camera-1`).** The coordinator, receiver and frame DMA
  have no restart lifecycle, so the image captures one frame per power cycle.
  The single-frame profile is documented, which makes the camera record's
  stop-and-start acceptance text inconsistent with it. First reconcile that
  text and capture color bars. Design re-arm as a separate lifecycle that keeps
  two existing guarantees: the producer stays quiet while capture is armed,
  and drain is proven before any buffer is reused. Keeping the sensor streaming
  between re-arms conflicts with the receiver contract.
- **Second connector (`camera-10`).** The receiver and route support parser 1,
  but the native binding accepts only CAM1. A CAM2 profile needs the pins listed
  in the [pin record](../pins.md): data PK10 to PK19, PE9 clock, PE10 control
  and TWI9 on PE14 and PE15. It also needs an MCLK2 register address and a
  deskew value from pinned sources, and a review of the receiver's port-0
  special case.

All four are offline work with host checks. Capture needs the module.

### GPU (`gpu-2`)

The compute path accepts one 4 KiB input page and one 3,968-byte output page.
Replies return 32 words at a time and the GPU memory map is fixed. That cannot
carry mimus weights. The goal schedules GPU evaluation after hardware
functionality, so this is a prerequisite for the performance phase rather than
a bring-up blocker. The reserved write-back pool has only 448 KiB free, so a GPU
mimus session first needs a larger DRAM reservation.

- *Offline:* first estimate CPU against GPU time for one mimus linear layer,
  including staging. Design a session with persistent weight mappings only if
  that estimate favors the GPU.

### NPU

- **No weighted mimus layer path (`npu-7`).** The assembler accepts one UINT8
  asymmetric-affine input and output. The decoder refuses per-channel
  quantization. The fixed FP32 relative-shift profile already runs a
  weight-free mimus subgraph. Missing is a linear layer that keeps mimus's int8
  per-row quantization. This is a performance-phase prerequisite. *Offline:*
  first check whether the pinned compiler can import mimus scales at all. Then
  build one exact-model profile for one linear layer and compare it bit-exactly
  with the NEON kernel.
- **Small fixed memory window (`npu-8`).** NPU mappings use 4 KiB pages in one
  16 MiB root inside a fixed 4 MiB parent at `0x41600000`. *Offline:* obtain a
  DDR reservation outside image RAM. Optionally allow 64 KiB pages per root while
  keeping the byte-for-byte table verification. The page-walk benefit is
  unmeasured.
- **Interrupt completion only on core 0 (`npu-9`).** The interrupt owner works
  only on core 0, while the multicore worker runs on core 1. *Offline:* make
  interrupt binding refuse unless the worker is pinned to core 0. Routing the
  interrupt to another core waits for a concrete multicore NPU consumer.

### Video codecs

- **HEVC sources are absent (`video-8`).** The HEVC CMake file lists nine
  sources that do not exist here. The parser and controller live in a separate,
  unreviewed worktree. *Offline:* replace the existing misleading error in
  [a733-hevc.cmake](../../cmake/a733-hevc.cmake) with one that names the missing
  integration. After review, integrate onto the shared decoder platform.
- **JPEG limited to about 64 by 64 pixels (`video-9`).** The native application
  is a 4 KiB fixture profile. The decoder supports 1920 by 1080. *Offline,* after
  the first physical fixture decode: add an extended-memory profile that reuses
  the H.264 full-HD allocation pattern. Also raise the dimension cap, the output
  capacity and the inline buffers.
- **H.264 High at 1080p limited to two references (`video-10`).** The pool holds
  three 1080p pictures, so common streams with three references are refused.
  *Offline:* grow the reservation to five slots and re-derive the memory record
  and IOMMU spans. Other refusals remain, such as more than two reorder frames.

### UART and PCIe

- **Bluetooth UART has no DMA or streaming receive (`buses-pcie-4`).** UART DMA
  accepts only UART2. The Bluetooth link on UART1 moves at most 64 bytes per
  1 ms tick, which caps receive near 64 KB/s at a 150 KB/s line rate.
  *Offline:* first loop the HCI step until receive returns short, or use the
  existing UART receive-timeout interrupt. A DMA receive ring, with a
  request-line entry for UART1 only, can follow. Determine whether RTS/CTS flow
  control is wired and enabled, because that decides between lost throughput and
  FIFO overrun.
- **PCIe fixed at Gen1 with configuration access only (`buses-pcie-5`).** BAR,
  memory window and MSI setup exist only in the endpoint test client.
  *Offline:* add a shared ATU region helper with per-region permission. Move BAR
  and MSI into the core when a second real consumer exists. Cite a primary
  source for Gen2 and Gen3 support before adding speed change. *Accessory:* a
  PCIe endpoint.

## 6. Concurrent workloads

The goal requires representative concurrent workloads but does not list them.
Four gaps prevent any such workload from running:

- devices cannot be linked into one image (`bringup-gaps-1`);
- every image stops at a fixed deadline (`bringup-gaps-2`, `supervisor-apps-9`);
- eight-core scheduling is unqualified (`smp-2`, `bringup-gaps-10`);
- NPU interrupt completion works only on core 0 (`npu-9`). DMA and UART
  completion are tick-polled (`dma-iommu-2`, `buses-pcie-4`), which adds latency
  under load.

Recommended first workload: mimus with I2S receive, Wi-Fi, USB0 evidence and USB
mass storage, built from the mimus board image. Write the workload set into the
[validation guide](../validation.md) so that qualification has a fixed target.

## 7. Live audio input

**I2S microphone.**

- Finite capture of 1 to 5 seconds is implemented and software-tested
  (`audio-2`).
- Cyclic receive is blocked until its completion and reuse ordering is
  established (handoff).
- No microphone is attached. Recorded 16 kHz audio and paced replay substitute
  for it, as the goal allows.

**Streaming recognition (`bringup-gaps-9`).** Capture and recognition run in
sequence: the I2S mode records a fixed prefix and then recognizes it. End-of-speech
latency therefore equals the full compute time. The pinned mimus
revision (`3e05158`) has no streaming API and no session prior; later mimus
revisions add them. Streaming is optimization work under section 8 of the goal.

- *Offline:* first move the mimus pin to a revision with the streaming front
  end. Carry over the SunstoneOS patches and re-establish reference parity and
  model admission. Then add a task that feeds each receive period, or each
  paced recorded period, into the stream.

**Bluetooth headset.** Four gaps stand between the headset path and repeated
physical use:

- slot reclamation (`bluetooth-5`);
- loss tolerance (`bluetooth-6`);
- disconnect handling (`bluetooth-7`);
- UART receive throughput (`buses-pcie-4`).

All four can be fixed and modeled offline; qualification needs a headset.

## 8. Native mimus and the 0.05x target

**What exists.**

- A single-core image, a two-worker multicore adapter and a board image.
- The only board timing: 19.48 s for 8.87 s of audio, a ratio of 2.2, on one
  A55 core at 494 MHz.
- The multicore adapter fixes `workers()` at 2, with the second worker pinned to
  core 1.
- Paced replay loads the model once per session and reuses it across segments.
  Its completion time includes formatting.

Reaching 0.05 from 2.2 needs about a 44-times speedup. The clock alone supplies
at most 3.6 times, so several levers must multiply:

- clock (`mimus-perf-1`);
- more workers, including the A76 cores;
- an exact int32 or SDOT inner kernel;
- removal of serial overheads.

The mimus performance area report covers the last three.

**Clock (`mimus-perf-1`, `clock-power-pins-5`).** See the CPU section above.
Clock reporting in native images comes first, so that every timing record
names its clock.

**No warm path across requests (`mimus-perf-5`).** The recorded path, the USB
service and the keyboard-repeat board loop reload the model on every request.
So do the Bluetooth and I2S callers in
[bluetooth/recognition.cc](../../apps/bluetooth/recognition.cc) and
[i2s/recognition.cc](../../apps/i2s/recognition.cc). Each reload re-checks the
model CRCs, recreates the recognizer and rebuilds the arena at a hard-coded
address. On the recorded path, `compute_us` covers only `Recognize` and leaves
out formatting. The goal requires warm PCM-to-formatted time with loading
reported separately.

- *Offline:* keep one loaded model and recognizer per asset generation in a
  shared session object. Drop it on any upload to asset slot 0 or 1, before the
  buffers are overwritten. Skip per-tensor CRCs for assets already verified at
  upload. Report a load time and a warm PCM-to-formatted time. Keep the arena
  address and size in one constant.
- *Check:* two consecutive recognitions in one QEMU session, with identical text
  and one reported load.

**GPU and NPU (`gpu-2`, `npu-7`, `npu-8`).** The goal asks for GPU and NPU
evaluation only after hardware functionality works. Neither accelerator can
take mimus work today. Start each with a measured feasibility estimate for one
linear layer.

**Thermal (`security-misc-1`).** Sustained multi-worker runs at a raised clock
need the thermal policy first.

## Dependencies between gaps

| Prerequisite | Unblocks |
| --- | --- |
| Physical recovery re-established by the user | Every board step below |
| E902 and SCP exclusion research; memory ownership of `0x42e00000` and `0x48000000` | Stage-2 loader admission (`cpu-startup-2`, `storage-6`, `bringup-gaps-3`) |
| Stage-2 loader | Resident contract producer (`bringup-gaps-2`), FreeRTOS packaging target (`build-system-2`) |
| Watchdog-expiry recovery trial | Armed resident contract with watchdog renewal |
| Card layout and procedure (`bringup-gaps-4`, `storage-7`), FEL tool rebuild (`build-system-9`), recovery established | Any card write; physical cold boot |
| Integrated image (`bringup-gaps-1`) and resident lifetime | Concurrent workloads |
| Eight-core scheduling qualified (`smp-2`, `bringup-gaps-10`) | Multi-worker mimus, concurrent workloads, resident OS |
| Thermal policy (`security-misc-1`), PMIC write path, rail-ledger change mode | CPU clock increase (`clock-power-pins-5`, `mimus-perf-1`) |
| Clock reporting and warm timing (`mimus-perf-5`) | Measuring the 0.05x metric |
| mimus re-pin | Streaming recognition (`bringup-gaps-9`) |
| Faster SDIO (`radio-net-6`) | Radio cold start inside the watchdog window; then `radio-net-9` and `radio-net-10` |
| Reset-line profile (`camera-3`) and color bars (`camera-2`) | First camera capture and its acceptance check; then `camera-1` and `camera-10` |
| Reproducibility fixes (`bringup-gaps-5`, `build-system-6`, `build-system-9`, `x-duplication-8`) | The reproducible-image requirement; no hardware needed |

## Recommended completion sequence

Hardware access is unavailable, so steps 1 to 3 are offline. Steps 4 to 6 wait
for the user to re-establish board recovery. Any board step that can reset or
hang the board ends access to it until further notice.

1. **Offline foundations that need no hardware.**
   - Reproducibility: `bringup-gaps-5`, `build-system-6`, `build-system-9`
     including the FEL tool recipe, `x-duplication-8`, and the packer lock entry
     from `build-system-2`.
   - Harness: `checks-harness-11`, then `checks-harness-10`.
   - Record corrections: the frequency-control row in the hardware matrix and
     the camera acceptance text.
   - mimus measurement: native clock reporting (`mimus-perf-1`) and the warm
     session with load and warm timings (`mimus-perf-5`).
   - Eight-core QEMU and modeled native scheduling phases (`smp-2`).
2. **Robustness fixes needed before the first physical runs.**
   - USB: `usb-6`.
   - Radio: four-bit SDIO at default speed (`radio-net-6`).
   - Bluetooth: `bluetooth-5` and the driver part of `bluetooth-7`.
   - Camera: `camera-3` and `camera-2`.
   - Build and documentation: the HEVC build guard (`video-8`) and the DSI
     domain precondition (`display-3`).
3. **Offline design and models for persistent operation and integration.**
   - Research and document E902 and SCP exclusion.
   - Model the stage-2 loader (`cpu-startup-2`, `storage-6`, `bringup-gaps-3`)
     together with the resident contract (`bringup-gaps-2`,
     `supervisor-apps-9`).
   - Write the card layout and the install, update and restore procedure, and
     add the planning tool (`bringup-gaps-4`, `storage-7`).
   - Build the integrated image from the mimus board image with a static
     interrupt table (`bringup-gaps-1`), adding the thermal policy
     (`security-misc-1`), the DMA interrupt (`dma-iommu-2`) and the NPU affinity
     guard (`npu-9`).
4. **First board steps, one bounded test at a time, run by the primary agent.**
   - Refresh board identity and admission.
   - Run the recorded full returning eight-core workload (`bringup-gaps-10`).
   - Read the native clock report.
   - Then device paths that need no accessory: the USB0 protocol, the thermal
     sensors and the radio identity read at four-bit width.
5. **Recovery, then persistent boot.**
   - Run the watchdog-expiry recovery trial.
   - Back up both cards.
   - Write the cold-entry package and observe its report.
   - Then boot through the stage-2 loader into the resident OS.
   - Qualify the integrated image and the representative concurrent workload.
6. **Performance, with user approval for each clock or voltage change.**
   - Qualify the thermal policy.
   - Apply a fixed operating point (`clock-power-pins-5`).
   - Add more mimus workers including the A76 cores, and the kernel levers.
   - Re-pin mimus for streaming (`bringup-gaps-9`).
   - Then evaluate the GPU and NPU (`gpu-2`, `npu-7`, `npu-8`).
7. **Accessory-gated qualification, as each accessory arrives.**
   - OV5640 module: `camera-1`, `camera-10`.
   - Microphone: `audio-2` and cyclic receive.
   - Headset: `bluetooth-6`.
   - USB device: `usb-8`.
   - DSI panel: `display-3`.
   - PCIe endpoint: `buses-pcie-5`.
   - Access point: `radio-net-9`.
   - Test streams: `video-9`, `video-10`.
