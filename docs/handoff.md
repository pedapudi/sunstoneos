# Current work

All seven secondary cores now pass production CPU preparation and local GIC
initialization on the original board. Each target restores its local interrupt
state and actual translation controls before matching shutdown acknowledgment
and independently observed OFF. Shared distributor and saved DRAM restoration
also pass. Evidence is under `secondary-local-probe-20261018/physical-110/`.
The runner exited and the claim closed as passed. Current ROM is `003:111`,
identity `[7,2741,1097070,0,0]`, on port `3-1`. No reconnect is needed.
Interrupt delivery, cached execution on the remaining cores, concurrent cache
coherence and FreeRTOS scheduling remain outstanding. Preserve DDR continuity;
refresh warm launcher state before the next native operation.

The complete PWM fan/LED and SHA-256/AES applications now have independent
boot-CPU profiles. Their frozen images and focused results are integrated under
`pwm-single-core-20261018/` and `crypto-single-core-20261018/`. Native source
inputs match; final PWM fixture/document revisions match the accepted follow-up
snapshot. Integration did not rebuild images or repeat checks. Physical device
operation and admission remain outstanding.

PWM and crypto nominal fixtures now share pinned provider settings and reject
missing NSI/MBUS gates or PMIC pin settings before ELF loading or instruction
execution. Six focused host cases pass under `provider-fixture-20261018/`;
all seven integrated source files match. This corrects repeated setup omissions
without changing native images or repeating their instruction checks.


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
The corrected monitor also passes the second board's 19-register PMIC probe,
including normal ROM return and automatic USB access under
`second-board-20261018-3-3/pmic-rail-probe/runtime/`. Broader cleanup behavior
remains unqualified. Frozen multicore diagnostics are unchanged.

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

The five-sensor thermal application now has an integrated boot-CPU-only profile.
Thirteen actual startup/IRQ/provider cases and six host cases pass under
`thermal-single-core-20261018/`; focused Astra review is accepted. All 333
repository and 243 external frozen inputs match integration. Calibration,
thresholds and repeated acquisition code are unchanged. Physical acquisition,
interrupt delivery and accuracy remain outstanding.

The thermal driver now accepts normalized 24 MHz references from 19.2, 24 and
26 MHz oscillators. Cold setup selects normalized SYS24 when raw DCXO is not
24 MHz; adoption rejects an incorrect active source. One focused ASan/UBSan
check and Astra review pass under `thermal-clock-reference-20261018/`. All 338
repository and 243 external frozen inputs match integration. Relevant image
layout is unchanged, so the accepted startup and host cases were reused.
The second board reports APB1 `0x03000005` (100 MHz) and GPADC clock
`0x80000000` (enabled normalized SYS24) under
`thermal-clock-reference-20261018/clock-observation-101/`. Physical supply,
ownership, entry and recovery admission remain required before acquisition.

The complete board application now has a boot-CPU-only profile with actual USB,
radio, crypto entropy and network providers. All four workload selectors pass
startup instruction checks. Fifteen startup cases, nine host cases and one
native identity-binding check pass under `board-single-core-20261018/`; focused
Astra review is accepted. Integration matches all 325 repository and 244 external
frozen inputs without replaying tests. Both host profiles now bind sessions to
the linked native build identity. The affected RTC callback retains function
defaults, and its existing frozen bundle validates. Physical USB/radio execution,
entropy qualification and topology/power admission remain outstanding.

CPU task reclamation and repeated GPU compute submission are integrated.
One finite QEMU case executes actual FreeRTOS/newlib cleanup after task migration
in both directions. Private stream closure, heap restoration and reuse of static
task storage pass. All 26 preserved source inputs match integration under
`smp-runtime-cleanup-20261018/`; physical scheduling remains unresolved.

The GPU application executes two successive submissions of its fixed arithmetic
workload and copies each acquired result. The service preserves command queues,
advances completion counters and refuses output after cancellation or fault.
Five focused composition cases and host decoding checks pass; GPU numerical
writes remain modeled. The native compute image builds under
`gpu-completion-20261018/`. The supervised host now selects its protocol from the
frozen manifest and verifies both submissions before saving the complete output.
Thirty-three focused host cases pass under `gpu-physical-proposal-20261018/`;
the existing ELF was reused. This image genuinely depends on the eight-core boot
barrier. Physical rendering and computation remain unqualified.

NPU registration now accepts three reviewed compiled convolution artifacts,
selected before scheduling and fixed for the context lifetime. Two additional
model shapes match CPU references in the external simulator. Native construction,
two successive requests, result copies and matching completion release pass
focused modeled checks under `npu-models-20261018/`. First-result mismatch and
wrong completion tickets prevent success publication. The built image is for
software validation only; physical inference and memory confinement remain
unqualified. Arbitrary models, runtime model switching and mimus acceleration
remain required follow-up work. The NPU controller requires the supported
MMU control value before submit, completion and close. Ninety-six focused
single-bit corruption cases and native compilation pass under
`npu-first-physical-20261018/`. This refusal check alone does not establish effective
engine confinement. Static review of the pinned revision-9.2 simulator now traces
external descriptor, coefficient, input and output accesses through the existing
page-directory MMU. Exact native configuration excludes the observed flat mode;
page exception bits select fault retention. The reviewed method for the trusted
first model is this private MMU plus exclusive internal SRAM ownership. Physical
translation and actual platform admission remain unqualified; full coefficient
decoding is not a prerequisite for that bounded method. See
[native address routing](npu-address-path.md).

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
Both A733 boards have working reconnect permissions and separate port locks and
queues. The failed full eight-core attempt on `003:103` has been recovered;
its process exited and its claim closed after the user reconnected the board.
The current board state appears below. Earlier core-1 Nonsecure publication,
cached EL1 entry, SMC return, restoration and matching shutdown/OFF remain valid
for their recorded scope.
The second is on `3-3`, instance `003:101` after normal GPU-supply-probe return, and
responds as A733. Its user-reported 32 GB microSD remains untouched. DDR
initialization, two 4 KiB page comparisons and a returning SRAM platform survey pass under `second-board-20261018-3-3/platform-attempt1/`. Firmware reports
12 GiB; full capacity and device admission remain unqualified. Multicore work
belongs directly to the primary agent, without delegation.

- Startup diagnosis: core 1 accepts native RELEASE, completes CPU/DSU
  preparation and executes the native walk-blocking helper. SCTLR_EL1 changes
  from `0x00c50838` to `0x00c50839`; TCR_EL1 changes from zero to `0x00800080`.
  The target reads its frame TYPER as `0x0000010000000121` and PWRR as `0x100`.
  Both controls restore exactly before matching shutdown acknowledgment and
  observed PPU OFF. Shared state and saved DRAM restore; ROM returns as `003:090`.
  The runner exited and its claim closed. Evidence is under
  `native-target-walks-frame-20261018/native/runtime/`. Three focused software
  cases cover the extended path and retained read/restoration failures.
  The GIC-600 power handshake, wake/sleep and full local initialization now pass
  physically. A returned diagnostic exposed an invalid full-width IGROUPR0
  equality; checking required SGI/timer bits fixes it. Current evidence is under
  `native-local-group-mask-20261018/`. Interrupt delivery, cache coherence,
  cached execution on the other secondary cores and scheduling remain unresolved.
- Bluetooth: authenticated finite headset capture, 8-to-16 kHz conversion and
  native mimus transcription are integrated. Actual host
  recognition matches the reference for the same converted audio, and cancellation
  prevents copied publication under `bluetooth-mimus-native-20261017/`.
  The recognition-enabled image now includes retained startup inputs; one focused
  model-configuration handoff case passes. Its 210 compiled repository inputs
  matched at the earlier integration under `board-session-seal-mimus-20261018/`. The case models
  the recognizer; earlier audio and inference results were reused. Five focused
  cases now execute recognition copy, the serializer and TinyUSB
  using the preserved image and prior real transcript. Complete copied output,
  repeated requests under backpressure, malformed requests, provider/deadline
  refusal and stop during copying pass under `bluetooth-usb-native-20261018/`.
  Physical headset operation, receive scheduling and execution within the
  supervised window remain unqualified. Whole-image startup reconciliation is
  separate from the accepted unchanged Bluetooth/USB source inputs.
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
The boot-CPU-only RTC service is now integrated as well. Twelve native startup
and five host cases pass under `rtc-single-core-20261018/`; all 182 compiled
repository inputs and 583 frozen inputs match integration. The frozen image and
checks were reused. Its two clients, calendar operations and alarm service no
longer depend on secondary startup. Physical launch remains pending actual
board admission, with resources retained until whole-board power removal.
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

The original board on `3-1` returned successfully as `003:111`, identity
`[7,2741,1097070,0,0]`, after sequential production CPU preparation and local
GIC initialization on all seven secondaries. Every target completed all eleven
controller writes and seven cleanup writes. Target controls, local state,
shared distributor and saved DRAM restored. All 45 host transactions succeeded;
the runner exited and its claim closed as passed. Evidence is under
`secondary-local-probe-20261018/physical-110/runtime/`.

The preceding primary-preparation diagnostic returned as `003:109`.
Its report confirms completed preparation and exact EL3/EL1 control restoration;
all saved DRAM was restored, ROM responded, and the runner exited with its claim
closed as passed. Evidence is under
`primary-publication-probe-20261018/physical-108/runtime/`.

The user-confirmed reconnect before that run established cold instance `003:108`.
Fresh DDR initialization, returning platform survey and all-seven-secondary-OFF
observations passed under `reconnect-20261018-108/`. The preceding unobserved full
launch on `003:103` remains a failed attempt with unknown failure location.
Its retained child and coordinator were stopped after confirmed recovery, and
its claim was closed. Do not repeat that full launch unchanged. Preserve current
DDR continuity and refresh the warm launcher state before another native image.
Earlier core-1 local initialization, cached entry, SMC and restoration evidence
remains valid for its recorded scope; full multicore operation is unresolved.

The second board on `3-3`, instance
`003:101`, has successful DDR, returning survey, SD initial-register and PMIC
evidence. Its warm launcher survey and cache-off preparation also pass under
`warm-093-preparation/`; DMA0 remains reset with both bus gates masked. Preserve
its recorded DDR continuity without repeating training. The subsequent 20-register
PMIC probe passes under `gpu-supply-probe-20261018/runtime/`, including the GPU
supply selector. Refresh the current launcher state before another native image.
Both have reconnect permissions. The common access helper selects each port
through `SUNSTONE_USB_PORT`; locks, queues and failure records are independent.
Queue assignments are in [the board-test procedure](board-test-queue.md).

The first DDR preparation after reconnecting the original board refused ambiguous
two-device discovery before any USB command. Explicit port selection resolved
that host-side refusal. Fresh DDR, the returning platform survey and all-seven-OFF
observations then passed under `reconnect-20261018-086/`.

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

Core 1 powers on, reaches native entry at EL3, accepts RELEASE and completes
CPU/DSU preparation. It identifies as Cortex-A55 r2p0 with MPIDR `0x81000100`.
Its observed SCTLR_EL1 is `0x00c50838` and TCR_EL1 is zero. No workaround writes
are required. The preparation diagnostic stopped before target translation-control writes and
redistributor access. Matching shutdown acknowledgment, actual PPU OFF, shared
control restoration and DRAM restoration all pass. All 50 host transactions
succeeded. The runner exited, its queue claim closed, and ROM returned as
`003:089`. Evidence is under `reconnect-20261018-086/native/runtime/`.

Earlier attempts that scanned or configured interrupt redistributors returned no
report. Their backups remain under `startup-stage-20261017-083/native/runtime/`
and `reconnect-20261018-084/native/runtime/`. User-confirmed power reconnections
closed those retained sessions. Their failure locations remain unknown.

The installed session rules grant reconnect access on ports `3-1` and `3-3` until
host reboot. Use `tools/usb-access.py` to inspect or disable it. The user reconnected
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

Second-board DDR/survey proposals are prepared offline under
`second-board-20261018-3-3/preparation/`. The reviewed recovery
plan remains preserved. Both phases passed on that board with ROM responsive
and survey SRAM restored; their queue claims are closed. No SD command has run.
The SD host requires both native reads as well as USB success. Read-only admission
also permits one bounded CMD0 initialization without claiming cold card history;
twelve focused policy cases pass and the native image was reused.

The second-board register batch under `second-board-20261018-3-3/sd-initial-state/`
completed with the SD clock gated, reset asserted, and IRQ 193 disabled, inactive
and not pending. PF mux `0x0f3f3f33` assigns four pins to JTAG and disables the
remaining three. The integrated read-only path now admits that exact pattern
with explicit JTAG/debug-client exclusion, then uses the existing banked pin
handoff. Focused provider, five host-policy cases and native session parsing pass.
Only three affected objects were rebuilt; 163 original objects were reused.
Evidence is under `second-board-20261018-3-3/sd-rom-pins/`.

The board reports adaptive PF voltage mode with sensed and selected 3.3 V.
Adaptive mode is now admitted alongside the manual modes when sensed and
selected voltage remain 3.3 V. Exact mode, rail stability and writer exclusion
remain required; no voltage configuration is written. Four focused provider
cases pass, including retained failure on mode, status or selector changes.
Only the platform and build identity were rebuilt; 164 objects were reused.
Read-only batches now capture the exact PF pull/drive fields, oscillator and
fabric clock controls. All seven secondary power-state registers report OFF.
The integrated SD, USB and PMIC providers now decode the observed 26 MHz
oscillator and normalized 24 MHz reference. PMIC timing uses its selected parent
and retains the admitted raw oscillator rate. USB accepts a normalized 24 MHz
route and refuses raw 26 MHz. Focused checks and review pass; all fourteen
changed source snapshots match integration under `clock-reference/` in the
second-board evidence directory. The final image must rebuild affected providers
and their callers because PMIC and USB internal layouts changed.

A conditional PMIC clock read confirms APBS0 source 0, APBS1 source 4,
controller gate/reset value `0x10001` and divider `0x28`. The divider read ran
only after the clock and reset checks passed, under the same board lock.
Every read completed, the process exited and the queue claim closed. No I2C
transaction or register write ran. Evidence is under
`second-board-20261018-3-3/sd-pin-clock-state/`, `sd-peri0-state/` and
`pmic-clock-state/`.

The final read-only batch records all fourteen secondary CPU control/mode words
as zero and the inherited PMIC pin fields. USB0 PHY/reset and device gate remain
enabled by ROM; USB1 is disabled and the shared reference already selects
normalized 24 MHz. Evidence is under `native-handoff-state/`. The integrated
USB provider has an explicit ROM takeover for the supervised single-core SD
image. Focused software checks pass; physical takeover remains unqualified.

The MMC controller now waits one millisecond after clock activation before CMD0
and two milliseconds after CMD0 completion. Six focused sanitizer cases and
review pass under `cmd0-pacing/`; the waits retain the original ownership and
deadline checks. No card command has run physically.

Independent single-core SD/USB startup is built and reviewed.
It seals session inputs outside EL1-cleared BSS and checks secondary OFF state
in EL3 before shared hardware initialization. Twelve startup cases, two native
composition cases and producer/host acceptance pass under
`sd-single-core-20261018/`. Source-backed supply settling and pin settings
are documented under `single-core-admission/`; final ownership, image-bound
admission and actual operations remain required. Full all-core support remains
required independently of this storage image.

The second board's fixed ROM and nonsecure ROM capture matches the inspected
reference byte for byte. Its selector confirms USB0 programmed-I/O operation.
Scratch SRAM and DDR restore exactly under `rom-usb-capture/runtime/`.
This establishes the inspected USB PIO predecessor for the recorded lifetime;
external DMA handoff remains separate. The expanded PMIC probe passes all 19
reads, including enabled DCDC9 at a programmed 1,240 mV and ELDO6 at 800 mV.
The returning monitor reports status zero and ROM returns with access as
`003:093`. Its process exited and queue claim closed. These programmed settings
support USB supply admission; they are not measured voltages. No storage command
has run. The warm survey observed instruction caching enabled after ROM return.
The unchanged cache-off helper and a follow-up survey pass, with scratch restored
and every launcher predicate satisfied under `warm-093-preparation/`.

DMA0 has BGR `0` and MBUS gate word `4`: its reset is asserted and both of its
clocks are disabled. The read batch therefore skipped its gated register bank
and restored scratch. The source review accepts this maintained reset state as
current DMA0 inactivity under excluded writers. It does not claim completion of
every historical interconnect write. No outstanding aborted DMA transfer was
identified for the trusted SD/PIO launch, and no DMA clock adoption is required.
The actual SD launch bundle remains under preparation.

GPU and NPU parent-clock providers now decode the normalized 24 MHz reference
from supported oscillator inputs. Three focused provider groups pass under
`accelerator-clock-reference-20261018/`; physical accelerator operation remains
unqualified. Integration preserves the accepted SD image: its differing GPU
resource source affects clock validation, which the SD rail-reservation path
does not call. Input comparisons are under
`progress-usb-clock-kernel-20261018/`.

## Next actions

The updated complete supervised eight-core image is frozen under
`smp-ns-publication-20261018/`: ELF SHA-256
`762237cd0f023a2c6895167b347352bb383e1012b5c39d1f8f0139278a68dba3`.
It includes required-interrupt-mask verification and Nonsecure code/data/table
publication before secondary release. Four routine cases and one actual-boot
case pass; the snapshot records 89 compiler inputs and 34 linked objects. Reuse
these results. The final caller order blocks EL1 walks before temporary EL3
mapping; unchanged routine evidence is explicitly reconciled in
`publication-reuse.json`. One further instruction-model case reaches cached EL1 on all eight cores.
The prepared full image was then launched once on the original board; USB
disappeared and no native observer result arrived. Do not repeat it unchanged.
The returning primary-setup diagnostic now passes physically on that complete
image. Its unchanged production preparation function completes before the return
checkpoint; the checked EL3/EL1 controls and saved DRAM restore, and ROM returns
as `003:109`. Two instruction cases and five host cases cover the overlay,
restoration and retained failure. Evidence is under
`primary-publication-probe-20261018/`. Host transport uses the existing runner;
only its `run` function changed, adding the new profile while retaining common
queue, lock, transfer and failure handling. Successful cases were reused after a
fixture command-log repair. The next CPU observation must address later startup;
primary preparation success on this recovered run does not locate the earlier
unobserved full-image failure. Do not reuse old fixed overlay addresses.
The second board was not accessed.

A returning sequential survey for cores 2–7 is prepared under
`secondary-identity-probe-20261018/`. It calls the complete image's unchanged
power and entry controllers, records each core's identity and reset controls,
and requires matching shutdown acknowledgment plus observed OFF before restoring
its configuration. Three instruction cases and five host cases pass. The primary
preparation prefix was reused for the two failure cases. The first board's warm
survey, unchanged cache-off helper and follow-up survey pass on `003:109`;
DDR continuity is preserved. The sequential survey then passed physically on
all six targets and returned as `003:110`. Cores 2–5 identify as A55 r2p0;
cores 6–7 identify as A76 r4p1. The A76 targets reported different nonzero
TCR_EL1 values, reinforcing the requirement to preserve each target's own
controls. All six PPU observations had OFF in the low state bits. The runner
exited and its claim closed. No reconnect is needed.

Production CPU preparation and local initialization now also pass on all seven
secondaries under `secondary-local-probe-20261018/physical-110/`. Next extend
the core-1 cached-entry result to the remaining cores, then establish concurrent
coherence and delivery. Do not repeat the unobserved full launch unchanged.
Interrupt delivery, mutable cache coherence and scheduling remain outstanding.

The [GPU publication audit](gpu-single-core-publication.md) supports retaining
its frozen image under the existing clean-entry contract. PMIC return101 is a
candidate preceding cache cleanup; current entry state and continuous sole-writer
history must still be bound before upload. Do not infer an additional system
cache or rebuild the GPU image merely from the separate CPU publication finding.


The accepted [goal](goal.md) prioritizes complete native device integration.
After hardware functionality, warm mimus recognition must finish within 0.05
times audio duration; 0.01 times is a stretch target for bounded investigation.
Startup and live end-of-speech latency are separate measurements. Existing
recordings and model identities remain the reference.

Core-1 release and preparation now pass physically, with successful shutdown,
restoration and USB ROM return. The interrupt driver lacked the GIC-600
redistributor power-up sequence. That sequence is implemented and passes focused
software checks; its connection to the physical failure remains a hypothesis.
The primary agent handles all further multicore work directly.

1. Continue from the successful local interrupt snapshot under
   `native-local-interrupt-read-20261018/`. Core 1's frame is asleep with no
   enabled, pending or active interrupts; translation controls restore and ROM
   returns after observed OFF. The returning overlay now compiles the corrected
   production controller and restores local state before shutdown. Five focused
   instruction cases pass under `native-local-interrupt-init-20261018/`;
   one physical attempt produced no ROM return or result buffer. Physical
   recovery is complete, but no failure location is established. The same launch
   must not repeat. The [Arm/Linux audit](a733-multicore-reference-audit.md)
   distinguishes firmware responsibilities, permitted register ordering and
   diagnostic refusals that currently produce no returned report.
   A narrower overlay now stops before the first redistributor write and verifies
   unchanged local state before shutdown. Two instruction cases pass under
   `native-before-power-20261018/`. The identical payload passes physically under
   `native-before-power-current-20261018/`, with unchanged local state and ROM
   return as `003:095`.
   The interface-only profile now stops after EL3 GIC access and group-enable
   reads, before calling the controller. Three focused instruction cases pass
   under `native-cpu-interface-20261018/`, including retained SRE and group-enable
   refusals. Compiled inspection confirms no new redistributor or wake-control
   writes in that interval. Physical execution also passes: SRE changes from
   `0x9` to `0xf`, both group-enable reads are zero, and matched shutdown/OFF,
   shared-state restoration and saved DRAM comparison succeed. ROM returns as
   `003:094`. The power-handshake profile then passes physically under
   `native-before-wake-20261018/`: PWRR remains `0x100`, WAKER remains `6`, and
   the callback refuses WAKER before writing it. Local state and wake control
   restore, matched shutdown/OFF completes, and ROM returns as `003:096`.
   Three focused instruction cases cover completion, one rejected power request
   and retained baseline mismatch. Wake and return-to-sleep subsequently pass
   under `native-wake-sleep-20261018/`: awake WAKER is `0`, restored WAKER is `6`,
   no local configuration write runs, and matching OFF/restoration/ROM return
   complete as `003:097`. Three focused cases cover success and retained wake
   and sleep timeouts. The returned configuration refusal then identified
   phase23/error4: IGROUPR0 reads `0xfff0ffff`, but the driver required all ones.
   Arm defines unimplemented interrupt bits as read-as-zero/write-ignored.
   The corrected required-SGI/timer-bit check passes full local initialization
   and restoration under `native-local-group-mask-20261018/`, with observed OFF
   and ROM return as `003:099`. The subsequent EL3 cache probe also passes
   physically under `native-el3-cache-20261018/native-attempt/runtime/`.
   It executes the production adoption helpers, restores all five saved
   controls, completes local cleanup and observed OFF, and returns as `003:100`.
   Four instruction cases cover adoption, inherited SIF and retained readback
   failures. Cached EL1 entry now passes as described below; interrupt delivery,
   remaining cores, cache coherence and scheduling still require qualification. Preserve failed-attempt backups and
   reuse accepted preparation and restoration evidence. The failed UFS probe,
   intentional watchdog expiry and unattended fault tests remain deferred;
   USB power-control work is closed.

Core 1 now passes production cached EL1 entry and SMC return after explicit
Nonsecure code/table publication. Four affected modeled cases pass, including
retention when one code alias remains stale. The physical result under
`native-ns-publication-20261018/native/runtime/` records all 32,768 code lines and
128 table lines maintained, matching changed instructions through both physical
address spaces, and exact restoration of temporary, EL1 and EL3 controls.
Local cleanup, matching shutdown/OFF and DRAM restoration pass; ROM returned
`003:103`. The process and claim closed successfully. The earlier missing-EL1
attempt remains preserved under `native-el1-cache-20261018/` and is not counted
as an EL1 success. Reuse the corrected publication for subsequent diagnostics.
Next extend the proven entry to interrupt delivery, mutable cached memory,
remaining cores and FreeRTOS scheduling. Maintenance currently admits only clean
immutable aliases; arbitrary dirty runtime data needs its own ownership/cleanup.

2. Qualify all-core startup, interrupts, scheduler results and coherent memory
   through the supervised observer before dependent full-device images.
   Preserve the sealed [watchdog-dependent observer](a733-smp-observe-host.md)
   and its existing entry requirements. USB enumeration alone does not establish
   diagnostic success.
3. The second board's single-core SD launch candidate is prepared under
   `second-board-20261018-3-3/single-core-admission/candidate-093/`. Its image
   remains valid, but the instance-bound admission needs refreshing after the
   returning GPU supply probe. Its remaining
   card-state question is whether board and card were unpowered before initial
   attachment. ROM source shows native initialization without a 1.8 V request,
   but cannot establish an unknown card's state. Await that factual answer before
   setting the native 3.3 V signaling admission. Use the integrated read-only SD application for initial card identification
   and a sector-zero read after actual controller, memory and supply admission.
   Preserve the inserted card's contents. The separate maintenance image needs
   card/range ownership, protected backups and its qualified watchdog procedure;
   it is not the first discovery path.
4. The GPU single-core compute application is integrated for validation
   independently of SMP. Twelve startup cases, nine package/admission cases and
   eight USB/profile cases pass; focused review accepted the frozen image.
   All 175 preserved source inputs match integration. Evidence is under
   `gpu-single-core-20261018/`. Preparation of the second-board launch inputs
   is in progress; no physical GPU execution result exists. A second-board
   read under `gpu-clock-read-20261018/` observes GPU clock and reset/bus words
   both zero, with scratch restoration and normal same-instance return.
   The additional DCDC4 read passes under `gpu-supply-probe-20261018/runtime/`:
   enable register `0x10` is `0xff`, voltage register `0x15` is `0x9e`, and its
   seven-bit selector 30 programs 800 mV. ROM returns as `003:101`; the process
   and claim close. The runner validates the explicit DDR-to-ROM-return chain
   against the current survey and live instance. The frozen GPU candidate is now
   reconciled to `003:101` under `gpu-single-core-20261018/physical-candidate/`.
   All requested raw clock and supply samples are present; no additional PMIC
   read or rebuild is needed. Current entry state, image-specific cache/writer
   exclusion, trusted power history and takeover/recovery authority remain
   unresolved. Successful execution of the modified PMIC probe remains positive
   second-board evidence.
   Establish actual power,
   memory and recovery admission for the integrated GPU,
   NPU, codec, RTC and thermal applications. Their internal operations can advance
   without camera, display or microphone accessories. Retained buffers do not
   establish completed device writes or permit resource reuse. The larger memory
   profile supplies capacity; physical exclusion, coherence and device mappings
   remain required. Camera capture separately requires the actual module and wiring.
The independent single-core NPU convolution image is integrated and reviewed.
Eleven startup/provider cases, seven host cases and six comparisons of unchanged
SD/GPU preprocessing pass under `npu-single-core-20261018/`. Integration matches
145 distinct repository inputs and reuses the frozen ELF and external compiled
inputs without rebuilding or replaying tests. The startup restores interrupt
masking after FreeRTOS static task construction. Physical NPU inference remains
pending actual supply, memory, DMA, USB and recovery admission. The preserved
software fixture bundle is never a deployable admission record.

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

The independent board-application worker is preparing a boot-CPU-only USB/radio
image in `sunstoneos-worktrees/board-single-core`, from `3eca977`. It owns the new
target and additive shared single-core startup branches; it has no board access.
The existing USB-only and USB-plus-radio features must remain intact. It is also
correcting a source-proven prepared-session identity mismatch, with targeted
validation and preserved historical artifacts. This work is not a diagnosis of
the full multicore attempt.
