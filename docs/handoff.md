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

The returning boot monitor now requires both interrupt affinity-routing bits
already enabled and preserves them during cleanup. Seven focused instruction
cases and one secure-QEMU kernel run pass under `monitor-are-20261018/`.
The corrected monitor has no physical result; broader cleanup behavior remains
unqualified. This change leaves the frozen multicore diagnostics untouched.

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

Implementation and debugging use Sol 6.1 with medium reasoning; Astra reviews
substantive correctness risks. The coordinator alone operates the board.

CPU task reclamation and repeated GPU compute submission are integrated.
One finite QEMU case executes actual FreeRTOS/newlib cleanup after task migration
in both directions. Private stream closure, heap restoration and reuse of static
task storage pass. All 26 preserved source inputs match integration under
`smp-runtime-cleanup-20261018/`; the physical startup failure remains unresolved.

The GPU application executes two successive submissions of its fixed arithmetic
workload and copies each acquired result. The service preserves command queues,
advances completion counters and refuses output after cancellation or fault.
Five focused composition cases and host decoding checks pass; GPU numerical
writes remain modeled. The native compute image builds under
`gpu-completion-20261018/`. Physical rendering and computation remain unqualified.

NPU registration now accepts three reviewed compiled convolution artifacts,
selected before scheduling and fixed for the context lifetime. Two additional
model shapes match CPU references in the external simulator. Native construction,
two successive requests, result copies and matching completion release pass
focused modeled checks under `npu-models-20261018/`. First-result mismatch and
wrong completion tickets prevent success publication. The built image is for
software validation only; physical inference and memory confinement remain
unqualified. Arbitrary models, runtime model switching and mimus acceleration
remain required follow-up work.

Combined thermal/ADC monitoring is integrated with repeated copied samples,
shared-parent ownership and readback-confirmed converter disable on stop.
Thirteen controller stop cases, twenty modeled native composition/USB cases and
four host groups pass under `monitor-application-20261018/`. Integration reuses
the frozen image; physical sampling, analog drain and recovery remain unqualified.
Reusable read-only microSD access, continuous fan/LED control and the PCIe
supply correction are also integrated. The coordinator
alone performs physical board tests; independent software work proceeds in parallel.

The RTC USB integration check is complete. Five cases execute the actual native
serializer, TinyUSB initialization, vendor FIFO, events and completion flow using
the frozen service image. Two identical 2,480-byte results survive fragmentation
and backpressure; cancellation, poison and provider/deadline refusal cases pass.
The controller endpoint/events and lower provider statuses remain modeled.
Evidence under `rtc-usb-native-20261018/` was accepted and integrated without
rebuilding or replaying earlier service checks. Physical USB and complete
provider initialization remain unqualified.

The shared prerequisite for fan/LED and combined monitoring is integrated:
APBS0 stability ownership and actual PMIC source/divider validation, plus atomic
publication of a stable GPADC24M parent. Two new focused checks pass under
`pwm-service-20261018/prerequisite/`; integration reuses their frozen artifacts.
The provider's new internal APBS0 field requires matching caller/fixture layouts
in the next native images. The combined monitor image includes the complete prerequisite. Its stop path
checks converter disable independently of publication; resources remain retained.
Fan updates may introduce an explicit low interval. Only the coordinator may
operate the board.
The bounded observer attempt on the reconnected board did not return. Instance
`003:084` is quarantined; the runner retains its lock and claim. Further device
commands are stopped pending physical recovery and source diagnosis.

- Startup diagnosis: table construction, cache publication and CPU/DSU planning
  passed physically. The observed identity is supported and needs no workaround
  writes. EL1 walk blocking, exact control restoration and interrupt-controller
  reads also pass. Complete distributor setup and restoration now pass physically.
  Core 1 now powers on, reaches native entry and powers itself off. The primary
  verifies OFF and restores saved state before returning to ROM. Execution after
  RELEASE, cache coherence and scheduling remain unresolved. The diagnostic for
  secondary interrupt initialization passes five focused software cases, but its
  physical launch did not return. A further observer now reports selected failures
  before any redistributor write and returns only after observed target OFF.
  Its three focused software cases pass; the physical failure remains unlocalized.
- Bluetooth: authenticated finite headset capture, 8-to-16 kHz conversion and
  native mimus transcription are integrated. Actual host
  recognition matches the reference for the same converted audio, and cancellation
  prevents copied publication under `bluetooth-mimus-native-20261017/`.
  The recognition-enabled image now includes retained startup inputs; one focused
  model-configuration handoff case passes, and all 210 compiled repository inputs
  match integration under `board-session-seal-mimus-20261018/`. The case models
  the recognizer; earlier audio and inference results were reused. Physical
  headset operation, native USB serialization and execution within the supervised
  window remain unqualified.
- I2S: finite capture-to-mimus is integrated for one to five seconds of 16 kHz
  mono audio. The native image links, and focused capture, admission, cancellation
  and copied-output checks pass. Startup retains the complete authenticated
  session before provider initialization, including capture duration and model
  identities. The corrected image and five focused binding checks pass; all 127
  preserved repository inputs match integration under `i2s-binding-20261018/`.
  Earlier audio and inference evidence under `i2s-mimus-20261017/` was reused.
  No microphone is connected; physical capture and inference timing remain
  unqualified.
- Camera: startup retains and authenticates the complete session before provider
  initialization. The corrected image and five focused binding checks pass;
  all 120 preserved repository inputs match integration under
  `camera-binding-20261018/`. Existing capture and pixel checks were reused.
  No MIPI camera is connected; physical capture remains unqualified.
- HDMI: the framebuffer image retains and authenticates its complete session
  before counter and provider initialization. The corrected image and five
  focused binding checks pass; all 97 preserved repository inputs match
  integration under `hdmi-binding-20261018/`. Existing display checks were reused.
  Physical monitor output remains unqualified.
- GPU: the native application supports fixed rendering or repeated fixed arithmetic
  computation. The compute image includes the current PMIC clock-parent provider,
  copied numerical results and a distinct host capture format. Focused software
  checks pass under `gpu-completion-20261018/`; the earlier render image remains
  preserved under `gpu-pmic-refresh-20261018/`. Actual startup, memory isolation,
  firmware operation and successful physical workloads remain unqualified.
- JPEG: the nonconstant diagnostic is integrated. Its integer oracle matches
  all 2,304 CPU-decoded samples exactly; the native link and focused parser,
  copy/layout and host refusal checks pass. Physical decoding remains unqualified.
  Evidence is under `jpeg-nonconstant-20261017/`. Startup now copies and verifies
  the complete session against its admitted digest before provider initialization;
  USB failure revokes board liveness before parking execution. The affected image
  and focused checks pass. Frozen inputs are reconciled under
  `jpeg-sealing-20261018/`; prior decoder evidence was reused. That image retains
  the JPEG provider source from before the separate H.264 addition.
- H.264: the fixed Baseline IDR/I_PCM application is integrated, including native
  initialization, guarded DMA buffers, pixel verification and copied USB output.
  Its image and focused checks pass; all 174 compiled repository inputs match
  integration. The independent CPU reference and instruction traces were reused.
  Physical decoding and general H.264 support remain unqualified. Evidence is
  under `h264-native-20261018/`.
- Crypto: the native SHA-256/AES application is integrated. Its image links,
  and focused application and host checks pass. A physical snapshot confirms
  the adopted CE clocks.
  Predecessor task completion, future launch exclusion, secure status access
  and memory authority remain physical prerequisites. A boot-path audit is
  investigating the predecessor handoff. Inspection of the actual DDR initializer
  found no crypto launch path; actual ROM/FEL history remains unresolved. No board
  crypto command has executed. Session binding and USB failure revocation are
  corrected; the affected image and focused checks pass. All 183 preserved
  repository inputs match integration under `crypto-sealing-20261018/`.
- Wi-Fi networking: fixed-address and DHCP board profiles are integrated, and
  the combined board image links. Profile checks pass under ASan/UBSan. An earlier
  unexplained DNS deadline miss remains a timing limitation.


Component documents and the hardware matrix hold the remaining integrations.
The [historical handoff](handoff-history-20261017.md) preserves earlier artifact
locations and reconciliation details; it is not a current qualification record.

The [shared board and Bluetooth startup paths](board-session-seal.md) now retain
authenticated session and profile copies before SID access or provider setup.
Networking hashes the same private profile used for runtime derivation. Both
affected images and 15 focused binding cases pass; 199 unique compiled repository
inputs match integration under `board-session-seal-20261018/native/`.
The additional recognition-enabled Bluetooth image and one focused startup case
also pass. Existing codec, protocol and recognition results remain reusable where
their inputs are unchanged.
NPU already compares its private copy with the retained authenticated digest,
and GPU already copies and verifies its session at EL3.

The [native thermal application](thermal-application.md) now connects the
five-sensor driver to shared resources, deferred interrupt handling and copied
USB output. It collects two successive sets with an observation time for each
channel. One native image, admission checks, 13 modeled native cases and four
host cases pass. All 175 compiled repository inputs match integration under
`thermal-application-20261018/`; existing calibration and controller evidence
was reused. Physical acquisition, accuracy, interrupt delivery and recovery
remain unqualified. Automatic thermal policy remains required.

The [native RTC application](rtc-application.md) now supplies repeated coherent
read, calendar-set and non-waking alarm operations through one owner and two
registered native clients. A static FreeRTOS queue serializes requests, and
client-bound generations prevent stale or cross-client cancellation and copies.
Verified alarm completion permits reuse; uncertain failures retain resources and
refuse further operations. Five cases using the actual FreeRTOS POSIX scheduler,
plus native binding/provider, launcher and host checks pass under
`rtc-service-native-20261018/`. All 23 owned files match integration. Of 182 compiled
repository inputs, four differ only by unused additive CLDO3 and PWM pin support.
The frozen image and successful checks were reused without rebuilding or replay.
Physical timing, UTC accuracy and physical USB remain unqualified. Source switching,
wider client registration, wake, calibration, reset-cause reporting and applicable
retention modes remain required.

The [native PCIe application](pcie-application.md) connects the root-port,
PHY and shared providers to x1 Gen1 observation and copied endpoint configuration.
Its corrected PD-bank supply contract reserves BLDO1 alongside the DCDC2 core
supply. Host admission separately requires fitted-board routes and named analog
supplies with stable voltage and writer exclusion. It no longer accepts a BLDO2
snapshot as evidence of PD-bank power. One affected image and focused provider,
native-request and electrical-admission cases pass under
`pcie-pd-rail-native-20261018/`; all 174 compiled repository inputs match integration.
Earlier PHY/protocol evidence was reused. Physical routing, measured supplies,
endpoint behavior and recovery remain unqualified. BAR allocation, interrupts,
DMA, higher link speeds and power transitions remain required work.

The [supervised read-only SD service](storage-read-application.md) now serves
repeated ranges to a native application task and the USB task through one MMC
owner. Copied results and matching acknowledgments permit reuse of each client
slot. One native image, eleven modeled native cases and host/package checks pass
under `storage-read-service-20261018/`. The inserted microSD remains untouched.
Physical SD operation, unnoticed card replacement and hotplug recovery remain
unqualified. Initial read-only discovery uses the supervised return/manual
power-removal plan; separate maintenance operations retain their own admission.

The [native SPI application](spi-application.md) integrates SPI3 chip select 0
on the header, with BLDO1 supplying its 1.8 V signal bank. One native link,
admission and nominal-provider checks, thirteen modeled native cases and four
host groups pass. Evidence and integration reconciliation are preserved under
`spi-application-20261018/`. The obsolete fixture clock-token producer now derives
its layout from the final image; the initial refusal and completed build were
preserved. Physical transfers require an identified peer, authorized command
effects, verified wiring and voltage, and recovery for every affected supply.

The [native fan/LED application](pwm.md) now supports continuous 40 kHz PWM,
duty changes, verified GPIO-low stop and retained-lease restart. Optional PL7
GPIO commands preserve neighboring outputs. Actual provider and pin checks,
four linked cases and host command/admission checks pass under
`pwm-service-20261018/`. All 24 owned files and 178 compiled repository inputs
match integration; only the inactive ADC CMake option differs. The frozen image
was reused without another build. Duty updates introduce a low interval;
uncertain failures may leave continuous output running until full power removal.
Physical waveform, gate discharge, fan-off current and visible LED behavior
remain unqualified. Other PWM channels and modes remain required.

The [native ADC application](adc-application.md) now samples the four board-selection
dividers through actual shared clock, CLDO3 supply and native register providers.
It publishes two sets of raw observations with deferred interrupt service and
copied USB results. Eighteen core cases, admission checks, sixteen modeled native
cases and four host groups pass under `adc-application-20261018/`.
Integration preserves the frozen image; only an unused PWM pin helper and its
declaration differ among compiled repository inputs. The wrong-exception-level
case models the CurrentEL register value; it does not establish EL2 execution or
EL0 isolation. Physical sampling, safe AVCC, reference accuracy, divider population
and recovery remain unqualified. The selection dividers permit observation only;
no external drive or strap modification is authorized. Repeated combined monitoring
is integrated separately as described above.


## Observed host and device state

The attached FEL device (`1f3a:efe8`) identifies A733. The boot CPU identifies
as Cortex-A55 r2p0. DDR firmware reports 12 GiB LPDDR5, with transfers checked
at two distinct pages. [Boot evidence](boot.md) records initialization and the
EL1 handoff. Board revision and populated storage remain unconfirmed.
No persistent-storage writes have been issued.

The last board instance is `003:084` on port `3-1`. Its diagnostic did not return
and the instance is quarantined. Runner session `51570` retains the board lock
and queue claim `03a79df09c6883041185a6dd61ebdb46fec5e97cb1f1990f13a569d0e4cc3944`.
Do not send further device commands or terminate the ownership holder before
coordinator recovery review. Physical reconnection remains required.
The returning native diagnostic passes the corrected A32 launcher, stack setup,
BSS clearing, admission, table construction, cache publication and CPU/DSU
planning. Observed MIDR is `0x412fd050`, CLUSTERIDR is `0x41` and CLUSTERCFR is
`0x073fc237`; no workaround writes are requested.

Native EL1 walk blocking and complete interrupt-distributor setup now pass
physically. The returning diagnostic restores SCTLR_EL1
to `0x00c50838` and TCR_EL1 to zero with exact readbacks, then restores the
interrupt distributor. GICD_CTLR changes from `0x37` to native terminal `0x33`
and returns to its original value. All eight SPI-enable banks match their saved
values; both affinity-routing bits remain set. Cache coherence and multicore
scheduling remain unproven.

Core 1 now powers on and reaches its native ARRIVED report at EL3 with caches
disabled. It identifies as Cortex-A55 r2p0 with MPIDR `0x81000100`. The diagnostic
stops before RELEASE and requests target-owned shutdown. The primary observes
both the matching acknowledgment and actual PPU OFF, then restores the original
control, mode and reset configuration. GIC and EL1 restoration also pass.
All 48 transactions, including copied state and DRAM restoration, passed.
The runner exited and its queue claim closed. Evidence is under
`startup-stage-20261017-082/native/`.

The earlier interrupt-initialization attempt on `003:083` did not return.
The user then disconnected and reconnected board power. Instance `003:084`
responded to FEL identification with the existing permissions. Fresh DDR
initialization, the returning platform survey and one read of all seven
secondary power states passed. Previous RAM contents and ownership continuity
were discarded; the old failed queue claim was closed after recovery review.

One attempt with the frozen pre-write failure observer also did not return.
All 27 host transactions completed, including launch, but no new ROM instance
appeared within ten seconds. No diagnostic report was recovered. This outcome
does not establish whether target admission, primary redistributor reads or a
later operation failed. No retry or further device command occurred. Fresh
preparation, failed execution and retained backups are under
`reconnect-20261018-084/`. Source review must identify a narrower observation
boundary or concrete correction before another launch.

The installed session rule grants reconnect access on port `3-1` until host
reboot. Use `tools/usb-access.py` to inspect or disable it. The user reconnected
with a microSD inserted and no button held; preserve that card. No MIPI camera or
display and no microphone are connected. The DSI panel is undecided; OV5640 is
the first camera target. A headset and HDMI monitor are unconfirmed.

The separate ESP32 `mimus keyboard` device is unrelated. Do not use it as the
Orange Pi console or send keyboard events to the desktop. No Orange Pi UART
adapter is visible.


## Reusable validation evidence

Reuse frozen images and completed checks when their source, dependencies,
configuration and hardware conditions still apply. A commit, clean integration
or documentation edit does not justify a rebuild or runtime replay. Follow
[the validation policy](validation.md); the dashboard requires no recurring
browser test or manual status update.

Raw artifacts are under `~/.cache/sunstoneos/evidence/`. Current evidence:

- Startup: `startup-stage-20261017-082/native/runtime/session.json` records
  core-1 native entry and target-owned shutdown, with observed OFF and verified
  control, configuration and GIC/EL1 restoration.
  `native-secondary-off-probe-20261017/` contains the frozen probe
  and five focused cases. Earlier distributor evidence remains under
  `startup-stage-20261017-079/`.
  `native-secondary-gic-ready-probe-20261018/` records the integrated interrupt
  initialization diagnostic and five focused cases. Only its corrected timeout
  fixture was repeated. The physical attempt and retained backups are under
  `startup-stage-20261017-083/native/runtime/`; no result or restoration was observed.
  `native-secondary-prewrite-observer-20261018/` contains the later observer and
  three focused cases for target admission refusal, primary snapshot exception
  and capture-fault retention. Its physical attempt under
  `reconnect-20261018-084/native/runtime/` did not return; no report was recovered.
- Crypto clocks: `crypto-clocks-20261018-082/session.json` records 13 read-only
  CCU/RTC observations. The CE clock, gates and reset releases are enabled;
  these observations do not prove engine quiescence or secure-register access.
- Crypto application: `crypto-native-20261018/handoff.json` identifies the
  frozen image and focused checks; `integration.json` reconciles every preserved
  repository input. The original admission-fixture sanitizer failure is retained,
  and only the corrected admission case was repeated. The modeled-authority
  package is not deployable; physical admission requires actual evidence.
  The corrected image is identified by `crypto-sealing-20261018/handoff.json`;
  its unchanged arithmetic evidence was reused without replay.
  `ce-predecessor-20261018/assessment.md` records the bounded DDR/ROM source audit
  and the proposed ROM observation. Board recovery precedes that observation.
- Bluetooth: `bluetooth-mimus-native-20261017/integration.json` reconciles all
  changed files and preserved inputs. Its `native-final/manifest.json` identifies
  the image; `composition/` holds actual host recognition and cancellation.
- I2S: `i2s-mimus-20261017/handoff.json` and `integration.json` identify the
  image, focused results and matching sources. DMA and recognizer responses are
  modeled in these capture fixtures; they do not prove physical accuracy.
- Recorded mimus: [physical single-core evidence](mimus-usb.md) covers 32
  recordings. [Supervised native integration](mimus-smp.md) and the shared
  recorded-entry compatibility link remain separate from physical SMP proof.

The [component records](hardware.md) identify the GPU, NPU, camera, JPEG,
network, storage and USB artifacts. Older sealed images retain their original
ABI and provider assumptions. In particular, clock tokens use 128-bit masks and
48-byte tokens; do not mix old and current producers. New PMIC builds support
the observed APBS1 parent. Older images need an affected rebuild before using
that additional parent support.

Preserve these limitations when reusing evidence:

- The original DMA assembly was archived after execution; pre-run hashes are
  absent, although the executed ELF survives.
- The first network lifecycle/entropy ELF was overwritten; its logs and digest
  survive. The earlier Bluetooth host snapshot was also captured after execution.
- Historical observer compiler resource headers lack hashes. JPEG's historical
  malformed-input runner and input archive are missing.
- Early eMMC routing used the NAND function; corrected SDC2/SDC3 checks supersede it.
- The compact DMA image with refused fixture handoffs must not run on hardware.
- GPU drain/isolation after failure remains unresolved; retained buffers do not
  establish completed writes or reverse a power transition.

The host toolchain and Python environment are described in [build.md](build.md).
Preserve configured compiler/linker/venv paths. Sources, packages, builds and
models live under `~/.cache/sunstoneos`; no runtime filesystem is implied.

Project [issues 1–11](https://github.com/pedapudi/sunstoneos/issues) track hardware
scope and dependencies. [Issue 12](https://github.com/pedapudi/sunstoneos/issues/12)
tracks the deferred native NPU toolkit. Suitable open-source dependencies remain
preferred; opaque replacements follow hardware functionality and optimization.

## Next actions

The accepted [goal](goal.md) prioritizes complete native device integration.
After hardware functionality, warm mimus recognition must finish within 0.05
times audio duration; 0.01 times is a stretch target for bounded investigation.
Startup and live end-of-speech latency are separate measurements. Existing
recordings and model identities remain the reference.

The next offline diagnostic will bypass the primary interrupt-frame scan and
stop core 1 after native RELEASE and CPU preparation, before translation-walk
changes or any redistributor access. It must copy the observed state, use the
proven target-owned shutdown and verify actual OFF before returning. Pinned
sources support the redistributor base and stride but do not establish safe
access to every frame while cores are off; a stalled read remains a hypothesis.
The implementation owner and reviewer are preparing this narrower boundary in
an isolated worktree. No further physical attempt is authorized by this plan.

1. Diagnose the nonreturning pre-write observer before another launch. Core-1
   activation, entry and shutdown before RELEASE pass physically, but both the
   interrupt-initialization diagnostic and its failure observer returned no report.
   Preserve the held claim and backups until physical recovery is confirmed.
   A further power reconnect requires fresh DDR and admission. Do not repeat
   the same observer unchanged. The failed UFS probe, intentional watchdog expiry
   and unattended fault tests remain deferred; USB power-control work is closed.
2. Qualify all-core startup, interrupts, scheduler results and coherent memory
   through the supervised observer before dependent full-device images.
   Preserve the sealed [watchdog-dependent observer](a733-smp-observe-host.md)
   and its existing entry requirements. USB enumeration alone does not establish
   diagnostic success.
3. Use the integrated read-only SD application for initial card identification
   and a sector-zero read after actual controller, memory and supply admission.
   Preserve the inserted card's contents. The separate maintenance image needs
   card/range ownership, protected backups and its qualified watchdog procedure;
   it is not the first discovery path.
4. Establish actual power, memory and recovery admission for the integrated GPU,
   NPU, codec, RTC and thermal applications. Their internal operations can advance
   without camera, display or microphone accessories. Retained buffers do not
   establish completed device writes or permit resource reuse. The larger memory
   profile supplies capacity; physical exclusion, coherence and device mappings
   remain required. Camera capture separately requires the actual module and wiring.
5. Complete every remaining path in the [hardware matrix](hardware.md), including
   USB host/roles, radio and entropy binding, Bluetooth security/headset procedures,
   audio, display, codecs, PCIe, SPI, routed PWM/ADC and power transitions.
   Complete the remaining RTC modes and physical service qualification. OV5640 is the first camera;
   no MIPI camera or display is connected, and the DSI panel is undecided.
   No microphone is available. Recorded audio remains the mimus fixture.
6. Qualify unchanged-model mimus with paced input and all cores. After every
   required hardware function works, optimize and measure complete recognition,
   startup, live latency, concurrency, recovery and resource use. Finish
   reproducible boot/update artifacts and physical qualification of all required
   functions. The full accepted goal remains incomplete.
