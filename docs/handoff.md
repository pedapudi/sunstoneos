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

Repeated I2S capture, playback and duplex are integrated. Four copied capture
slots remain owned until complete USB delivery and matching acknowledgment.
Cancellation preserves the copied backlog through terminal drain. Focused
native ownership, USB completion and host checks pass; source review is closed.
The earlier image's audit matched all 163 compiled inputs. Evidence:
`i2s-repeated-20261018/snapshot/manifest.json`, `review.json` and
`root-integration.json`. Accepted checks were reused.
Fixed hardware-linked rings are now integrated for repeating TX and checked-copy
cyclic RX/duplex. TX sources remain immutable; RX copies pass through private
staging before the existing USB slots. Complete USB delivery and exact ACK still
precede slot reuse. Descriptor/provider checks and three standalone-client cases
pass; client service calls are modeled, and the native image link is separate
evidence. Three focused review findings are resolved. Root integration verifies
168 compiled inputs, 69 linked objects, 127 sources, 224 artifacts and four tools
under `i2s-hardware-cyclic-20261019/root-integration.json`. Earlier unaffected
evidence was reused within the scope recorded in `identity-reuse-clarification.json`.
Root CPU-cache review does not establish device write completion. The manual's
completed-package counter does not explicitly order destination writes and
cursor updates. Cyclic RX/duplex physical admission additionally requires
`cyclic-rx-completion-and-reuse-ordering-established`; no qualifying record
exists. Physical continuity, DMA drain, cache visibility and wire-level drain
remain unqualified. Runtime PCM direction 6 now accepts successive copied
16 kHz mono S16LE periods through finite TX leases; immutable cyclic replay
remains a separate mode. Actual provider composition consumes eight distinct
periods. Nine standalone client/USB cases and eight host cases pass; the full
native image is linked, not executed. Root verifies 170 inputs, 70 objects,
131 sources, 314 artifacts and four tools against revision `0151af7` under
`i2s-runtime-pcm-20261019/root-integration.json`, within `closure.json` scopes.
The host supplies a requested prefix plus four guard periods, then CANCEL;
guards may be truncated. No graceful wire drain or complete-file playback is
established. Broader routed audio and physical qualification remain required.

Negotiated read-only SD modes are integrated. Sealed profiles retain one-bit
400 kHz operation or select four-bit default speed up to the card's CSD limit
and 25 MHz, or require four-bit high speed at 50 MHz. Controller/provider,
linked two-client reads, preparation and host checks pass under
`sd-bus-modes-20261019/`. The SD image rebuilt for USB HID layouts includes
those headers and class source consistently across all consumers. Linked SD/client,
USB0 copied-read, moved-field liveness and host identity checks pass; focused
review is closed. At integration revision `894051d`, the audit verified 182
sources, 1,219 artifacts and all 411 compiled inputs under
`sd-shared-usb-20261019/root-integration.json`.
Its ELF SHA-256 is
`141078f41ea22546a55117dd21245655a20162d4d09487a6647bad7ddf0a9d78`.
The preceding image and its six later USB-input differences remain historical;
unchanged protocol, clock and refusal evidence was reused. No card writes or
physical qualification are established. Both microSD cards remain preserved.

Native read-only eMMC access is integrated through the existing copied-client
service. Both SDC2 and SDC3 selections pass instruction checks for identification,
EXT_CSD and copied reads. Admission/capture checks and source review pass under
`emmc-native-20261018/`. Both current boards lack eMMC, so this optional path has
no physical qualification target. Its single-core image retains resources
permanently and has no qualified ROM return.

The native H.264 application now accepts one uploaded IDR picture, optionally
followed by one P picture, within an 8 KiB clip and 4 KiB per NAL unit. Pictures
are 48 by 32 pixels in progressive Baseline format. The two-picture profile uses
picture-order count type 2 and one default short-term reference. Decoder-context
picture-info/neighbor scratch persists across pictures; input, output and
motion-vector slots remain distinct. Up to two indexed immutable copied outputs
are available, preserving the IDR result if P decoding fails. Controller/provider,
parser, independent syntax and host checks pass with device effects modeled.
Root verifies 425 frozen inputs, 166 objects, 194 sources, 786 artifacts and seven
tools under `h264-interframes-20261019/root-integration.json`. Its ELF SHA-256 is
`862fae444a481eb9505d8ba7ef3835760e208764663c1e7aef8459f8b9be43ac`.
Six later USB hub dependencies differ from the integration tree; the frozen
candidate retains consistent inputs/layouts and supplies no current-tree rebuild
evidence. Earlier unaffected parser/upload checks were reused. Physical
reference visibility, pixels, confinement and drain remain unqualified; all
resources remain retained. Longer clips, repeated clips and wider profiles
remain required.

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

Native USB host HID input is integrated through the actual TinyUSB class and
xHCI interrupt-IN path at low, full and high speed. The board application copies
four reports to USB0 output without sending desktop input events. Sanitizer,
native consumer/serializer and host checks pass; source review is closed.
The original polling HID image's audit matched 408 compiled inputs under
`usb-host-interrupt-20261018/root-integration.json`.
Optional CPU interrupt completion is now integrated for the supervised board
image through `SUNSTONE_BOARD_USB2_IRQ=ON`. CPU0 interrupt 187 masks and notifies
the existing HID/MSC owner. That task acknowledges events, drains a bounded
batch and checks empty/race state before rearming. Other profiles retain polling.
Eighteen root routing/dispatch cases, ten composed device cases and owner,
native-capture and review checks pass. The IRQ image's audit matches 413 compiled
inputs and 161 objects, with 32 sources, 59 artifacts and three tools verified
under `usb-host-cpu-irq-20261019/root-integration.json`. Its selected consumers
use the new xHCI structure layouts consistently. Earlier SD, UART and other
images remain valid with their own frozen inputs; these audits do not establish
matching inputs at future revisions or require catch-up rebuilds. Physical
interrupt delivery, enumeration, report timing/content, VBUS cutoff, DMA drain
and detach/reuse remain unqualified.
A bounded USB hub path now supports one self-powered high-speed USB2 hub with
one to seven ports and one downstream low/full/high-speed HID or high-speed
MSC device. It uses the actual TinyUSB hub class, separate slots/control rings
and hub/transaction-translator contexts before child access. The existing CPU0
IRQ profile is reused without CPU/GIC/PHY changes. Component, native owner and
copied host checks pass with modeled hardware under `usb-host-hub-20261019/`.
Root verifies 414 inputs and 162 objects against revision `3f94f90` under
`dma-reconcile/root-integration.json`. The reconciled ELF SHA-256 is
`54c13a7125ab0af070acdd15f23fb0c0e4049a85bd7f19baaa18e0c960ab4e09`.
All load segments match the accepted candidate apart from build identity;
accepted cases were reused. Later headers do not invalidate its frozen inputs.
Hub signaling, external supply behavior,
DMA ordering and detach/reuse remain physically unqualified. Type-C VBUS cutoff
does not remove an independently powered hub's supply.

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

The optional single-core NPU interrupt profile is integrated. Bootstrap and two
inferences complete through deferred interrupt 97 service. The ISR masks and
notifies; one owner task acknowledges events and acquires output. Source-off
CLOSE and verified GIC stop precede ownership release. Host success requires
three notified completions and the stopped receipt, plus both copied outputs.
Nine service scenarios, controller/native-client/host checks and Astra review
pass; eighteen root routing/dispatch/monitor instruction cases pass. Integration
verifies all 184 compiled inputs, 144 sources, 1,026 artifacts and four tools
under `npu-interrupt-service-20261019/root-integration.json`. Physical interrupt
delivery, inference, numerical output, shutdown and ROM return remain unqualified.

The retained single-core NPU runtime application now accepts repeated live
inputs for one selected model through SUBMIT and sequence-bound READ requests.
The owner uses the existing input publication, matched completion, copied output
and REAP path. Complete output transmission precedes mailbox reuse. CLOSE
requires the IRQ-stop receipt and local gate/reset shutdown; OS/USB remain live
and this profile does not return to ROM. Linked USB/mailbox/client and host
checks pass with service replies and output modeled. Evidence:
`npu-runtime-input-20261019/`. Its reconciled ELF SHA-256 is
`063f8fce5cc975255d2f7a80c170aec5d1a28e2d9ae06fb7f9b30377857f9381`.
Root verifies 186 compiled inputs, 74 objects, 297 sources and 329 artifacts
against revision `5954081` under `dma-reconcile/root-integration.json`.
Load-segment equivalence apart from build identity supports reuse of accepted
checks. Later shared-header changes do not invalidate this frozen candidate.
The fixed returning profiles remain separate.

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
Caller-selected GPU compute programs are integrated. A sealed session fixes a
trusted host-compiled program and initial copied input; the same executable supports
both checked array kernels. Bounded multi-dispatch now supports up to 31 direct
dispatches, each containing one 32-lane workgroup, and at most 992 elements
within the package's input/output bounds. One final firmware timeline covers
all dispatches. Compiler-reference, sanitizer, native service and host checks
pass with device writes modeled; focused review is closed. Evidence is under
`gpu-dispatch-geometry-20261019/`. Integration verifies 85 objects, 169 sources
and 66 artifacts. Of 215 compiled inputs, 214 match; the sole monitor difference
adds inactive `SUNSTONE_NPU_INTERRUPT` branches. Frozen monitor source and object
remain authoritative; no active GPU monitor or structure change is present.
Optional GPU runtime input now copies same-shape data into the existing input
allocation after acquired preceding completion. Program, geometry and mappings
remain fixed. Exact-sequence READ returns copied output; healthy CLOSE retires
the GPU while retaining USB reporting. Native service and linked mailbox/parser/
host checks pass with hardware effects and device output modeled. Device-cache
ordering depends on the pinned firmware's documented per-job coherency contract;
CPU cache maintenance alone does not establish GPU-cache visibility. Evidence:
`gpu-runtime-input-20261019/`. Its reconciled ELF SHA-256 is
`d6b6fad18e634fc5d5cffdc791214511ccab3436b80a281f1fe464fccc460892`.
Root verifies 215 compiled inputs, 85 objects, 172 sources and 14 artifacts
against revision `5954081` under `dma-header-integration/root-integration.json`.
Load-segment equivalence apart from build identity supports reuse of accepted
checks. Later shared-header changes do not invalidate this frozen candidate.
Fixed returning artifacts remain separate.
The native reservation callback now permits another submission after acquired
completion, checking owner, epoch, serial and the actual firmware timeline.
Focused checks exercise that callback and reject stale ownership/completion.
The corrected fixed-workload returning candidate is frozen under
`gpu-returning-repeat-fix-20261019/`, with seven helper instruction cases and one
normal two-job/CLOSE/provider-parking model passing. It reuses 86 of 88 objects;
return objects and structure layouts are unchanged. Its ELF SHA-256 is
`2ab2cb1b3bac150d5eb695793ab0eb39bb453afc7fbd7ecb2f595ebfca3fd08d`.
Root source and binary review passes under `root-review.json` in that directory,
verifying all 88 objects, 218 frozen inputs and eight unchanged linked return
functions. Physical admission remains false. The historical two-job image
contains the previous NEW-only check and is superseded for repeated submission;
its publication and host evidence retain their narrower scope. Physical
computation remains unqualified. General dispatch geometry, shape/program
replacement, graphics and concurrency remain required.
The [publication audit](a733-multicore-reference-audit.md#publication-must-cover-the-executable-physical-address-space)
and device skill document the Nonsecure publication requirement. Root retains
CPU and architectural return review. For the preceding adapter, all 172 NPU
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

1. Continue GPU grid dispatch, USB support for two downstream devices and
   repeated Bluetooth capture implementation. Prepare the NPU multioperator
   proposal; these extensions remain in progress. Bounded H.264 IDR/P sequences,
   GPU/NPU live input and the single-leaf USB hub path are integrated offline.
   Preserve each image's complete frozen inputs and accepted audit scope.
   I2S runtime PCM, fixed cyclic TX and checked-copy RX/duplex are integrated,
   with physical RX admission blocked by the documented completion/reuse ordering
   gap. Preserve copied-slot
   acknowledgment and cancellation-backlog rules. Reuse accepted images with
   their complete frozen source closures, including SD and UART images that
   predate the latest shared USB layout. Preserve the accepted fixed-workload GPU
   returning profile; its passed root delta review supplies no physical admission.
   General GPU geometry, graphics and concurrency remain required.
   CPU/startup changes remain root-owned. Preserve both cards.
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
