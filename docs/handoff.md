# Current work

The active objective is [goal.md](goal.md). FreeRTOS now runs on the physical
A733 boot CPU. The kernel check passes timer preemption, blocking queues, and
floating-point context checks. An EL3 secure-timer timeout recovers a forced
EL1 stall and returns to FEL. [Physical kernel evidence](a733-kernel.md) records
the image, resources, measurements, and limitations.
The [bring-up sequence](bringup-sequence.md) orders recovery, shared platform
services, complete device functionality, qualification, and performance work.
Parallel workers now use separate worktrees and build directories. One coordinator
owns physical board tests through the [submission queue](board-test-queue.md)
and the existing exclusive board lock. The shared queue is initialized; the
physical FEL-input check and the separate [watchdog enable/disarm and expiry
checks](watchdog-expiry.md) are pending on confirmed pad connection and current
target/access conditions. No new physical request has run.

A native USB channel now passes status, packet-boundary echo, log, and commanded
ROM return checks. [USB evidence](usb.md) records the implementation and limits.
The [C++ runtime](runtime.md) passes a physical check for allocation, containers,
constructors, and task-local newlib state.
Mimus now matches the host on 32 development recordings in one physical boot.
The [native USB application](mimus-usb.md) loads assets, accepts repeated
recognition requests, and returns results while FreeRTOS runs. Its host lease
returned the board to ROM with automatic access after the client stopped requests.
[Application evidence](mimus.md) records arithmetic and performance limits.
[Paced replay](mimus-replay.md) now passes a four-utterance FreeRTOS emulator
comparison, including capture during recognition and formatting across utterances.
The native service command compiles but has not run on the board.
The separate [SMP emulator target](smp.md) passes three bounded two-core trials
with queues, mutexes, preemption, affinity, and floating-point register checks.
A separate session passes 64 task migrations with general/vector registers,
condition flags, stack position, and floating-point control/status preserved.
The [SMP runtime](smp-runtime.md) now passes concurrent C++ allocation, shared
memory-stream locking, and newlib task-state preservation through 256 migrations.
[Multicore mimus](mimus-smp.md) now matches the host for one full recording and
four paced utterances in QEMU, including packed matrix rows on both cores.
A focused matrix check also passes serial parity and guards across 12 shapes.
The [secondary CPU power controller](cpu-start.md) matches isolated board-firmware
writes for all seven secondary cores and passes retained-failure checks and
AArch64 compilation. Reuse `cpu-start-20261016`. The [secondary entry](cpu-entry.md)
now reserves per-core mailboxes/stacks and waits for matching arrival identity
and verified power cleanup before release. Host composition and instruction
emulation pass; reuse `cpu-entry-20261016`. The native adapter requires an EL3,
cache-disabled boot owner. Continuation, coherence, per-core interrupts and
physical multicore qualification remain outstanding.
The native [DMA descriptor encoder](dma.md) now passes a host execution model
for memory/FIFO chains, 35-bit addresses, guards, and invalid-input refusal.
The polled controller core now passes a noncoherent host model for concurrent
leases, completion after bus drain, bounded cancellation, and retained buffers
after faults. Both components compile for AArch64. Board integration and physical
DMA remain unimplemented or untested; no hardware access occurred in these checks.
The [native DMA memory adapter](dma-memory.md) now passes a FreeRTOS emulator
check for translations, permissions, cache instructions, and two serialized EL1
callers. Its RAM/FIFO admission policy also passes host checks. Board reservations
and physical startup qualification remain outstanding. The [privileged DMA service](dma-service.md)
now passes one two-core FreeRTOS session with copied metadata, task ownership,
concurrent channels, retained results, and I2S shutdown ordering. Its root build
target links successfully. Reuse `dma-service-20261016`; integration identities
are in `hardware-integration-20261016b`. No emulator session was repeated.
The [DMA0 platform controller](dma-platform.md) now verifies inherited fabric
clocks, enables dedicated gates/reset release when admitted, and configures
CPU/MCU routing under retained whole-controller ownership. Host/reference checks
and native compilation pass; reuse `dma-platform-20261016`.
The [startup adapter](dma-startup.md) now connects the common clock registry,
register mutex, one physical-counter history and deferred service activation.
Focused two-core success, retained-failure and health/refusal cases pass.
Reuse `dma-startup-20261016`; the initial image hash was not preserved, while
source snapshots/logs and the final focused image are recorded. The integrated
target links; source and generated-port identities match. No emulator case was
repeated for integration. Physical controller handoff remains unqualified.
Native [I2S0 capture](i2s.md) now configures finite mono/stereo reception through
the DMA controller. Host checks pass shutdown ordering, deadlines, and retained
faults; format plans match pinned audio functions. Reuse the unchanged format
comparison from `i2s-20261016`; the affected capture fixture passes with the
new platform API. Allocation, continuous streaming, and physical audio remain outstanding. No
microphone is attached. The [I2S0 platform binding](i2s-platform.md) now supplies
bounded clock/reset/pin acquisition and release, with retained partial ownership.
Host checks and pinned CCU comparison pass; core, platform, and native adapter
compile for AArch64. Reuse `i2s-platform-20261016`, including the focused parent
clock regression check. Its 14 final input hashes match the integrated files.
The [common clock registry](clock-leases.md) now binds audio ownership and the
existing GPU resource manager. Host composition checks and AArch64 compilation
pass; reuse `clock-leases-20261016`. Production startup must create one registry,
complete GPU parent admission, and provide the audio binding before consumers run.
Shared [header pin ownership](pins.md) now covers PB4–PB8 with retained tokens,
verified register updates, and host/reference evidence. Reuse `pins-20261016`
and the focused late-operation checks in `pins-deadline-20261016`. The I2S
binding uses this manager after acquiring local controller ownership.
The [NPU investigation](npu.md) identifies available driver source and the
remaining model-loader/compiler dependencies. A host inspector now matches
compiled-model metadata and rejects malformed containers. Native [NPU page-table
construction](npu-mmu.md) now passes a host model for translations, permissions,
all physical-address extensions, guards, and atomic refusal. It compiles for
AArch64. The [NPU controller core](npu-controller.md) now passes host startup,
completion, cancellation, and retained-fault checks and compiles for AArch64.
The [NPU memory policy](npu-memory.md) now passes a host integration check for
private buffer mappings, canonical table validation, noncoherent publication,
and retained faults. It compiles for AArch64. The [native NPU adapter](npu-native.md)
now passes a FreeRTOS emulator check for CPU translation/cache operations, MMIO,
deadlines, and retained failure. DMA passes its focused emulator check with the
shared CPU operations. The [NPU FreeRTOS service](npu-service.md) now passes a
two-core check with EL0 clients, a preemptible EL1 worker, full register retention
through worker migration, task ownership, timed replies, and autonomous
completion/cancellation/deadlines. The [address relocation component](nbg-relocation.md)
passes host bounds and atomic-refusal checks and comparison with isolated SDK
pool/SRAM patch instructions. The [complete loader trace](nbg-loader-trace.md)
now matches all active relocations and full SDK command buffers at two allocation
bases, using the native encoder's command-pointer mode. It records deferred
tensor binding, allocation padding, and the completion-event difference that
native assembly must handle. The encoder compiles for AArch64.
The [native container decoder](nbg-model.md) now passes malformed-input and
borrowed-view checks, compiles for AArch64, and supplies C records that reproduce
the saved SDK submission through the host reconstruction. The
[native assembler](nbg-assembly.md) now plans allocations and constructs that
neural-operation profile entirely in C. It passes atomic-refusal checks and a
comparison with saved SDK bytes at page-aligned addresses. The command image
uses the native completion event and reserves the complete fetch span.
The [checksum verifier](nbg-checksum.md) now matches two published models and
the isolated SDK CRC routine. Native assembly requires a matching complete-file
checksum before publishing requirements or modifying buffers.
The assembler now checks [complete encoded rings](nbg-rings.md), including
tensor membership and exclusive ends at allocation boundaries. It still matches
the saved SDK submission. Linear accesses, weight prefetch, and actual hardware
access behavior remain unresolved.
The [pinned host compiler](npu-compiler.md) now produces a small convolution with
matched float and quantized simulator references. Its emitted `0x20000` format
now has native decoding, checksum verification, and assembly support. Physical
execution of this fixture remains outstanding.
The [directory investigation](nbg-directory.md) now identifies the changed tensor
and patch layouts and matches the payload and section checksums with the isolated
compiler CRC routine. The payload checksum excludes the header and directory;
native structural validation remains essential. The network-information operation
count differs from the section's record count and needs interpretation.
The decoder uses the checked section count and retains raw network metadata.
The [native decoding evidence](nbg-model.md) covers the numerical fixture, malformed
directories, and continued agreement with the older saved SDK submission.
The [checksum extension](nbg-checksum.md) now verifies the normalized primary CRC
and every listed section CRC. A [convolution SDK trace](nbg-conv-loader.md) reaches
intercepted submission with five allocations and no working pool. It records a
136-byte command image, a different operation state word, and a 192-byte output
allocation. Native assembly now matches all five saved buffers after address and
completion-event adjustments. Empty pools require a zero buffer slot. One focused
directory sanitizer check passes fourteen atomic refusals; the existing 120-case
assembly check and older SDK comparison also pass. The AArch64 assembler builds.
Reuse this evidence under `~/.cache/sunstoneos/evidence/nbg-directory-assembly-20261016/`.
Descriptor access bounds and command admission remain outstanding.
The [GPU firmware component](gpu.md) now decodes and stages the published
MIPS image for the board software's target BVNC, 36.56.104.183. Its two
allocations match an independent ELF reader. One sanitizer check passes
malformed-image and buffer refusal cases; the component compiles for AArch64.
The [MIPS preparation component](gpu-mips.md) now constructs physical mappings,
boot data, and wrapper register values with host reference agreement.
The [firmware queue transport](gpu-queues.md) passes host checks for publication
ordering, completion-slot ownership, events, deadlines, and retained failures.
Native [initialization records](gpu-fw-state.md) connect nineteen firmware
resources to the queues. Full allocations match the reference initializers;
one host check integrates their mappings with firmware staging and queue formatting.
Native [GPU workload tables](gpu-vm.md) now prepare all six page sizes and a
firmware memory-context record. Complete bytes match pinned entry encoders;
one sanitizer check passes translation boundaries and 34 atomic refusals.
Native [workload table publication](gpu-vm-memory.md) now passes one FreeRTOS
emulator session using CPU translation/cache instructions, per-page publication
and reclamation, and retained mapping failure. Reuse `gpu-vm-memory-20261016`.
Device context attachment and physical cache visibility remain unqualified.
The [startup controller](gpu-start.md) now publishes firmware resources, performs
ordered register setup through callbacks, and waits for readiness with an absolute
deadline. Complete traces match the original startup functions. Host checks cover
minimum delays, 42 retained-failure cases, and 40 atomic input refusals.
The [firmware control worker](gpu-control.md) now checks published compatibility,
requires one successful health command, and services bounded interrupt/event
batches. Its queue integration passes 78 retained-failure cases. ABI and interrupt
operations match pinned references; the health-command reference was reused.
[Bounded shutdown](gpu-stop.md) now sends two power requests, waits for separate
firmware acknowledgements, retires control/queue I/O, and performs idle/reset
checks before requiring platform isolation. Host integration passes 115 retained
faults, and the affected control regression passes. Commands and register plans
match the pinned functions. The isolation callback remains unimplemented on the board.
The [FreeRTOS lifecycle service](gpu-service.md) integrates preparation, startup,
control, and shutdown under one privileged worker. Its two-core emulator cases
pass session ownership, reopen, event service, and retained failures. Physical
backend operations and resource-specific event handlers remain outstanding.
The [native memory adapter](gpu-memory.md) now connects shared firmware records
to uncached CPU mappings and publishes/reclaims cached firmware backing. Two
emulator cases pass lifecycle integration and retained mapping failure. Board
allocation, physical cache behavior, and isolation remain unqualified.
The [power controller](gpu-power.md) now has host/reference evidence for bounded
GPU clock/reset and power-domain transitions. The [resource manager](gpu-resources.md)
now has host and AArch64 adapter evidence for lease ownership, inherited settings,
and retained I2C restoration failures. The service's asynchronous power interface
passes power-controller integration, timeout, and partial-acquisition failures
in QEMU. The [native platform binding](gpu-platform.md) now connects resource
admission to power transitions and shared task mutexes. Instruction-model and
two-core FreeRTOS checks pass; reuse `gpu-platform-20261016`. Cold initialization
and physical operations remain outstanding. The [native lifecycle backend](gpu-backend.md)
now binds MMIO, identity admission, memory operations, power, and GIC callbacks.
Instruction-model checks pass register composition, retained faults, shared-clock
ordering, and actual firmware-event reception. Reuse `gpu-backend-20261016`;
55 input hashes match and the source-lock extension preserves all prior pins.
Physical memory isolation, reservations, interrupt setup, and firmware boot remain
required. The backend's external isolation provider is still unimplemented.
[Interrupt delivery](gpu-interrupts.md)
now wakes the privileged worker through a bounded GIC mask/notification/rearm
path. Two QEMU cases pass cross-core delivery, backlog handling, reopen, and
retained failures. One native-memory lifecycle case also passes with the updated
service's interrupt callbacks disabled. Physical GPU assertion remains untested.
The [translation-cache invalidation component](gpu-flush.md) now has host
integration evidence for table publication, command completion, queue pressure,
and retained failure. Its complete commands match the pinned driver. Live table
edits, service integration, and physical invalidation remain outstanding.
[Firmware event dispatch](gpu-events.md) now decodes all delegated event types
and resolves every requested resource before calling a handler. Host ABI and
control integration checks pass. Native [free-list management](gpu-freelist.md)
now constructs backing tables and handles growth/reconstruction through deferred
responses. Host/reference checks pass ownership, partial allocation refusal,
response backpressure, and retained failures. Concrete allocation/publication,
linked render-target recovery, and physical operation remain open. Optional
[service integration](gpu-freelist-service.md) now passes bounded construction,
deferred event responses, shutdown draining, and retained faults in QEMU. Reuse
`gpu-freelist-service-20261016`. Enabled lists retain backing after stop and
temporarily refuse reopen until verified lease disposal is implemented.
Firmware boot, graphics/compute execution, and physical GPU qualification remain
outstanding. Reuse the evidence under `~/.cache/sunstoneos/evidence/gpu-firmware-20261016/`.
The [client command transport](gpu-cccb.md) now writes bounded sequences,
handles ring wrap and queue pressure, and waits for firmware notification
acceptance. Host/reference checks compare full ring and KICK bytes and exercise
retained failures; the component compiles for AArch64. Reuse `gpu-cccb-20261016`.
The [offline compute-context builder](gpu-compute-context.md) now matches complete
reference bytes and integrates with native VM and client-queue formatting.
Reuse `gpu-compute-context-20261016` and the focused root-exclusion result and
final AArch64 object in `gpu-compute-context-root-20261016`.
The [context program producer](gpu-compute-programs.md) now constructs real
USC/PDS programs from pinned Mesa compiler output. Complete allocations and
register images match the reference; focused refusal checks and native compilation
pass. Reuse `gpu-compute-programs-20261016`. The [fixed numerical job](gpu-compute-job.md)
now constructs a real compiled shader and command stream. Completion requires
a firmware-written timeline value and correct output with intact guards.
Source-byte comparison, host lifecycle checks and native compilation pass;
reuse `gpu-compute-job-20261016`, including its focused publication-order check.
Single-core admission still requires platform proof. Multicore addressing,
service integration and physical execution remain outstanding.
The [DSI command transport](dsi.md) now encodes and transmits bounded low-power
writes under a combined host/PHY lease. Packet/register comparisons, retained
failure checks and native compilation pass; reuse `dsi-command-20261016`.
PHY initialization, platform authority, scanout, panel configuration and physical
qualification remain outstanding.
The [radio investigation](radio.md) identifies a source-backed AIC8800D80
candidate, SDIO bootstrap before Bluetooth UART operation, board wiring,
firmware availability, and stack dependencies. Installed silicon identity,
reset/LPO wiring, firmware redistribution terms, and voice routing remain
unconfirmed. The read-only patch-table inspector passes focused published-file
and malformed-input checks; reuse `radio-20261016`. The native
[identity-message codec](radio-codec.md) now encodes source-matched read requests
and validates response bounds while preserving unrelated messages. Its host
sanitizer check and AArch64 compilation pass; reuse `radio-codec-20261016`.
The [SDIO controller and card worker](sdio.md) now implement bounded preparation,
enumeration, CMD52 and whole-word CMD53 transfers. Host checks and final AArch64
compilation pass; reuse `sdio-controller-20261016/final-inputs.json` and its
focused follow-up cases. The [bootstrap transport](radio-transport.md) now supplies
function setup, fixed setup write/confirmation, conditional pad restoration and
both identity reads. Reuse `radio-transport-20261016` and the focused integration
and final native objects in `radio-bootstrap-20261016`. The [firmware loader](radio-firmware.md)
now verifies pinned blobs, uploads Bluetooth patches and Wi-Fi firmware, and
waits for start confirmation. Reuse the complete upload model and focused UART
mode/alias checks in `radio-firmware-20261016`; final native objects compile.
The [runtime worker](radio-runtime.md) now supplies the binding exchange,
continuous receive and bounded raw-record ownership. Source/vector comparisons,
host composition and native compilation pass; reuse `radio-runtime-20261016`,
including focused invalid callback checks. Platform clock/pin/power bindings,
normal Wi-Fi traffic and Bluetooth HCI remain required. No radio board request exists.
The [OV5640 driver](camera.md) now supplies bounded sensor control and one
two-lane VGA mode. Complete modeled sensor state matches the pinned Linux
reference; lifecycle checks and native compilation pass. Reuse `ov5640-20261016`.
The camera register-map table has a prose-only PHY/packet-port label correction.
The [CSI receiver](csi-receiver.md) now configures local PHY, packet-port and
parser state. Host lifecycle checks and complete source-register comparisons
pass for both ports; reuse `csi-receiver-20261016`. The [camera DMA controller](camera-dma.md)
now configures a single NV16 image and retains buffers through producer stop,
DMA disable, independent drain and cache synchronization. Source comparisons,
host ownership checks and native compilation pass; reuse `camera-dma-20261016`,
including its focused capture-complete correction. Global PHY calibration,
parser single-image mode, ISP/VIPP routing, actual mapping/drain providers and
physical camera qualification remain outstanding.
Integrated component identities and source-pin reuse are recorded in
`hardware-integration-20261016c`, `hardware-integration-20261016d` and
`hardware-integration-20261016e`. No unchanged component check was repeated.
The shared A733 User Manual V1.00 is now pinned in `sources.lock.json` and
cached under `sources/camera-research/`. Its PERI0 input-divider definition
requires an additional admission guard in DMA and GPU clock checks. Focused
sanitizer checks and native compilation pass; reuse `peri0-input-divider-20261016`.
The affected clock checks now refuse divided input before register writes.
Earlier evidence remains applicable to unchanged platform behavior.
Complete NPU model admission and physical execution remain outstanding.
The [NPU platform controller](npu-platform.md)
now supplies bounded local clock/reset/power sequencing under retained resource
ownership. Source/model checks and native compilation pass; reuse
`npu-platform-20261016`. Its nine final input identities match the integrated
files, as recorded in `hardware-integration-20261016f`.
Actual DCDC2/PCK binding, shared SRAM reservations, cold PLL provisioning and
deferred service activation remain required.
Persistent boot, application qualification, other CPU cores, and
complete hardware qualification remain outstanding. No complete hardware
subsystem is qualified. Development uses branch `bringup` in the isolated
`/home/sunil/git/sunstoneos-worktrees/bringup` worktree.

The [storage probe](storage.md) reaches the SD/MMC controller, but eMMC CMD1
times out after applying the reference clock-mode setting. Population and power
remain unconfirmed. Pin/clock restoration and automatic ROM return passed.
The [PMIC reader](power.md) identifies AXP8191 and reads enabled UFS rail settings.
UFS initialization then failed to return to ROM. The [watchdog driver](watchdog.md)
passes host checks and a physical SRAM configuration/restoration trial.
Independent hardware reset remains untested.
The captured board ROM and an instruction-model check identify a direct FEL
redirect and a boot-input path before storage boot. The watchdog's RTC flag is
handled by boot0 and does not establish storage-independent recovery. Register
retention across reset and physical assertion of the boot input remain unknown.
The [manufacturer's schematic and manual](board-wiring.md) now identify BOOT
and GND pads beside mini-HDMI. They provide a documented physical FEL input;
the attached board's layout and a stable connection still need confirmation.
Qualify it before further new peripheral-access sequences.

## Observed host and device state

The attached FEL device (`1f3a:efe8`) identifies A733. The boot CPU identifies
as Cortex-A55 r2p0. DDR firmware reports 12 GiB LPDDR5, with transfers checked
at two distinct pages. [Boot evidence](boot.md) records initialization and the
EL1 handoff. Board revision and populated storage remain unconfirmed.
No persistent-storage writes have been issued.

The reconnected board at `003:064` has automatic access and answers ROM requests.
The [physical ROM capture](watchdog.md) differs from the published dump and now
supplies the board-specific reset reference. SRAM workspace restoration passed.
Read-only recovery-register inspection found no redirect owner and a disabled
watchdog. The boot input was high, so it cannot currently protect a reset from
storage boot. The user confirmed automatic FEL entry after disconnecting and
reattaching USB, with a microSD card inserted and no button held. Preserve that
card. No watchdog was armed and no reset was issued.
The failed UFS instance `003:063` remains quarantined. DDR must be reinitialized;
do not reuse previous RAM assets or signatures.
The [SRAM configuration trial](watchdog.md) now verifies ROM-redirect and
watchdog reset-policy writes with complete register/workspace restoration.
It never enables the timer. Timer enabling, expiry, and reset retention remain
unqualified. No new peripheral probe may run before independent watchdog recovery passes.
The installed
[session access rule](../tools/README.md) grants access after reconnects on
physical port `3-1`. Kernel and native USB checks regained access automatically.
The rule lasts until host reboot and can be removed with `disable-session`.
Rediscover the node and run `python3 tools/usb-access.py check-session` before
an intentional reconnect.

The separate `mimus keyboard` device is an existing ESP32 board. It must not be
used as the Orange Pi console. No Orange Pi UART adapter is currently visible.

The host has Clang 21, CMake, and LLVM object tools. LLD 21, ARM system emulation,
and USB/FDT development headers are extracted under `~/.cache/sunstoneos/host`.
Pinned sources, builds, packages, and raw evidence are under `~/.cache/sunstoneos`.
The [build instructions](build.md) describe the emulator configuration.
The local Python environment supplies the dependencies in
`checks/host-requirements.txt` for USB transport and compiled-code checks.

## Reusable validation evidence

The DDR wrapper passed its simulated control-flow check and physical
initialization. The explicit compatibility guards produced identical payloads
with Python `-O`. DDR and the EL1 handoff have not changed since those checks.

The secure-monitor kernel model passed normal execution and a forced EL1 stall.
The physical image then passed both cases without rebuilding between them.
The normal case completed 100 exchanges in 100 ticks. The forced stall returned
through secure timer interrupt 29 after approximately two seconds. The monitor
restored the modified GIC settings before returning to ROM.

The USB session helper passed its focused ACL lifecycle check and rule validation.
Automatic access after physical re-enumeration passed during the EL1 handoff,
kernel cases, and PMIC probe. Focused host checks cover expected-disconnect
metadata errors and refusal of the instance that failed to recover.

The original nonsecure QEMU platform remains available. Its earlier check passed
100 exchanges in 100 ticks. The added secure-monitor configuration is selected
by a separate build definition; the original configuration remains in the
other preprocessor branch. The physical and secure-model results cover the
new startup and configuration.

The USB image passed one bounded physical session after corrections to control
completion and zero-length IN submission. The monitor renewal model and focused
compiled-driver check also passed. Their identities and limits are in the USB
document. The runtime renews the secure deadline while its USB task runs.
Unbounded mode 3 remains untested. The native mimus application uses mode 4
with a tested host lease; its evidence is in the application transport document.

The C++ runtime passed its secure-monitor emulator check and one physical
session. The physical session exercised two allocating tasks for 100 rounds
each and returned to ROM automatically. [Runtime evidence](runtime.md) records
source identities, library dependencies, memory use, and limits.

The isolated mimus port is at the revision in `apps/mimus/source.json`. Its
physical bench run matched host CRCs, words, punctuation, capitals, and formatted
text. Profiling found linear matrix products used 83% of portable recognition
time. AArch64 NEON integer kernels now reduce the bench run from 39.09 to 19.48
seconds with the same host parity. This is a single-run performance screen;
recording parity now covers 32 fixtures through native USB. Performance
qualification and the required latency target remain outstanding.
The configured boot CPU clock is 494 MHz;
independent frequency measurement remains outstanding. Arena and allocator peaks
are recorded in the application document. Compute time remains above target.
The FEL receive tool now bounds reads;
a diagnosed oversized receive was completed without resetting the board. Use
the tool hash and recovery evidence in the application document.

## Prepared components awaiting integration review

The following isolated commits have preserved component evidence. Review their
runtime contracts before integration; a local commit does not establish physical
qualification. The [local progress dashboard](progress-ui.md) is implemented and reads these
canonical records automatically.

| Component | Worktree | Commit | Evidence directory under `~/.cache/sunstoneos/evidence/` |
| --- | --- | --- | --- |
| Secondary CPU continuation | `cpu-entry` | `c0d8c7e1877b9a87e1bece11c7c37ad2673a3dc6` | `cpu-continue-vectors-20261016/` |
| Eight-core diagnostic image | `a733-smp` | `f9a1a93` | `a733-smp-image-20261016/` |
| System IOMMU | `iommu` | `e864a59c81df4dfc62ac36c4e78424755c7a95b0` | `iommu-20261016/` |
| NPU startup and deferred activation | `npu-startup` | `02f4fb2` | `npu-startup-20261016/` |
| Wi-Fi initialization and passive scan | `radio-sdio` | `671c03cd9f817626bd6d685855e4273fd69990a8` | `radio-wifi-20261016/` |
| DSI video transmission | `display` | `94f76d80948dbd8062d6f0b999728a80617ef63a` | `dsi-video-20261016/` |
| Shared camera analog startup | `camera` | `c701156` | `camera-platform-20261016/` |
| GPU core-mode observation | `gpu-compute-mode` | `6899ede85c0c578b91223966bc938094f0b298cb` | `gpu-compute-mode-20261016/` |

The diagnostic image depends on the continuation. Native memory admission and
independent recovery remain prerequisites for physical execution. Inspect live worker state before assigning follow-up changes.

## Next actions

1. Establish a reset destination that cannot
   execute unknown storage code, then qualify the prepared hardware watchdog
   with an SRAM expiry test. The [physical recovery procedure](board-wiring.md)
   identifies BOOT/GND and the prepared input checker. A question about the
   user's ability to attach a stable, removable bridge is pending. No board
   reset or new hardware transaction was issued while preparing these records.
   Reuse the passed SRAM configuration/restoration
   evidence. The ROM redirect candidate and its unresolved
   retention requirements are in [watchdog.md](watchdog.md).
   The [protected watchdog diagnostic](watchdog-expiry.md) is implemented with
   instruction-model and host-protocol evidence. Its enable/disarm and expiry
   requests are pending in the board queue. Protected reset observation alone
   does not establish reset cause or bus-stall recovery. Reuse the existing
   `watchdog-expiry-20261016` evidence. Integrate recovery before further
   peripheral access.
   Resume UFS with the corrected capability-read ordering only after recovery
   protection passes. Implement read-only storage access and backup through the
   native transport; physical storage population is still unknown.
   Establish storage boot and recovery before overwriting existing contents.
2. Extend USB with interrupt delivery, cold clock/PHY setup, and remaining roles.
   Preserve independent task and host deadlines during controller work.
3. Physically validate paced replay after recovery qualification. Add worker cores
   with change-dependent parity checks. Keep accelerator and remaining hardware
   work active independently of the recorded-audio milestone.
4. Extend CPU and memory support to secondary cores, usable capacity, protection,
   and coherence, with checks selected from each change. The SMP emulator target
   now passes migration and register-state checks. The separate C/C++ runtime
   passes concurrent library checks. Mimus now passes two-core reference parity;
   qualify all physical workers. Task-deletion
   cleanup, interrupt nesting, and physical startup remain outstanding. The
   checked [power controller](cpu-start.md) now composes with reserved
   [secondary entry/mailboxes](cpu-entry.md). Supply the platform continuation,
   coherence and per-core interrupts before scheduler startup.
   Current evidence is in [smp.md](smp.md), [smp-runtime.md](smp-runtime.md),
   and [mimus-smp.md](mimus-smp.md).
5. Integrate the checked DMA controller core and native memory adapter with the
   platform. Reserve dedicated RAM, register verified FIFO routes, and supply
   clock/reset setup to the integrated privileged service. Bounded platform
   preparation and its binding to shared clocks, the register mutex, a common
   physical-counter clock and deferred service activation are integrated.
   Supply actual boot-authority handoff evidence and native memory reservations.
   Qualify completion
   and failed-stop
   recovery physically after watchdog protection passes. Resolve DMA1 resources
   and wider descriptor addressing. Reference facts, ownership behavior, and
   reusable host evidence are in [dma.md](dma.md).

6. Complete native NPU model admission and controlled command registration beyond
   the service's fixed startup catalogue. The [native assembler](nbg-assembly.md)
   resolves the observed profile's sources and destinations from checked container
   views and adapts the completion event and fetch reservation. Determine complete
   descriptor access lengths, qualify SRAM routing,
   and establish descriptor semantics and cold initialization equivalence.
   The [descriptor research](npu.md#descriptor-field-compatibility-is-a-research-result)
   maps all observed relocation destinations to candidate Mesa fields but exposes
   stride differences and an additional descriptor word. Ring geometry is now
   checked against source allocations. The [simulator stride investigation](nbg-strides.md)
   now identifies the target's revision-9.2 decoder and direct channel-stride
   packing. Its bounded instruction check passes with explicit synthetic-state
   limits; full access paths remain unresolved. Resolve instruction-family semantics,
   stride units, linear accesses, and prefetch before device admission.
   Integrate board
   power/clock setup and physical interrupt handling after recovery qualification.
   Use the [compiled numerical fixture](npu-compiler.md) for the first controlled
   numerical execution after admission and platform setup. Native decoding,
   checksum verification, and assembly now have host evidence. The
   [saved SDK convolution trace](nbg-conv-loader.md) matches native buffers;
   preserve its hashes and do not repeat SDK preparation. Extend support for
   additional operation classes and address widths as their semantics are resolved.
   The [NPU record](npu.md) identifies references, inspection evidence, and the
   implementation sequence.

7. Integrate the GPU lifecycle service's board backend with reserved allocations,
   the [native memory adapter](gpu-memory.md), [power controller](gpu-power.md),
   shared-resource manager, resource-specific firmware event handlers, and
   [interrupt delivery](gpu-interrupts.md). The [FreeRTOS service](gpu-service.md)
   now integrates resource preparation, startup, control, and shutdown under one
   privileged worker. Its two-core emulator check passes ownership, migration,
   reopen, event service, and retained startup/control/shutdown failures. Reuse
   `gpu-service-20261016`; no GPU firmware instructions or physical device
   accesses occurred. Physical shader execution and GPU job completion remain
   outstanding. Physical backend work requires independent recovery first.
   Reuse `gpu-memory-20261016` for native translation, shared-memory access,
   publication/reclamation, and permission-change failure. Reserve uncached
   firmware resource mappings without cacheable aliases on the board; qualify
   physical cache/interconnect behavior after recovery. Reuse `gpu-power-20261016`
   and `gpu-irq-20261016/failed-enable-disable` for the new power and interrupt
   components. The source-locked power setting is 400 MHz at 800 mV; it is not a
   frequency measurement or performance target. Power-off does not establish
   memory isolation. Reuse `gpu-resources-20261016` for shared leases and the
   native register/I2C adapter, and `gpu-power-service-20261016` for asynchronous
   service sequencing. Native platform binding and shared FreeRTOS locks are
   integrated; reuse `gpu-platform-20261016`. The native lifecycle backend is
   integrated with instruction-model evidence in `gpu-backend-20261016`.
   Real USC/PDS context programs, offline context construction and a fixed
   numerical shader with command stream and completion fence are integrated.
   Host/reference checks and native compilation pass. The [compute service](gpu-compute-service.md)
   integrates native memory publication, firmware records and owner-only jobs.
   Reuse `gpu-compute-service-20261016`; central compute/service/IRQ targets link.
   The GPU worker is resolving core-mode admission and multicore addressing.
   Repeated jobs, disposal and physical completion remain required.
   The client command transport is integrated; reuse `gpu-cccb-20261016`.
   Free-list backing, growth/reconstruction, and
   deferred responses are integrated; reuse `gpu-freelist-20261016` evidence.
   Lifecycle-service integration is complete with emulator evidence in
   `gpu-freelist-service-20261016`. The radio worker has completed dependency
   inspection, identity-message encoding/decoding, SDIO controller operations,
   card enumeration, bounded bootstrap transport and verified firmware loading.
   Binding response and continuous receive are integrated with host evidence.
   It is implementing normal Wi-Fi firmware initialization and scan control.
   The camera worker has completed OV5640, local CSI control and dedicated
   frame-DMA ownership. [Pass-through routing](camera-route.md) and single-image
   parser mode are integrated; reuse `camera-route-20261016`. It is implementing
   global analog startup. The [DSI PHY controller](dsi-phy.md) is integrated with
   inherited-clock admission; reuse `dsi-phy-20261016`. Its worker is implementing
   video transmission. Separate workers have prepared the secondary CPU
   continuation, eight-core diagnostic image and system IOMMU for review.
   Command transport and entry/mailbox components are integrated. Panel selection remains independent of DSI controller support.
   The [reset-evidence investigation](watchdog-reset-evidence.md) found no
   source-defined retained cause contract; ROM progress words are software
   records. Finite I2S0 capture and shared PB4–PB8 pin ownership are integrated;
   their clock/reset/pin binding is integrated with `i2s-platform-20261016`
   evidence. The privileged DMA service and shared GPU/audio clock registry are
   integrated, including deferred DMA activation. Physical reservations and
   handoff qualification remain required. The platform worker is now resolving
   NPU resource binding and deferred service activation. Bounded local
   power/clock/reset preparation is integrated; the native compiler remains deferred.
   Inspect live agent state before reassigning their files.
   Root remains the sole physical board coordinator.
   Native [shutdown](gpu-stop.md) now has host/reference evidence; reuse
   `gpu-stop-20261016`. Its platform callback must block new GPU memory traffic
   and drain outstanding transactions before resources can be reused. Implement
   that callback with shared platform power/reset services and qualify it after
   independent watchdog recovery. The [control worker](gpu-control.md)
   now gates queue activation on compatibility and a health-command result,
   bounds event batches, and aborts queue I/O after faults; reuse
   `gpu-control-20261016`. Actual compatibility publication and health completion
   remain untested. Native [startup](gpu-start.md)
   now has host/reference evidence for the MIPS register sequence, minimum
   delays, readiness, and retained failures; reuse `gpu-start-20261016`.
   Physical startup remains blocked on independent watchdog recovery.
   Native [GPU workload tables](gpu-vm.md) now have host and reference evidence
   for six page sizes; reuse `gpu-vm-20261016`. The bounded
   [invalidation component](gpu-flush.md) now has host integration and reference
   evidence in `gpu-flush-20261016`. Native workload-table publication and
   reclamation have evidence in `gpu-vm-memory-20261016`. Allocation, live mapping
   changes, invalidation integration, and device attachment remain outstanding.
   Native [initialization records](gpu-fw-state.md) now connect queues and firmware
   resources; reuse `gpu-fw-state-20261016` evidence. The pinned driver's
   `pvr_fw_startstop.c` describes startup sequencing; `pvr_mmu.c` describes
   translation-cache invalidation. The MIPS startup path skips GPU BIF setup;
   workload page tables are separate from firmware translation.
   Connect the service's [checked transport](gpu-queues.md) to physical memory
   callbacks, event handlers, and verified shutdown/recovery. GPU job completion
   remains separate from command acceptance. Reuse the `gpu-ccb-20261016` evidence.
   Native [MIPS preparation](gpu-mips.md)
   now constructs the firmware page table, boot configuration, and wrapper
   register plan. Complete outputs match the pinned reference functions;
   reuse the `gpu-mips-20261016` evidence. Integrate allocation, cache publication,
   remaining device features, and platform power. Confirm physical identity
   and boot firmware after watchdog recovery passes, then execute compute and
   offscreen graphics through an adapted compiler/runtime. Keep display support
   and the remaining hardware matrix in scope independently of NPU progress.

Project issues [1–11](https://github.com/pedapudi/sunstoneos/issues) track
implementation, dependencies and acceptance criteria. Their scope includes all
hardware and final qualification independently of mimus.

[Issue 12](https://github.com/pedapudi/sunstoneos/issues/12) tracks the deferred
native NPU compiler and development toolkit. External compiler tools remain
acceptable until complete hardware support. Defer CPU, GPU, and NPU optimization
until all board hardware is functional. The [validation policy](validation.md)
limits bring-up measurements and defines when performance results need repeating.

No microphone is available. I2S and Bluetooth headset input remain required.
OV5640 is the first camera; the DSI panel is undecided. Missing hardware must
not remove coverage requirements.
