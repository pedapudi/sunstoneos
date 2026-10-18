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

Implementation and debugging use Sol 6.1 with medium reasoning; Astra reviews
substantive correctness risks. The coordinator alone operates the board.

- Startup diagnosis: table construction, cache publication and CPU/DSU planning
  passed physically. The observed identity is supported and needs no workaround
  writes. EL1 walk blocking, exact control restoration and interrupt-controller
  reads also pass. Complete distributor setup and restoration now pass physically.
  Core 1 now powers on, reaches native entry and powers itself off. The primary
  verifies OFF and restores saved state before returning to ROM. Execution after
  RELEASE, cache coherence and scheduling remain unresolved.
- Bluetooth: authenticated finite headset capture, 8-to-16 kHz conversion and
  native mimus transcription are integrated. The image links; actual host
  recognition matches the reference for the same converted audio, and cancellation
  prevents copied publication. All 209 preserved repository inputs match
  integration. Physical headset operation, native USB serialization and execution
  within the supervised window remain unqualified. Evidence is under
  `bluetooth-mimus-native-20261017/`.
- I2S: finite capture-to-mimus is integrated for one to five seconds of 16 kHz
  mono audio. The native image links, and focused capture, admission, cancellation
  and copied-output checks pass. All 155 preserved repository inputs match
  integration. No microphone is connected; physical capture and inference timing
  remain unqualified. Evidence is under `i2s-mimus-20261017/`.
- JPEG: the nonconstant diagnostic is integrated. Its integer oracle matches
  all 2,304 CPU-decoded samples exactly; the native link and focused parser,
  copy/layout and host refusal checks pass. Physical decoding remains unqualified.
  Evidence is under `jpeg-nonconstant-20261017/`.
- H.264: the fixed Baseline IDR fixture matches its CPU reference, and selected
  A733 runtime register traces pass after a structure-field audit. Native driver,
  application and copied-output integration are in progress.
- Crypto: the native SHA-256/AES application is integrated. Its image links,
  focused application and host checks pass, and all 223 preserved repository
  inputs match integration. A physical snapshot confirms the adopted CE clocks.
  Predecessor task completion, future launch exclusion, secure status access
  and memory authority remain physical prerequisites. A boot-path audit is
  investigating the predecessor handoff. No board crypto command has executed.
- Wi-Fi networking: fixed-address and DHCP board profiles are integrated, and
  the combined board image links. Profile checks pass under ASan/UBSan. An earlier
  unexplained DNS deadline miss remains a timing limitation.


Component documents and the hardware matrix hold the remaining integrations.
The [historical handoff](handoff-history-20261017.md) preserves earlier artifact
locations and reconciliation details; it is not a current qualification record.


## Observed host and device state

The attached FEL device (`1f3a:efe8`) identifies A733. The boot CPU identifies
as Cortex-A55 r2p0. DDR firmware reports 12 GiB LPDDR5, with transfers checked
at two distinct pages. [Boot evidence](boot.md) records initialization and the
EL1 handoff. Board revision and populated storage remain unconfirmed.
No persistent-storage writes have been issued.

The current board instance is `003:083` on port `3-1`, with automatic USB access.
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
restoration and observed target OFF. The current `003:083` instance has passed
the guarded cache-off helper and returning launch-state survey; its next native
diagnostic has not run. Both preparation claims are closed.
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
- Crypto clocks: `crypto-clocks-20261018-082/session.json` records 13 read-only
  CCU/RTC observations. The CE clock, gates and reset releases are enabled;
  these observations do not prove engine quiescence or secure-register access.
- Crypto application: `crypto-native-20261018/handoff.json` identifies the
  frozen image and focused checks; `integration.json` reconciles every preserved
  repository input. The original admission-fixture sanitizer failure is retained,
  and only the corrected admission case was repeated. The modeled-authority
  package is not deployable; physical admission requires actual evidence.
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

1. Diagnose native RELEASE through secondary interrupt preparation before another
   full multicore launch. Core-1 activation, entry and target-owned shutdown now
   pass physically. Extend verified cleanup to the reached redistributor state;
   preserve the native layout and reuse completed instruction cases.
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
