# Current work

The user will disconnect board two and cannot reconnect either board until
further notice. Both queues have no active physical tests. Continue offline
implementation, instruction checks and integration; defer new hardware launches
that could require physical recovery. Do not request another reconnect meanwhile.

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
`emmc-native-20261018/`. Population and the fitted route remain unknown. The
single-core image retains resources permanently; it has no qualified ROM return.

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

The second board was recovered as `003:073`, identity
`[7,3858,1097032,0,0]`, after the user removed and reconnected its sole USB power
source. ROM version and access pass. The retained coordinator, wrapper and runner
are stopped, the failed queue claim is closed, and the queue halt is cleared.
Evidence: `usb-first-fault-20261018/root-physical-064/recovery-073.json`.
This recovery precedes the user's planned disconnection. Fresh cold DDR and
platform admission will be required when the board is available again.

The preceding USB attempt returned to ROM as `003:071` with native status 1.
Its first-fault report identifies a value mismatch at `0x04100040`: mask `0x700`,
expected zero, observed `0x29940`. The USB provider reports zero attempted and
completed writes. All four PMIC snapshots matched and were reaped; no native USB
exchange began. Source investigation identifies an inherited device-session bit
rejected by the earlier guard. The integrated correction permits that bit,
requires the ROM device role and rejects host request/mode. Independent DMA
guards remain. Affected checks and focused review pass; the changed sources
match integration under `usb-rom-session-20261018/`. Physical takeover remains
unqualified. No unchanged retry is authorized. Failure evidence:
`usb-first-fault-20261018/physical-064/runtime/session.json` and `kernel.log`.
Saved RAM was not restored across power removal.

Before this attempt, independent cold DDR initialization and two 4 KiB page
comparisons passed on instance064. Its returning SRAM survey passed every
launcher prerequisite and restored scratch memory. Fresh CCU/reference and
USB-selector/DMA0 captures passed. Evidence: `second-board-cold-064-20261018/`
and `usb-first-fault-20261018/{fabric-064,entry-064}/`. The configured AHB/NSI/MBUS
rates were 200/600/600 MHz; these are register-derived rates, not measurements.
This establishes initialization and sampled RAM access, not complete capacity
or stability. The subsequent failed attempt does not permit reuse of this state.

The USB BUSY correction and first-fault reporting are integrated. Thirteen
changed-path cases and focused review pass. All 65 new compiler inputs and 31
reused objects match their recorded inputs; all four consumers of the enlarged
USB structures were rebuilt. Existing native and host suites were reused.
Evidence: `usb-first-fault-20261018/integration.json`. The preceding instance063
failure had no first-fault report, so its precise cause remains unknown.

The scheduler privilege audit identified an EL0 lock-wait defect. FreeRTOS's
contended recursive lock executes WFE, but cached startup left SCTLR_EL1.nTWE
clear. A blocking wait could therefore enter the synchronous exception handler,
which permanently aborts for non-SVC exceptions. Startup now permits EL0 WFE.
The instruction check rejects the earlier image at the lock wait; the corrected
image completes the eight-core workload and observes permitted waits on every
core. Evidence: `scheduler-el0-access-20261018/`; details and Arm source are in
the [reference audit](a733-multicore-reference-audit.md#el0-lock-waits-require-permission-to-execute-wfe).
This is a source-backed correction, not a physical failure diagnosis. No board
operation accompanied it. Physical first-task entry now passes with the established return path. Task
switching and sustained workloads remain separate qualification requirements.
The original task-entry trace is preserved under
`scheduler-el0-access-20261018/first-task-entry.json`. Its observed stores covered
88 pages. The returning diagnostic covers the complete linker-owned normal
writable interval `0x41200000..0x41291000` (145 pages), including task creation
and context restoration. Monitor and table reservations are outside that interval.

The first board last returned responsive on port `3-1` as `003:067`, identity
`[7,3800,1097026,0,0]`. Its first-task runner exited and its queue claim closed as
passed. No reconnect is needed. The user reports a 5 V / 3 A supply; recovery
removes all power sources.

DDR continuity begins at `reconnect-20261018-120/` and extends through the
recorded CPU intervals and ROM instances 121 → 002 → 005 → 008 → 010 → 067.
Refresh current067 launcher state before another native test; do not retrain DDR.
Current010 preparation is preserved under `all-core-first-task-20261018/warm-010/`.
The CPU0 production IRQ handler previously passed register restoration and
owned-page cleaning under `primary-context-probe-20261018/physical-005/runtime/`.

The all-core diagnostic's corrected image and three instruction/seven host cases
are preserved under `all-core-pre-scheduler-20261018/entry-corrected/`.
CPU0 reapplies required EL1 preparation after the reused prefix restores its
original controls, then verifies exact restoration after the new interval.
Earlier builds without that entry correction are not eligible for execution.
Reuse the accepted physical barrier result; the next CPU work is task selection
and scheduler startup, including ownership and cleaning of their larger write set.

The preceding successful second-board result returned on port `3-3` as
`003:056`, identity
`[7,3661,1097015,0,0]`, after successful read-only RTC progression. Source control
was `0x401c`, status zero, prescaler `0x0f`, calibration configuration
`0x7a000000` and oscillator control `0x983fb0f7`: normal RC mode with RC enabled.
Two coherent reads 1,100,773 microseconds apart observed one second of calendar
advancement. Outcome zero, the progression marker, monitor status zero and
responsive ROM return passed. The kernel runner did not restore prior DRAM
contents; this result does not establish memory restoration. No RTC register was
written. The runner exited and its claim closed as passed. Evidence:
`rtc-rc-readonly-20261018/physical/observation.json`. The subsequent USB failure
was recovered by the earlier power cycle to064.
Read progression is established; accuracy, absolute time, alarms and retention
remain open. The integrated source and alarm applications await physical admission.

Second-board DDR continuity is cold126 → register capture127 → PMIC009 → RTC011
→ RTC056. Current056 launcher/cache preparation and fourteen CCU reads pass
under `usb-returning-20261018/warm-056/` and `fabric-056/`. USB selector zero
and reset-held DMA0 pass under `entry-056/`; no DMA aperture was accessed.
The subsequent USB attempt returned a failure report. Power removal ended
that lifetime. The later independent064 admission and retained071 failure are
recorded above.
The preceding production PMIC test completed one DCDC1 snapshot, request reap,
client-lease release and idle-state checks with inherited settings restored.
DCDC1 was enabled with selector `0x17`, decoded as programmed 3.3 V. Evidence:
`pmic-provider-probe-20261018/physical/runtime/session.json`.

The GPU transport-only application is integrated, with eighteen focused cases
and scoped Astra review under `gpu-transport-only-20261018/`. It retains actual
PMIC/USB tasks, skips GPU construction and OPEN, and records transport success
separately from GPU execution. Physical startup/takeover review remains required;
inherited GPU admission requirements remain enforced, and USB has no close/restore
API. No full GPU or NPU retry is admitted by these returning peripheral results.

The corrected GPU attempt on the preceding instance `003:125` produced no
native USB response. Its sole refusal-report read timed out, and communication
stopped. Evidence: `gpu-usb-ordering-20261018/physical-candidate-125/runtime/session.json`.
The failure stage remains unknown; the recovered board does not authorize an
unchanged retry.

Fresh cold DDR, PMIC, warm cache preparation and configuration reads passed
before that attempt under `second-board-cold-122-20261018/`. Those observations
support the attempted bundle; they do not transfer to the recovered session.
The corrected masked fabric and USB-before-GPU ordering did not produce an
observable result. A narrower shared single-core startup diagnostic or concrete
new correction must precede another full accelerator launch. Root owns shared
startup diagnosis. A continuous instruction check now executes the frozen GPU
entry through monitor admission, EL1 entry, BSS clearing and platform initialization
on the linked stacks. Its post-MMU data accesses satisfy the actual descriptors.
Evidence: `gpu-entry-audit-20261018/continuous-entry/result.json`. Cache effects,
registers, time and GIC behavior remain modeled; no physical startup or scheduler
result follows from this check. The provider audit found no additional candidate
input defect; its report is `gpu-usb-ordering-20261018/audit125-provider/pre-usb-application-audit.md`.
The GPU software-failure reporting correction is integrated. It retains allocated
resources, suspends any created GPU worker and blocks OPEN while allowing healthy
USB to report the construction failure. Eight focused application cases and review
pass under `gpu-software-failure-20261018/`; physical reporting remains untested.
The returning TWI capture observed all nine strict PMIC
baseline predicates, and each matched in the new lifetime. The probe performed
no I2C transactions or TWI writes. This removes a candidate mismatch for the
observed session; it does not locate the previous GPU failure. Comparison:
`pmic-baseline-probe-20261018/capture-only/physical/predicate-comparison.json`.
The primary-source audit remains under
`gpu-usb-ordering-20261018/audit125-provider/primary-predicate-audit.json`.

The frozen USB provider delegates interrupt-state admission to its authority
callback; it does not read GIC enable, pending and active bits itself. The
shared monitor disables peripheral interrupts before provider startup. This
corrects the earlier attempt record's description of runtime interrupt guards;
no interrupt-related failure cause has been established.

The preceding NPU failure on `119` was recovered and closed before this attempt.
Recovery evidence is under `second-board-recovery-122-20261018/`.

The attempted NPU session used normalized fabric words and 200/600/400 MHz rates.
The previous GPU session contained an NSI bit rejected by its USB provider;
the corrected GPU candidate removes that bit and derives the MBUS rate from
observed configuration. Four focused calls into the frozen GPU ELF pass under
`gpu-usb-ordering-20261018/physical-candidate-119/fabric-admission-result.json`.
This identifies an input defect; the physical failure stage remains unknown.
The corrected candidate was attempted on `125`; its failure also remains unlocated.

The NPU USB reporting correction is integrated. Ten affected native cases and
independent review pass; 123 repository compiler inputs match the frozen image.
Evidence: `npu-usb-terminal-20261018/handoff.json` and `integration.json`.
USB retains independent counter, registry, lock, owner and lifetime checks while
permitting terminal NPU failure frames. This is not a physical failure diagnosis.
The actual host validator accepted its offline software package under
`npu-usb-terminal-20261018/host-preparation/`. Physical authority remains unset;
no replacement session or launch bundle exists.

The first board also completed a read-only boot-CPU GIC observation under
`primary-gic-state-121-20261018/`. Its redistributor is powered and awake, with
no enabled, pending or active local interrupts. A returning primary diagnostic
must restore that awake state; the secondary helper assumes an asleep baseline.
No GIC configuration was changed, and FEL scratch restoration passed.

The preceding complete eight-core image and first single-core GPU image both
produced no native USB result. Each sole refusal-report read timed out, and
communication stopped. Scheduler execution, GPU initialization and GPU computation
remain unobserved. These results do not locate either failure or establish a
shared cause. Preserve `smp-secure-pmr-20261018/full-image/runtime/` and
`gpu-single-core-20261018/physical-candidate-116/runtime/`; do not repeat either
image unchanged.

The integrated GPU application now waits for a complete pending-status USB IN
transfer before opening the GPU. Eleven focused application cases passed and
the frozen image received independent review. Evidence:
`gpu-usb-ordering-20261018/handoff.json`. This improves failure observation;
it does not establish the previous failure's cause. Hardware validation remains
outstanding. Root owns common startup and multicore diagnosis.

The new boot-CPU cache diagnostic passes its instruction checks for EL3 cache
adoption, exact restoration and retention on restoration failure. It reuses the
accepted primary preparation prefix. Evidence:
`primary-cache-probe-20261018/software.json`. The integrated host runner also
passes six focused cases covering report capture, RAM restoration, wrong epoch,
changed inputs and retained failures; evidence is under that directory's `host/`.
Physical execution passed on the first board. This establishes CPU0 EL3 cache
adoption and restoration before GIC initialization or secondary release; combined
startup barriers, CPU0 interrupt delivery and full scheduling remain unobserved.
The optional boot-CPU local-GIC/EL1 extension builds and passes six instruction
cases and nine host cases under `primary-el1-probe-20261018/`. It executes the
production local controller and cached EL1 entry, then restores the primary's
awake interrupt state. Baseline refusals before device writes and controller
refusals with verified restoration return distinct reports. Failed restoration
retains ownership. Physical local setup, cached EL1 entry, control restoration,
saved DRAM restoration and ROM return passed under
`primary-el1-probe-20261018/physical-121/runtime/session.json`. The runner stopped
and its claim closed as passed. A later returning diagnostic also passes CPU0
timer PPI30 and self SGI0 delivery with empty pending/active state and restored
controls. Six instruction cases and eleven host cases cover that extension;
physical evidence is under `primary-interrupt-probe-20261018/physical-002/`.
The later production-handler diagnostic also passes cached EL1 stack and nesting
writes with complete owned-page cleaning. Task selection, broader runtime memory
use and eight-core scheduling remain unqualified. The preceding EL1 entry review is recorded in
`primary-el1-probe-20261018/root-review.json`.
Earlier models missed implemented interrupt bits, Secure priority-mask behavior
and security-specific cache publication. Passing modeled startup alone cannot
justify another full image launch. A missing USB report can mean an intentional
software park as well as an exception or stalled access; record that distinction
when choosing the next diagnostic.

The production-handler source review and passing result are preserved under
`primary-context-probe-20261018/`. Seven instruction cases and twelve host cases
cover the final image. A bounded callback replaces the scheduler-dependent
application dispatcher; the native vector, register saves, nesting updates and
non-switching return execute unchanged. A delivery race was corrected before
launch: final counters are read after masking interrupts, including handlers
that finish just after the deadline decision. The next CPU boundary is actual
task selection and scheduler startup, with the complete runtime write set
cleaned before return. The production all-core barrier now passes physically. Do not repeat the failed full launch unchanged.

Host USB power capacity and voltage under load are unmeasured. The board manual
recommends 5 V / 3 A; this is a supply rating, not measured consumption. See
[power](power.md) before interpreting a missing result as a software fault or
power failure.

The preceding returning diagnostic established timer and peer interrupt delivery
on all seven secondary cores, coherent exchange on its single shared page,
clean shutdown and ROM return. Those results remain valid for their scope.

The complete FreeRTOS image now contains the Secure interrupt-mask correction.
Its full instruction model represents Secure mask restrictions and Nonsecure
priority scaling. Both the rejected-mask case and eight-core scheduler case
pass under `smp-secure-pmr-20261018/scheduler-observer-priority/`. Every core
completes 32 floating-point rounds, background work and inter-core interrupts;
CPU0 handles 333 timer ticks. USB controller calls are substituted in this model.

The USB observer now shares priority one with background tasks. Its former
priority four prevented CPU0's lower-priority diagnostic work from completing
under the modeled timing. The workload and scheduling timing were preserved;
only the observer priority changed. The final image and all 89 compiler inputs
and 34 linked objects are preserved under
`smp-secure-pmr-20261018/final-snapshot/`. Unchanged launcher, admission and host
checks are reused. Warm launcher preparation preceded the failed full-image attempt; no
additional DDR training occurred.

Physical boot-CPU participation, general task-context handling and eight-core
FreeRTOS scheduling remain outstanding. The failed full observer has been released after confirmed power removal.
The returning diagnostic's cleanup covers SGI0/PPI30 and a single shared page;
it does not qualify general peripheral interrupt or OS shutdown.

The second board completed three nonsecure EL1 reads of thermal calibration
shadows and returned as `003:116`, identity `[7,2774,1097075,0,0]`, on port `3-3`.
Addresses `0x03006244`, `0x03006248` and `0x0300624c` returned `0x66961102`,
`0x19689689` and `0x00000496`. The probe programs no fuses or thermal registers.
Evidence: `thermal-sid-shadow-20261018/runtime/`. Runner PID `28797` exited
successfully and is absent; exec session `22934` is terminal and its queue claim
closed as passed. This successful probe preceded the retained GPU failure above.

This establishes calibration-shadow accessibility in the thermal driver's
security state. The existing decoder accepts these words: the encoded factory reference is
25.8 °C. This is not a current temperature measurement. Physical temperature
acquisition and interrupts remain outstanding. Earlier PMIC reads established enabled
DCDC2 programmed to 800 mV and CLDO3 programmed to 1.8 V; they did not measure
analog stability. The unchanged thermal image is prepared under
`thermal-clock-reference-20261018/physical-candidate-adopt-116/`. Preserve DDR
continuity only within the recorded powered lifetime. The later GPU attempt now
requires power removal and fresh board preparation before another launch.

The SID probe reused fourteen accepted monitor/platform/FreeRTOS objects and
compiled only its new payload. Its focused model and source review passed.
A missing runtime-hashed survey source was caught before any device access;
the frozen package was repaired and the unexecuted operation resumed under the
same claim. Packaging now verifies those source dependencies before claiming.

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
The second is on `3-3`, instance `003:112` after normal system-supply-probe return, and
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

The original board on `3-1` returned successfully as `003:115`, identity
`[7,2770,1097074,0,0]`, after seven-secondary cached exchange and timer/peer
interrupt delivery. Each target handles one PPI30 and one SGI0, then disables
delivery and confirms no pending or active interrupt. Priority restoration,
64-line cache cleaning, target control/local-state restoration and observed OFF
all pass. The copied 4 KiB physical page matches completely. Shared distributor
and saved DRAM restoration pass; all 45 host transactions succeed. The runner
exited and its claim closed. Evidence:
`secondary-interrupt-probe-20261018/physical-114/runtime/`.

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
`003:112`, has successful DDR, returning survey, SD initial-register and PMIC
evidence. Its warm launcher survey and cache-off preparation also pass under
`warm-093-preparation/`; DMA0 remains reset with both bus gates masked. Preserve
its recorded DDR continuity without repeating training. The subsequent 20-register
PMIC probe passes under `gpu-supply-probe-20261018/runtime/`, including the GPU
supply selector. The subsequent 21-register probe adds the system-supply
selector under `sys-supply-probe-20261018/runtime/` and returns as `003:112`.
Refresh the current launcher state before another native image.
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
