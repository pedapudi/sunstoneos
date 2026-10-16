# Current work

The active objective is [goal.md](goal.md). FreeRTOS now runs on the physical
A733 boot CPU. The kernel check passes timer preemption, blocking queues, and
floating-point context checks. An EL3 secure-timer timeout recovers a forced
EL1 stall and returns to FEL. [Physical kernel evidence](a733-kernel.md) records
the image, resources, measurements, and limitations.

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
Its FreeRTOS service, native memory publication, platform resource setup, and
physical execution remain outstanding.
Persistent boot, application qualification, other CPU cores, and
complete hardware qualification remain outstanding. No complete hardware
subsystem is qualified. Development uses branch `bringup` in the isolated
`/home/sunil/git/sunstoneos-worktrees/bringup` worktree.

The [storage probe](storage.md) reaches the SD/MMC controller, but eMMC CMD1
times out after applying the reference clock-mode setting. Population and power
remain unconfirmed. Pin/clock restoration and automatic ROM return passed.
The [PMIC reader](power.md) identifies AXP8191 and reads enabled UFS rail settings.
UFS initialization then failed to return to ROM. The [watchdog driver](watchdog.md)
is prepared and passes host checks, but independent hardware reset remains untested.
ROM disassembly and an instruction-model check identify a direct FEL redirect
candidate. The watchdog's RTC flag is handled by boot0 and does not establish
storage-independent recovery. Register retention across reset remains unknown.
Qualify it before further new peripheral-access sequences.

## Observed host and device state

The attached FEL device (`1f3a:efe8`) identifies A733. The boot CPU identifies
as Cortex-A55 r2p0. DDR firmware reports 12 GiB LPDDR5, with transfers checked
at two distinct pages. [Boot evidence](boot.md) records initialization and the
EL1 handoff. Board revision and populated storage remain unconfirmed.
No persistent-storage writes have been issued.

The board did not return to FEL after the UFS probe. The old node `003:063`
remains visible; its cached descriptor is not evidence of ROM execution.
The host records that instance as unavailable for further requests. Independent
power-cycle/FEL recovery has been requested. After recovery, verify a new ROM
instance and reinitialize DDR; do not reuse previous RAM assets or signatures.
No new peripheral probe may run before independent watchdog recovery passes.
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

1. Recover the board independently. Establish a reset destination that cannot
   execute unknown storage code, then qualify the prepared hardware watchdog
   with an SRAM expiry test. The ROM redirect candidate and its unresolved
   retention requirements are in [watchdog.md](watchdog.md).
   Integrate recovery before further peripheral access.
   Resume UFS with the corrected capability-read ordering only after recovery
   protection passes. Implement read-only storage access and backup through the
   native transport; physical storage population is still unknown.
   Establish storage boot and recovery before overwriting existing contents.
2. Extend USB with interrupt delivery, cold clock/PHY setup, and remaining roles.
   Preserve independent task and host deadlines during controller work.
3. Physically validate paced replay after recovery qualification, then continue
   matrix optimization. Add worker cores
   with change-dependent parity checks. Keep accelerator and remaining hardware
   work active independently of the recorded-audio milestone.
4. Extend CPU and memory support to secondary cores, usable capacity, protection,
   and coherence, with checks selected from each change. The SMP emulator target
   now passes migration and register-state checks. The separate C/C++ runtime
   passes concurrent library checks. Mimus now passes two-core reference parity;
   extend matrix-kernel parallelization and qualify physical workers. Task-deletion
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

6. Bind the native NPU controller core to a FreeRTOS service with validated
   table/buffer publication, privileged MMIO, serialization, and platform
   power/clock setup. Implement native model relocation and obtain a pinned
   A733 host compiler for a small numerical fixture. The [NPU record](npu.md)
   identifies references, inspection evidence, and the implementation sequence.

Project issues [1–11](https://github.com/pedapudi/sunstoneos/issues) track
implementation, dependencies and acceptance criteria. Their scope includes all
hardware and final qualification independently of mimus.

No microphone is available. I2S and Bluetooth headset input remain required.
OV5640 is the first camera; the DSI panel is undecided. Missing hardware must
not remove coverage requirements.
