# Current work

The user will disconnect board two and cannot reconnect either board until
further notice. Both queues have no active physical tests. Continue offline
implementation, instruction checks and integration; defer new hardware launches
that could require physical recovery. Do not request another reconnect meanwhile.

The user confirms that neither board has populated eMMC or UFS. These absent
devices need no population probe or media qualification on the current boards.
Preserve both microSD cards; their native storage support remains required.

All eight CPUs physically reach their first FreeRTOS task context and return
safely. The returning diagnostic executes actual task construction and initial
context restoration, then records the idle-task pointer, stack, argument and
exception state on each core. All cores finish before cleaning the complete
145-page writable interval, or 9,280 cache lines per CPU. CPU0 restores its
controls; all seven secondaries acknowledge shutdown and reach independently
observed OFF. Saved DRAM restoration and responsive ROM return pass. Evidence:
`all-core-first-task-20261018/physical-010/runtime/session.json`.

The instruction checks, nine host cases and root review also pass. Task bodies,
timer interrupts and application SGIs are excluded from this physical diagnostic.
Sustained multicore scheduling remains physically unqualified. The existing
production barrier and 256 atomic increments also
pass in this physical attempt. Reuse these results without repeating the launch.

An offline extension now passes actual idle-task yield, FreeRTOS context
save/selection/restoration and pinned-worker entry on all eight cores. Complete
owned-memory cleaning and matched return pass; a wrong worker argument retains
all cores before cleanup. Evidence: `all-core-task-switch-20261018/`, accepted
bundle `build-no-observer/`. The frozen image's higher-priority USB observer is
suppressed in this CPU-only diagnostic. Worker bodies and timer/SGI workloads
remain excluded. Nine host cases and root source review now pass. The runner
validates worker contexts, return frames and memory bounds, restores saved RAM
on completed return, and retains transport failures. Evidence: `host/software.json`
and `host-root-review.json` in the same directory. Physical use still requires
fresh board admission and available manual recovery; no test is queued.

The complete returning CPU workload now passes instruction checks. Every core
finishes 32 floating-point rounds, runs its background task and receives SGIs;
CPU0 also services the production timer. All writers stop before interrupt
cleanup, memory publication and matched shutdown. A damaged completed-round
count retains the session before cleanup. That refusal check reuses the saved
completed-workload checkpoint without repeating the worker execution. Sources,
image and results are preserved in `all-core-task-workload-20261019/frozen/`.
Timing and cache coherence remain modeled. Host validation now passes thirteen
focused cases, including copied workload results, arrival epochs, task contexts,
timer-priority restoration, saved-RAM restoration and failure retention.
Evidence: `host-frozen/` and `host-root-review.json` in the same directory.
Physical qualification remains outstanding; no launch is admitted.

Native DSI scanout binding is integrated. It connects the media/PLL providers,
retained memory allocations, actual IOMMU tables and ordered controller accesses.
Focused composed checks, AArch64 compilation and source review pass under
`display-native-20261018/`; accepted evidence was reused during integration.
Panel selection, analog-profile qualification and initial isolation authority
remain outstanding. STOP retains resources because display-engine drain is
unsupported. No standalone image or physical scanout is qualified.

The finite UART2 header application is integrated. Actual clock/pin ownership,
FIFO exchange, bounded drain and release produce copied USB results. Selected
sanitizer, session and host checks pass; the native image links. All 419 compiled
inputs and 195 preserved repository sources match during integration.
Evidence: `uart-native-20261018/frozen/manifest.json` and `integration.json`.
Physical loopback, electrical admission, recovery and inherited multicore startup
remain outstanding.

UART2 interrupt service is integrated. The handler masks the owned source and
notifies its owner task; the task services receive/transmit causes before rearming.
Success requires both receive and transmit interrupt evidence and completed
drain. Focused sanitizer and host checks, eleven linked routing/dispatch checks,
and source review pass. Integration matches all 423 compiled inputs and 202
preserved repository sources. Evidence: `uart-irq-20261019/frozen/manifest.json`,
`root/review.json` and `root-integration.json`. Accepted checks were reused.
Physical delivery, serial operation and inherited multicore startup remain
unqualified. DMA and broader serial modes remain required work.

Simultaneous I2S capture/playback is integrated with separate DMA channels and
period catalogues, shared clock/pin lifetime and failure propagation to both
directions. Success requires both transfer counts and both drained results.
Native client/pool/USB checks, composed stream checks and host capture pass;
focused source review is complete. Evidence: `i2s-duplex-20261018/snapshot/` and
`integration.json`. Physical audio and wire-level drain remain unqualified.

Native read-only eMMC access is integrated through the existing copied-client
service. Both SDC2 and SDC3 selections pass instruction checks for identification,
EXT_CSD and copied reads. Admission/capture checks and source review pass under
`emmc-native-20261018/`. Both current boards lack eMMC, so this optional path has
no physical qualification target. Its single-core image retains resources
permanently and has no qualified ROM return.

H.264 header parsing and register planning now accept bounded Baseline IDR streams
with prediction and residual coding. Four independent x264/FFmpeg comparisons,
parser sanitizer checks and native application checks pass. The driver accepts
16–64 by 16–32 pictures. The application now accepts a supplied 48-by-32 stream
without rebuilding firmware, with an optional independent pixel reference.
Upload, session admission, copied output and host capture checks pass under
`h264-input-20261018/`; root integration matches the frozen image and 422 compiled
inputs apart from one documented comment-only header change. Each retained
session supports one frame of at most 4096 input bytes. Broader formats, repeated
decoding and physical pixel/confinement/drain qualification remain outstanding.
Parser evidence under `h264-stream-20261018/final-frozen/` was reused.

JPEG now accepts a supplied baseline YCbCr420 image and returns copied pixels
with parsed dimensions. Complete input authentication precedes decoder resource
acquisition; an independent pixel reference is optional. Geometry, upload,
native copy, session and host checks pass under `jpeg-runtime-input-20261018/`.
Integration preserves the accepted image and 424 compiled inputs; the only
current-source difference is an unrelated H.264 API comment. Physical pixel
correctness and confinement remain unqualified.

Crypto now accepts bounded SHA-256 and AES-128 ECB requests with private copied
input, deferred engine construction and request-bound output. Native, sanitizer,
admission and host checks pass under `crypto-runtime-input-20261018/`; all 414
compiled inputs match integration. Review resolved key-cleanup races and bounded
handling of empty USB fragments. Physical engine operation, secure access and
drain remain unqualified. Both integrations reused accepted checks.

Finite thermal, ADC and USB applications are integrated, together with healthy
GPU CLOSE and terminal NPU CLOSE. Their focused checks and Astra reviews pass;
compiled-input comparisons reuse those results without repeating them. Evidence:
`thermal-adc-returning-source-20261018/integration.json`,
`usb-returning-20261018/integration.json`,
`npu-supply-returning-20261018/integration.json` and
`gpu-returning-integration-20261018/frozen/integration.json`.
GPU and NPU finite adapters are integrated. Both stop their workers and park
PMIC outside active requests before the final task critical section. The NPU
returning composition is now integrated, with 26 new return checks, three
session-retention checks and a check of the linked task privilege state.
Evidence: `npu-finite-return-20261018/frozen/`. Its SRE tasks run at EL1t;
the cached multicore image's EL0 exit rules do not apply. Its returned-RAM host
runner is integrated, with validated copied outputs before full DRAM restoration
and retained ownership after uncertain completion. Its shared transport helper
uses a per-port lock and forbids further requests after a transport failure.
Host evidence is under `npu-finite-host-20261018/transport-frozen/`. Final root
physical admission remains outstanding. Root host source review is complete in
`npu-finite-host-20261018/root-host-source-review.json`; accepted checks were
reused. The sole changed host dependency adds an unrelated kernel observer;
the platform-admission function used by NPU is unchanged. Frozen compiler inputs
remain authoritative: the current USB header changes an embedded structure's
layout, even though this NPU image does not execute USB platform functions.

The corrected GPU returning composition is integrated. It publishes private and
device data through Nonsecure EL1 mappings before SMC, then completes final stack
cleanup before reset. Root source review and 25 selective modeled cases pass;
the device-specific Astra review and frozen source/object audit also pass.
Evidence: `gpu-returning-application-20261019/ns-publication/`. Earlier EL3-only
publication results are superseded. The GPU host adapter is integrated and
uses the same bounded transport as NPU. Its 93 focused checks and Astra review
pass, including copied-result validation and bounded memory restoration.
Evidence: `gpu-returning-host-20261019/`. Physical inference and ROM return remain
unqualified for both accelerators. Existing device checks were reused.
Root GPU host source review also passes; all 18 host sources match the preserved
bundle. Its record is `gpu-returning-host-20261019/root-host-source-review.json`.
The [publication audit](a733-multicore-reference-audit.md#publication-must-cover-the-executable-physical-address-space)
and device skill document this requirement. Root retains CPU and architectural
return review. For the preceding adapter, all 172 NPU
compiler inputs match. The 89 new GPU compiler inputs match; two inherited
monitor/runtime sources have additional inactive branches, so the accepted GPU
objects and their frozen inputs remain authoritative. Evidence:
`npu-finite-quiescence-20261018/integration.json` and
`gpu-finite-adapter-20261018/layout-correction/integration.json`. Thermal and ADC retain configuration and leases;
ADC additionally requires electrical admission for the fitted input dividers.
Separate RTC source initialization and external-source alarm applications are
also integrated. Twelve source cases and seventeen alarm cases pass, with
35 and 33 matching compiled inputs respectively. Evidence is in
`rtc-source-returning-20261018/integration.json` and
`rtc-alarm-returning-20261018/integration.json`. Source initialization needs its
own physical admission before alarm testing. Alarm success additionally requires
verified Secure interrupt-state restoration; a completion message is insufficient.

## Observed host and device state

No board access is authorized while physical recovery is unavailable. Both queues
were idle at the last observation; no live query has been made since the user
announced the second board would be disconnected. Historical USB instance numbers
must not be reused as current identity or admission.

The original board uses host USB port `3-1` and a separate 5 V/3 A supply.
USB data-cable removal alone may not remove its power. Its accepted eight-core
first-task result is identified above. Full workload execution remains deferred
until physical recovery is available and the primary agent refreshes admission.

The second board uses port `3-3`; USB was its only power source. The last confirmed
power removal recovered instance `003:073`, identity `[7,3858,1097032,0,0]`.
ROM version and access passed, retained processes stopped, and the failed queue
claim closed. Evidence: `usb-first-fault-20261018/root-physical-064/recovery-073.json`.
That recovery predates the planned disconnection. Fresh cold DDR and platform
admission are required when the board becomes available again.

The preceding USB takeover attempt returned to ROM with native status 1 before
any USB-controller write. The guard rejected an inherited device-session bit at
`0x04100040`. The integrated correction permits that bit while requiring device
role, excluding host request/mode and retaining DMA checks. Affected checks and
review pass under `usb-rom-session-20261018/`; physical takeover remains unqualified.
Failure evidence is under `usb-first-fault-20261018/physical-064/runtime/`.

Per-port permissions, locks, queues and failure records remain independent.
Reconnect access rules last covered ports `3-1` and `3-3` until host reboot.
Future access must verify their state and use explicit port selection. Preserve
both microSD cards. No MIPI camera, display or microphone is attached; OV5640 is
the first sensor target and the DSI panel is undecided. The separate ESP32 mimus
keyboard is unrelated; do not use it as the Orange Pi console or send desktop
keyboard events.

## Current integration

The [hardware matrix](hardware.md) records each device's implemented functions,
software evidence and remaining physical qualification. Its component links
identify the native APIs, dependencies and evidence for paths not summarized above.
The [historical handoff](handoff-history-20261019.md) preserves earlier integration,
recovery and validation records; it is not current launch authority.

## Reusable validation evidence

Raw evidence lives under `~/.cache/sunstoneos/evidence/`; paths above are relative
to that directory. Reuse accepted checks while their relevant inputs match.
Compiled-input audits establish applicability of prior checks, not physical
operation. Do not rebuild or rerun merely for a commit or handoff update.
The [reference index](reference-index.md) locates cached primary sources, and
[validation.md](validation.md) defines selective checking and fixture requirements.

## Next actions

Continue offline while the user cannot reconnect either board. No hardware
launch is admitted. Earlier instance identifiers and preparation records are
historical evidence; they do not authorize a new session.

1. Complete repeated I2S streaming in `i2s-cyclic`, including copied-period
   ownership, USB completion before acknowledgment, cancellation and retained
   backlog. Develop native USB host interrupt transfers and copied HID reports
   in `usb-host-interrupt`, beginning with the pinned xHCI/TinyUSB requirements.
   Coordinate shared-provider edits before implementation. CPU/startup changes
   remain root-owned. UART interrupt service and H.264, JPEG and crypto runtime
   input delivery are integrated.
   Implement the approved trusted GPU compiled-program profile in
   `gpu-program-submission`, using pinned Mesa metadata, copied input/output and
   the existing native service. Preserve the frozen fixed-workload image.
   Broader workgroup geometry, graphics and concurrency remain required.
2. Integrate reviewed commits and frozen evidence promptly. Reuse successful
   checks when their relevant inputs match. Keep shared drivers and startup
   outside these workers' scope unless a concrete dependency requires a
   coordinated change. Update the hardware matrix and existing issues.
3. When physical recovery becomes available, the primary agent must refresh
   board identity and admission before selecting one bounded CPU test. All eight
   initial task contexts already pass physically. The full returning workload
   passes instruction and host checks under `all-core-task-workload-20261019/`;
   actual scheduling, floating-point work, timers and inter-core interrupts
   together remain physically unqualified. Preserve the reviewed final-writer
   barrier, Nonsecure memory publication and matched shutdown. Do not repeat
   the earlier unobserved full launch unchanged.
4. Resume independent device qualification only after board-specific preparation.
   The second board needs fresh cold DDR and platform admission after its
   disconnection. USB takeover has a source-backed device-session correction;
   GPU and NPU have reviewed returning images and host capture. Their physical
   operations remain unqualified. Refresh each device's clock, power, memory,
   DMA and recovery prerequisites; software fixtures are not physical authority.
5. Complete the remaining functions in the [hardware matrix](hardware.md),
   including operational modes beyond the bounded applications. Preserve both
   cards and require established recovery before persistent writes. Keep the
   failed UFS probe and intentional watchdog expiry deferred. No MIPI camera,
   display or microphone is attached; OV5640 is the first sensor, the DSI panel
   remains undecided, and recorded audio supplies mimus input.
6. Run unchanged-model mimus natively and qualify representative concurrent
   workloads. After every required hardware function works, optimize complete
   recognition to at most 0.05 times audio duration and investigate 0.01 times
   as a bounded stretch target. Report loading and live latency separately.
   Complete reproducible boot images, recovery/update procedures and physical
   qualification before declaring the [goal](goal.md) achieved.

The primary agent retains all multicore implementation, review and board tests.
The [Arm/Linux audit](a733-multicore-reference-audit.md) and
[startup record](a733-smp-supervised.md) hold the source comparisons. The accepted
returning CPU diagnostics use frozen ELF
`762237cd0f023a2c6895167b347352bb383e1012b5c39d1f8f0139278a68dba3`;
rebuilding it requires new symbol and patch bindings. Preserve the accepted
artifacts instead of rebuilding them for handoff or documentation changes.
