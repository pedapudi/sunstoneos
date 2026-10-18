# Current work

The [accepted goal](goal.md) requires native FreeRTOS, complete board hardware
support and local mimus transcription. The [hardware matrix](hardware.md) is the
authoritative inventory. No complete subsystem has physical qualification.
The [development dashboard](progress-ui.md) reads these records automatically.

Development uses the isolated `bringup` branch in
`/home/sunil/git/sunstoneos-worktrees/bringup`. The main checkout is unchanged.
Local commits and issue updates are authorized; publishing or merging is not.
Parallel workers own separate worktrees and files. The root coordinator alone
operates the board, through the [test queue](board-test-queue.md) and exclusive
board lock. Inspect live worker state before assigning overlapping files.

## Working behavior and integration limits

FreeRTOS runs on the physical boot A55. The [kernel check](a733-kernel.md) passes
timer preemption, blocking queues and floating-point context checks. An EL3
secure timer recovers a forced EL1 stall, but did not recover the UFS probe.
The failed UFS probe remains disabled. Independent watchdog qualification is
deferred; unrelated supervised RAM-only tests use a reviewed recovery plan.
Physical USB programmed-I/O, C++ allocation and repeated mimus requests have
passed their recorded workloads.

[Mimus](mimus.md) matches host results on 32 development recordings through the
[native USB application](mimus-usb.md). The model remains unchanged. Two-core
[emulator execution](mimus-smp.md) also passes recorded and paced-input parity.
Physical paced replay, all-core execution, accelerator backends, real audio
input and final latency/resource qualification remain outstanding. Existing
compute time exceeds the target; optimization waits until all hardware works.

The [hardware matrix](hardware.md) records each implemented device path, its
software evidence and remaining modes. Recent integration includes native
HDMI startup, DMA USB result observation, GPU graphics memory, storage updates
and maintenance watchdog supervision. These paths have no physical qualification.

## Current integration

Native HDMI startup, DMA USB observation, GPU graphics resources, storage updates,
native camera binding and fixed GPU graphics submission are integrated. The
complete [maintenance image](storage-maintenance-launch.md) connects native
tasks, immutable launch admission, fatal-fault revocation and protected backups.

The [native mimus image](mimus-smp.md#native-eight-core-application-image) now
connects eight-core startup, shared providers, the SMP C++ runtime and existing
recorded/paced recognition with two inference workers. Ten focused lifecycle,
packaging and capture cases pass. Central mimus and maintenance builds pass;
full recognition on the native image and physical execution remain untested.

The [native NPU application](npu-application.md), [radio entropy provider](radio-entropy.md),
[Type-C source controller](typec.md) and [USB2 host path](usb2-host.md) are integrated.
Their focused software evidence is preserved and reconciled with the integrated
sources. The NPU application still needs real boot authority.

The [board application](board-application.md) integrates shared startup, separate
Type-C service and power tasks, simultaneous USB0 output and USB2 sector reads,
and [native radio startup](board-radio.md). Radio startup connects crypto-engine
entropy, WPA2 association and a bounded TCP exchange. Focused software checks and
the central board image build pass. All 156 actual repository dependencies match
the preserved application snapshot; no runtime checks were repeated for integration.
Full native startup, physical transfers and entropy qualification remain untested.

The board image still requires a qualified hardware-watchdog window. A Sol 6.1
worker is implementing a separate supervised all-core observer that requires the
watchdog to remain disabled. Its intended completion is USB result capture and
manual reconnection; it does not claim a software return to ROM after all-core
entry. A second worker owns a separate native camera application. Both use
isolated worktrees and request Astra source review before focused execution.
Neither may access the board. Root owns central integration and physical tests.

The GPU central integration compiles and links six affected targets against the
actual graphics/reservation adapters and fixed render service. Empty fixture
pools remain unusable. Artifacts and source comparisons are preserved under
`gpu-render-20261017/central/`; runtime cases were not repeated. The four component
cases exercise submission and retained failures with poisoned output, so they do
not establish successful GPU rendering. Native camera evidence and matching
integrated source identities are recorded in `camera-native-20261017/handoff.json`.
Its `root-reconcile.json` records the shared clock and PMIC profile extensions;
the camera's exercised scopes and rail decoding are unchanged. Complete
image/provider admission and physical capture remain outstanding.

The compact DMA image includes native result frames, privilege routing and a
host capture path. Focused checks passed on the linked image using refused
handoffs. That fixture image must not be deployed. Physical boot admission and
USB/DMA operation remain required.

I2S reconciliation is complete; no workload was repeated. Generated assembly,
objects and generation inputs for the original DMA composition are preserved
under `dma-pool-session-20261016/assembly-later-capture-20261017/`. Their original
pre-execution hashes were not recorded, so this later capture does not prove
those assembly bytes were compiled. The executed ELF retains its original hash.

The larger device-memory launcher is integrated with its focused software
evidence. It accepts the exact runtime layout and excludes both reserved pools
from uploads. It creates no physical memory or recovery authority, and ROM
return alone does not establish scheduler or device results.
The larger runtime passes full-pool publication, interior translation refusal
and deadline refusal in the native fixture. A corrected uncached-translation
check accepts matching effective shareability 2 or 3, as the existing uncached
adapter does; write-back memory still requires 3. The launcher accepts the
corrected image and manifest without repeating its instruction cases.
Central configuration includes the new target. The compact observer boot hook
was reconciled with the conditional source additions; its compact source remains
identical after preprocessing those additions, and its sealed bundle is intact.

GPU isolation research found no source-defined A733 acknowledgement that proves
completion of writes already accepted beyond the GPU. The [research record](gpu-isolation.md)
identifies the missing guarantee and asynchronous shutdown requirements. Retaining
memory after failure does not restore an already changed power state.

## Observed host and device state

The attached FEL device (`1f3a:efe8`) identifies A733. The boot CPU identifies
as Cortex-A55 r2p0. DDR firmware reports 12 GiB LPDDR5, with transfers checked
at two distinct pages. [Boot evidence](boot.md) records initialization and the
EL1 handoff. Board revision and populated storage remain unconfirmed.
No persistent-storage writes have been issued.

The user disconnected and reconnected the board. It now enumerates at `003:065`
as `1f3a:efe8`; the reconnect rule and current node permissions pass the host
check. No ROM command has been sent to this new instance. Reidentify it under
the board lock before protocol work, and treat DDR as uninitialized.
The prior `003:064` instance answered ROM requests. Its
[physical ROM capture](watchdog.md) differs from the published dump and supplies
the board-specific reset reference. SRAM workspace restoration passed there.
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
unqualified. Intentional expiry and unattended fault tests remain deferred;
this does not block separately reviewed supervised RAM-only work.
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

Component documents identify their source inputs, artifacts, selected checks and
limitations. Raw evidence is under `~/.cache/sunstoneos/evidence/`. The central
`hardware-integration-20261016f` through `hardware-integration-20261016q`
records preserve earlier source comparisons, build artifacts and source pins.
`gpu-render-20261017/central/integration.json` records the current GPU links
and component source reconciliation. Native mimus central build artifacts and
input comparisons are under `mimus-native-20261017-lifecycle/`. NPU, radio
entropy, Type-C and USB host handoffs and `root-reconcile.json` records are under
`npu-application-20261017/`, `radio-entropy-20261017/`,
`typec-native-20261017/` and `usb2-host-20261017/`. Complete radio and board
application evidence is under `board-radio-20261017/` and
`board-application-20261017/`. The latter includes central build inputs and
artifacts in `central-snapshot/`; `central-integration.json` references matching
external inputs already preserved by the worker. Storage native
checks and input reconciliation are under `storage-update-20261017/`, including
`native-handoff.json` and `native-root-reconcile.json`. Complete maintenance image
evidence and central source reconciliation are under `storage-maintenance-20261017/`.
Reuse results only
within their recorded source, ABI, configuration and hardware conditions.

The shared clock registry now uses 128-bit masks, 128 slots and 48-byte tokens.
TWI11 has an independent exclusive scope with shared FABRIC ancestry. One focused
high-bit ownership check passed; 61 affected native units and three compatibility
fixtures compiled without replaying earlier workloads. Evidence is under
`clock-wide-20261017/`. All token producers and consumers must use the same
layout. Previously sealed images retain their original ABI and evidence; the
complete maintenance image compiles its current consumers together.
The latest storage bootstrap contention correction follows that compile batch
and is included in the maintenance image build, not the earlier object evidence.
S_TWI1 and USB2 now have independent exclusive scopes. S_TWI1 holds APBS1
ancestry; the USB2 provider must explicitly reserve its selected and shared
parents. One focused coexistence/conflict check and changed-core compilation
pass without changing the token layout. Evidence is under
`clock-usb-scopes-20261017/`; native Type-C and USB provider checks now pass
within their recorded scope.

The cached eight-core runtime has instruction-model scheduler evidence. The
larger device-memory image also has native reservation publication/refusal
checks. Central links establish build integration only. The sealed observer
bundle remains intact; new image identities must not inherit its preparation
or physical-entry claims.

Known evidence limits remain explicit:

- The original DMA composition's generated assembly was archived later; its
  pre-execution hashes were not recorded. The original executed ELF is preserved.
- The first network lifecycle/entropy ELF was overwritten during fixture relink;
  its digest and logs survive. Later focused executables are preserved.
- Historical observer compiler resource headers lacked recorded hashes.
- JPEG's historical malformed-input runner and input archive are missing.
- Early eMMC routing evidence used the NAND pin function. Corrected SDC2/SDC3
  source and composed checks supersede that routing claim.

Apply [the validation policy](validation.md): select checks by changed behavior,
build affected targets once and preserve successful evidence. Commits, clean
integration and documentation changes do not justify replaying workloads.
The dashboard needs no separate status maintenance or recurring browser tests.

## Next actions

The accepted [goal](goal.md) prioritizes complete native device integration.
After hardware functionality, warm mimus recognition must finish within 0.05
times audio duration; 0.01 times is a stretch target for bounded investigation.
Startup and live end-of-speech latency are separate measurements. Existing
recordings and model identities remain the reference.

1. Complete and review the separate supervised all-core observer. Its intended
   recovery is manual reconnection after USB capture or failure. Prepare one
   bounded RAM-only physical attempt after its software checks and real entry
   admission are complete. Preserve storage,
   honor actual admission requirements and stop at the first transport failure.
   The failed UFS probe, intentional watchdog expiry and unattended fault tests
   remain deferred. USB power-control investigation is closed; no cable move or
   power command was requested.
2. Qualify the integrated maintenance image after independent recovery passes.
   Its physical use requires card/range ownership, protected backups and actual
   controller, memory and supply admission. No prepared bundle grants those
   conditions by itself.
3. Connect the integrated native camera and fixed GPU render service to complete
   application startup. Bind
   immutable boot admission, genuine resource ownership and qualified memory
   to executable applications. Retained buffers permit neither invented drain
   nor resource reuse. The larger memory profile supplies capacity; physical
   exclusion, coherence and device mappings remain required.
4. Qualify all-core startup, interrupts, scheduler results and coherent memory
   through the supervised observer when ready. Preserve the sealed
   [watchdog-dependent observer](a733-smp-observe-host.md) and its existing entry
   requirements. USB enumeration alone does not establish diagnostic success.
5. Complete every remaining path in the [hardware matrix](hardware.md), including
   USB host/roles, radio and entropy binding, Bluetooth security/headset procedures,
   audio, display, codecs, PCIe and power transitions. OV5640 is the first camera;
   module availability remains unconfirmed and the DSI panel is undecided.
   No microphone is available. Recorded audio remains the mimus fixture.
6. Qualify unchanged-model mimus with paced input and all cores. After every
   required hardware function works, optimize and measure complete recognition,
   startup, live latency, concurrency, recovery and resource use. Finish
   reproducible boot/update artifacts and physical qualification of all required
   functions. The full accepted goal remains incomplete.

Project [issues 1–11](https://github.com/pedapudi/sunstoneos/issues) track hardware
scope, dependencies and acceptance criteria. [Issue 12](https://github.com/pedapudi/sunstoneos/issues/12)
tracks the lower-priority native NPU development toolkit. External compiler tools
remain acceptable until complete hardware support.
