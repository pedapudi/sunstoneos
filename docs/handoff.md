# Current work

The active objective is [goal.md](goal.md). FreeRTOS now runs on the physical
A733 boot CPU. The kernel check passes timer preemption, blocking queues, and
floating-point context checks. An EL3 secure-timer timeout recovers a forced
EL1 stall and returns to FEL. [Physical kernel evidence](a733-kernel.md) records
the image, resources, measurements, and limitations.
The [bring-up sequence](bringup-sequence.md) orders recovery, shared platform
services, complete device functionality, qualification, and performance work.

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
Physical secondary-core startup and multicore application qualification remain outstanding.
The native [DMA descriptor encoder](dma.md) now passes a host execution model
for memory/FIFO chains, 35-bit addresses, guards, and invalid-input refusal.
The polled controller core now passes a noncoherent host model for concurrent
leases, completion after bus drain, bounded cancellation, and retained buffers
after faults. Both components compile for AArch64. Board integration and physical
DMA remain unimplemented or untested; no hardware access occurred in these checks.
The [native DMA memory adapter](dma-memory.md) now passes a FreeRTOS emulator
check for translations, permissions, cache instructions, and two serialized EL1
callers. Its RAM/FIFO admission policy also passes host checks. Board reservations,
clock/reset setup, and a production privileged service remain outstanding.
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
Platform resource setup, complete model admission,
and physical execution remain outstanding.
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
   Integrate recovery before further peripheral access.
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
   cleanup, interrupt nesting, and physical startup remain outstanding.
   Current evidence is in [smp.md](smp.md), [smp-runtime.md](smp-runtime.md),
   and [mimus-smp.md](mimus-smp.md).
5. Integrate the checked DMA controller core and native memory adapter with the
   platform. Reserve dedicated RAM, register verified FIFO routes, and supply
   production FreeRTOS serialization and clock/reset setup. Qualify completion
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
