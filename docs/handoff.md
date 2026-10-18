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
- GPU: the supervised image now includes the PMIC clock-parent support needed
  for the observed board configuration. One affected link and nominal offline
  bundle validation pass; all 307 preserved repository inputs match integration
  under `gpu-pmic-refresh-20261018/`. Existing GPU and PMIC checks were reused.
  The software-only bundle cannot supply physical admission, and successful
  rendering remains unqualified.
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

The [native RTC application](rtc-application.md) now connects coherent calendar
reads, one non-waking alarm, deferred interrupt acknowledgement, cancellation and
copied USB output. One affected image and focused driver, native instruction and
host checks pass. All 174 compiled repository inputs match integration under
`rtc-native-20261018/`; accepted artifact and dependency evidence was reused.
The fitted board revision, physical operation and native USB serialization remain
unqualified. Explicit calendar setting and clock-source initialization are now
in development. Their startup allowance must accommodate documented calendar
write times while preserving the other applications' existing deadlines.
Alarm wake and retention qualification remain required work.

The [native PCIe application](pcie-application.md) now connects the root-port
driver and shared providers to x1 Gen1 link observation, read-only endpoint
configuration and copied USB output. One native image, sanitizer admission,
14 modeled native cases and four host groups pass. All 174 compiled repository
inputs match integration under `pcie-application-20261018/`; controller and PHY
evidence was reused. Physical use requires an identified endpoint, verified
carrier wiring and power, and recovery for every affected power domain.
BAR allocation, interrupt-driven transfers, DMA, higher link speeds and power
transitions remain required work.

The [supervised read-only SD application](storage-read-application.md) now
identifies a card and reads sector zero through the actual native MMC providers,
then publishes a separate copied USB result. Focused model, ownership, binding
and host checks pass. A correction makes the active read deadline cut off
publication while the board session remains live; its affected case passes.
Only the application body and build ID changed during that correction; the
other 163 objects and earlier evidence were reused. Integration reconciles the
165-object image and its source inputs under `storage-read-native-20261018/`.
Physical SD operation remains unqualified. The inserted microSD supplies a
future physical target whose contents remain protected. The separate maintenance
image still requires an identified card and qualified watchdog; this read-only
path permits initial discovery without either prerequisite.

Native SPI application integration is in development. Route and population
research precedes selection of a usable board controller and transfer profile.
The application will connect the existing controller and shared providers;
physical transfers require an identified peer and verified wiring and voltage.


## Observed host and device state

The attached FEL device (`1f3a:efe8`) identifies A733. The boot CPU identifies
as Cortex-A55 r2p0. DDR firmware reports 12 GiB LPDDR5, with transfers checked
at two distinct pages. [Boot evidence](boot.md) records initialization and the
EL1 handoff. Board revision and populated storage remain unconfirmed.
No persistent-storage writes have been issued.

The last board instance is `003:083` on port `3-1`. Its diagnostic did not return
and the instance is quarantined. Runner session `81427` retains the board lock
and queue claim `5ab543c3a907541e29e1bbfe08963deef489cb022ae96b6262a867959dd8b9d3`.
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

Fresh reads before that launch established all seven secondaries OFF with clear
request bits; see `startup-stage-20261017-080/cpu-state/session.json`. The next
investigation covers RELEASE through secondary interrupt preparation, before
cache enable. Return requires interrupt quiescence, saved redistributor-state
restoration and observed target OFF. Instance `003:083` passed the guarded
cache-off helper and returning launch-state survey. Its subsequent diagnostic
attempted native RELEASE through secondary interrupt initialization. All 27
host transactions completed, including launch, but no new ROM instance appeared
within ten seconds. No result was recovered, so the failing instruction and
target state remain unknown. Memory backups and ownership remain retained;
there was no retry. Evidence is under `startup-stage-20261017-083/native/runtime/`.
The user may be away for several hours; continue bounded returning diagnostics
before another full-image attempt.

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
  and capture-fault retention. Its physical use awaits recovery.
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

1. Recover the board before the reviewed pre-write failure observer. Core-1
   activation, entry and shutdown before RELEASE pass physically. The later
   diagnostic has no physical result; the new observer captures selected target
   admission failures and primary snapshot exceptions before interrupt writes.
   It still retains post-write failures and cannot recover a stalled MMIO access.
   Preserve the held claim and backups until physical recovery is confirmed.
   A power reconnect invalidates the previous RAM state. Reinitialize DDR using
   the reviewed cold-start procedure, then obtain fresh launch-state evidence
   and admission before running the observer.
   The failed UFS probe, intentional watchdog expiry and unattended fault tests
   remain deferred. USB power-control investigation is closed.
2. Qualify the integrated maintenance image under its reviewed recovery procedure.
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
