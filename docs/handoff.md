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
Neither application is physically qualified. Further board attempts require
recovery of the quarantined instance and diagnosis of multicore startup.

The [native Bluetooth application](bluetooth-native.md) is integrated with its
host preparation and shared capture adapter. Firmware bootstrap, UART1 and H4/HCI
identification connect to actual providers and copied USB output. Four focused
composition cases and seventeen host/wrapper cases pass. All 420 recovered native
dependencies are preserved; 176 repository inputs match except for one comment.
The host's seventeen preserved inputs also match integration. Evidence is under
`bluetooth-native-20261017/`. Physical identification, pairing/security and
headset audio remain unqualified.

The [supervised NPU startup](npu-application.md) now supports a disabled PLL with
live DRAM and a quiescent, already-powered domain. It reuses the existing clock
and power logic, preserves SRAM reset and makes no power-domain writes. One
native link and the focused startup/admission cases pass. All 468 inputs are
preserved; the actual native dependency closure contains 169 files, including
112 repository inputs with only two comment differences. The saved image is
reused without rebuilding. Evidence is under `npu-live-20261017/`.
Modeled startup reaches service activation; worker commands and physical
inference remain untested. Real supply, clock-source, SRAM-exclusion and
manual-power-removal admission remain required.

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
The current observer software record binds the tested native image, SCR-free
launcher and current host under `supervised-reconnect-20261017-072/software.json`.
The [supervised all-core observer](a733-smp-supervised.md) leaves the watchdog
disabled. Three physical attempts produced no native USB observer. The latest
attempt followed a successful returning register survey and fresh entry checks;
its single refusal-report read timed out. Removing the suspect SCR access did
not resolve startup. The user has reconnected and the failed session is closed.
Next diagnosis must locate the failing stage using the previously working
single-core return path before another full-image attempt.

The [native camera application](camera-application.md) connects actual providers,
OV5640 capture, retained full-frame copies and USB0 output. Its supervised mode
now uses explicit manual-power-removal admission and the shared host runner.
The native link, mode/admission checks and complete-frame collection checks pass.
All 155 native dependencies were verified; 101 repository dependencies and the
24 changed sources match integration. The saved image is reused without another
build. Evidence is under `camera-supervised-20261017/`. The camera module is not
connected; physical capture remains unqualified. The
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
routing and physical output remain outstanding. NPU startup with an unsuitable
supply remains unsupported. NPU and GPU packaging/capture integration is complete at the software
level. Their shared transport remains unchanged by the observer and board
approval correction. Physical access remains exclusive to the coordinator.

JPEG and Bluetooth share optional board startup callbacks. Camera integration
preserves the existing controller evidence and fixes its IOMMU admission to
derive recovery authority from the actual boot proof. The default watchdog
mode remains separate from supervised retention. HDMI framebuffer integration
and offline multicore startup diagnosis continue.

The native PMIC provider now accepts the APBS1 reference-clock parent selected
by the pinned DDR loader. It verifies the 24 MHz PLL reference and rejects later
route changes without writing clock or divisor registers. Focused provider checks
and native compilation pass; source and artifact comparisons are under
`pmic-parent-20261017/batch1/`. New application builds include this correction.
Earlier sealed images retain their original provider layout and need rebuilding
before using the additional parent support. Physical operation remains untested.
The early-boot NPU PLL draft is preserved without execution. The source-backed
live-DRAM path supersedes that approach and requires no boot0 overlay.

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

The current board instance is `003:073` on port `3-1`, with automatic USB access.
User-confirmed power removal replaced the failed instance. Its holder stopped,
the queue recorded the failure and subsequent claims resumed. No device command
has been issued to this new instance, and DDR has not been initialized.
The recovery record is `supervised-reconnect-20261017-073/recovery.json`.
The user is away for several hours; avoid another full-image attempt without a
specific diagnostic or correction.

On the preceding instance, fresh DDR initialization reported 12,288 MiB and two
4 KiB pages matched. Thirty-nine read-only control/status values matched the
reviewed entry conditions. The returning SRAM survey then captured SVC mode,
disabled MMU/caches, primary MPIDR, zero frequency declaration and RMR, and an
advancing counter. Every remaining launcher predicate passed. The survey restored
both SRAM intervals and ROM remained responsive; SCR was deliberately unread.
This is physical evidence for the survey and entry conditions, not multicore
startup or coherence. Records are under `supervised-reconnect-20261017-072/`.

The following SCR-free launch reused the unchanged native image. Uploads,
readback and one execution request succeeded, but no observer appeared within
19 seconds. The sole refusal-report read failed with USB timeout error -7.
Communication stopped immediately. The subsequent user reconnect allowed the
coordinator to close that retained session. Its `runtime/session.json` remains
the failure record. The invalid-access hypothesis did not resolve startup;
the failing instruction or stage remains unknown. Preserve successful evidence
and investigate the actual launcher/native entry against the working single-core
path before selecting another bounded diagnostic.
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

1. Diagnose the failed multicore launch offline and identify the smallest useful
   correction or diagnostic before another physical attempt. The user reconnected
   after the SCR-free attempt timed out; the new instance has no DDR state.
   Reuse the working return path and refresh admission for the selected diagnostic.
   The failed UFS probe, intentional watchdog expiry and unattended fault tests
   remain deferred. USB power-control investigation is closed.
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
   no MIPI camera or display is connected, and the DSI panel is undecided.
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
