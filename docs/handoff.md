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

Native I2S and JPEG device paths are integrated. The [I2S application](i2s-application.md)
connects DMA0 capture or recorded playback to retained buffers and USB0 output.
Its saved image passes twelve initial checks and four USB identity and failure
checks. All 191 preserved compiler dependencies were verified; 128 repository
dependencies match integration after accounting for the two rebuilt source files.
The USB correction reuses 64 objects. Root configuration passes and its compiler
and linker options match the saved build after path normalization. Broad header
hashing changes generated identity constants; the frozen worker image retains
its original identity. Evidence is under `i2s-application-20261017/`.

The [JPEG application](jpeg-application.md) connects shared startup, decoder IOMMU
mappings, guarded buffers and verified copied output. Its central build and
focused software checks pass. All 419 compiler dependencies were recovered from
the existing build and verified against preserved inputs. The 173 repository
inputs match integration except for one comment-only change. The collector now
requires dependency coverage for every linked object and rejects empty records.
The JPEG host adapter is integrated; sixteen focused cases pass, with earlier
results preserved after a fixture repair. Its aggregate setup check reports no
mismatches. Sources and artifacts were reconciled under
`jpeg-supervised-host-20261017/batch1/`. The I2S host adapter is also integrated;
its first focused batch passes 27 case records, including four SRAM launcher
instruction cases. Sources and artifacts match integration, with a documented
source-only reporting-key correction. Evidence is under
`i2s-supervised-host-20261017/`. Both adapters reuse shared transport and retained
ownership handling. Their modeled admission records cannot authorize hardware.
Neither application is physically qualified. The first board attempt still
requires recovery of the quarantined instance.

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
sources. The NPU application also builds through the root CMake configuration
when its handoff and artifact paths are supplied. The central link passes;
application source properties remain isolated from other targets. Existing
runtime evidence is reused. The supervised image also links through the root
configuration. Its actual PMIC sampling, PLL retuning and startup token adoption
pass with modeled device responses. The 460 preserved build inputs were checked;
native inputs match, so the worker image is reused without rebuilding. Physical
NPU execution still needs real boot authority and full startup qualification.
The host adapter passes 33 cases plus five binding regressions; its nine source
files match the final snapshot. Evidence is under `npu-supervised-20261017/` and
`npu-supervised-host-20261017/`.

The [board application](board-application.md) integrates shared startup, separate
Type-C service and power tasks, simultaneous USB0 output and USB2 sector reads,
and [native radio startup](board-radio.md). Radio startup connects crypto-engine
entropy, WPA2 association and a bounded TCP exchange. Focused software checks and
the central board image build pass. All 156 actual repository dependencies match
the preserved application snapshot; no runtime checks were repeated for integration.
Full native startup, physical transfers and entropy qualification remain untested.

The default board image still requires a qualified hardware-watchdog window.
Its integrated supervised mode leaves the watchdog disabled and retains resources
until manual power removal. Its central build passes with 154 matching repository
dependencies. Later shared-header edits change comments only. The host capture
tool shares the observer's lock, queue and transport handling; 25 focused cases
pass against the integrated sources. Evidence is preserved under
`board-supervised-20261017/central/` and
`board-supervised-host-20261017/batch1/`.
The shared tool also accepts NPU and GPU result fields. Observer and board
approval now binds directly to the initially validated image, launcher, admission
and metadata hashes. Ten focused approval cases pass; transport and capture
functions remain identical to their prior implementation. Source and artifact
comparisons are under `supervised-bundle-binding-20261017/batch1/`.
Existing native, launcher and transport results remain valid within their scopes.
Before physical use, compose the selected bundle's software record from these
results and current source snapshots; older records retain superseded source hashes.
The observer record now includes the corrected launcher and current host sources
under `supervised-bundle-binding-20261017/batch1/integration/`. Its referenced
artifacts were verified without replaying tests. Fresh physical entry evidence
is still required after manual reconnection.
The separate
[supervised all-core observer](a733-smp-supervised.md) requires the watchdog to
remain disabled. Its native build and focused startup, admission and host checks
pass, including central integration. USB capture leaves the board running until
manual reconnection; successful capture retains the board claim and lock.
The first physical attempt produced no USB observer within the capture window.
Uploads and the execution request succeeded, but no scheduler result arrived.
The board instance is quarantined pending manual reconnection.
Its coordinator now holds `/tmp/sunstoneos-a733-3-1.lock`; the unrelated mimus
lock has been released. The replacement holder acquired the A733 lock before
the original process stopped. The queue claim and quarantine remain in force.
The physical-entry runtime directory contains `lock-transfer.json` with both
process identities. No board command was issued during this transfer.

The [native camera application](camera-application.md) connects actual providers,
OV5640 capture, retained full-frame copies and USB0 output. Its native build and
nine focused application checks pass, including central integration. The camera
module and physical launch conditions remain unqualified. The
[native GPU application](gpu-application.md) also integrates actual providers,
firmware, fixed rendering and copied USB output. Its central build and focused
retention/revocation checks pass. Missing physical isolation prevents shutdown
or reuse; a successful fixed render retains resources for the boot. Successful
GPU output and physical operation remain untested. The supervised board mode and
native HDMI colorbar application are integrated and pass their focused software
checks. Their physical operation remains untested. Inspect live assignments
before changing their files.

The supervised GPU mode is integrated. Its root-configured build and focused
session, launcher, packaging and refusal cases pass. All 151 repository
dependencies match the integrated sources, so the worker's linked image is reused
without another build. Default-mode compatibility units also compile. Evidence
and source reconciliation are under `gpu-supervised-20261017/batch1/`.
The GPU host adapter also passes 19 focused cases. All eight source files and
preserved artifacts match integration; evidence is under
`gpu-supervised-host-20261017/`. Physical rendering remains untested.

The HDMI application connects actual providers to a fixed 720p60 colorbar.
Its central build matches all 65 repository dependencies, and its 17 focused
software cases pass. Evidence is under `hdmi-application-20261017/`; framebuffer
routing and physical output remain outstanding. Cold NPU clock provisioning
and an unsuitable supply still require a separate implementation and physical
evidence. NPU and GPU packaging/capture integration is complete at the software
level. Their shared transport remains unchanged by the observer and board
approval correction. Physical access remains exclusive to the coordinator.

Native JPEG, I2S and Bluetooth integration is in progress in separate worktrees.
JPEG connects the existing decoder to actual memory translation, shared resources
and copied USB output. I2S connects capture or recorded playback to the actual DMA
pool and stream service. Bluetooth connects radio firmware bootstrap, UART1 and
controller identification without requiring a headset. These applications have
not completed their builds or focused checks. JPEG and Bluetooth share optional
board startup callbacks; existing board behavior is unchanged without that option.

The native PMIC provider now accepts the APBS1 reference-clock parent selected
by the pinned DDR loader. It verifies the 24 MHz PLL reference and rejects later
route changes without writing clock or divisor registers. Focused provider checks
and native compilation pass; source and artifact comparisons are under
`pmic-parent-20261017/batch1/`. New application builds include this correction.
Earlier sealed images retain their original provider layout and need rebuilding
before using the additional parent support. Physical operation remains untested.
An opt-in early NPU PLL step is also in development; it must preserve shared SRAM
and measure state after DDR training before any later NPU admission.

The GPU central integration compiles and links six affected targets against the
actual graphics/reservation adapters and fixed render service. Empty fixture
pools remain unusable. Artifacts and source comparisons are preserved under
`gpu-render-20261017/central/`; runtime cases were not repeated. The four component
cases exercise submission and retained failures with poisoned output, so they do
not establish successful GPU rendering. Native camera evidence and matching
integrated source identities are recorded in `camera-native-20261017/handoff.json`.
Its `root-reconcile.json` records the shared clock and PMIC profile extensions;
the camera's exercised scopes and rail decoding are unchanged. Complete native
application linkage now passes; physical admission and capture remain outstanding.

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

The last tested board instance was `003:065` on port `3-1`, with valid session access.
Fresh RAM-only DDR initialization passed once through the existing loader:
12,288 MiB reported, board setup result zero, and two distinct 4 KiB RAM pages
matched. The loader returned to FEL without a reset or persistent write.
Evidence is under `supervised-ddr-20261017-192435/`. A subsequent read-only
snapshot found all seven secondary cores inactive by the startup driver's
criteria, inherited USB0 clocks/reset state, and a disabled watchdog. Its record
is `supervised-entry-state-20261017-193112/result.json`. These observations do not
by themselves establish auxiliary-core/DMA exclusion or every cached-entry
condition. The follow-up `supervised-master-state-20261017-193555/` records
E902 clock gates disabled, DMA0/1 reset asserted and clocks gated, and MBUS gates
`0x4`. Its `ce/` record shows the crypto engine enabled; `ce-idle/` records zero
secure/nonsecure interrupt, task-status, trigger and error fields. No control was
changed. The reviewed physical-entry assessment and failed observation are under
`smp-supervised-20261017/physical-entry/`. Its `runtime/session.json` records one
execution request, no transport failure and no native observer within 19 seconds.
The host stopped board traffic and retained the test claim and exclusive lock.
Manual reconnection has been requested; do not contact the quarantined instance.

The launcher used for the failed attempt rejected a zero counter-frequency
declaration, although earlier board evidence records that initial value. Native
startup sets the declaration only after the launcher. This may explain the
missing observer, but the physical cause remains unconfirmed. The reviewed
correction accepts an unset frequency declaration and can report a clean refusal
after checking the session identity and restoring SRAM. Two changed launcher
cases and nine host cases pass; the all-core image was not rebuilt. The integrated
sources and preserved artifacts match the evidence under
`smp-supervised-20261017/frequency-refusal/`. The existing held process uses the
earlier implementation and still requires manual reconnection.
The prior `003:064` instance answered ROM requests. Its
[physical ROM capture](watchdog.md) differs from the published dump and supplies
the board-specific reset reference. SRAM workspace restoration passed there.
Read-only recovery-register inspection found no redirect owner and a disabled
watchdog. The boot input was high, so it cannot currently protect a reset from
storage boot. The user confirmed automatic FEL entry after disconnecting and
reattaching USB, with a microSD card inserted and no button held. Preserve that
card. No watchdog was armed and no reset was issued.
The failed UFS instance `003:063` remains quarantined. DDR evidence applies only
to the instance on which it was collected; reconnecting invalidates RAM contents.
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
external inputs already preserved by the worker. Camera and supervised observer
central records are under `camera-application-20261017/` and
`smp-supervised-20261017/`. All 105 camera and 30 observer repository dependencies
match their worker snapshots; runtime cases were not repeated. GPU application
central artifacts are under `gpu-application-20261017/`; all 150 actual repository
dependencies match its worker snapshot. Its optional isolation and revocation
fields change the backend ABI; older images retain their original scoped evidence.
Storage native
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

1. After confirmed manual reconnection, close the held observer
   test and refresh the board's DDR and entry evidence. Preserve storage, honor
   actual admission requirements and stop at the first transport failure. Use
   the corrected launcher and clean-refusal reporting for the next reviewed attempt.
   The failed UFS probe, intentional watchdog expiry and unattended fault tests
   remain deferred. USB power-control investigation is closed; no cable move or
   power command was requested.
2. Qualify the integrated maintenance image after independent recovery passes.
   Its physical use requires card/range ownership, protected backups and actual
   controller, memory and supply admission. No prepared bundle grants those
   conditions by itself.
3. Establish real power, memory and recovery admission for the integrated GPU
   and camera applications. Actual module wiring remains required before camera
   capture; successful GPU output remains to be observed. Retained buffers permit neither invented drain
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
