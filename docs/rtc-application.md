# Native RTC read and alarm diagnostic

The adoption and cold-initialization modes read the A733 SoC real-time clock
(RTC) and arm one non-waking alarm one to ten seconds ahead. A separate finite
runtime mode sets an authenticated calendar target after providers start and
returns its result without arming an alarm. An explicit authenticated mode can
initialize the onboard crystal source and set a requested calendar value before
application startup. Read-only adoption requires an existing valid source and calendar.
The owner task acknowledges its interrupt, disables the alarm, restores the
previous disabled target, and publishes a copied calendar/result snapshot over
USB. Cancellation follows the same ownership and restoration rules.
The source and linked image pass focused software checks. Physical operation
and retention remain unqualified.

A separate finite returning client has now observed the controller's inherited
clock state physically. R_CCU gate/reset-release is `0x10001`, RTC source control
is `0x401c`, and source status is zero. The external-source adoption requirement
refuses this state before calendar access. The client records an explicit
negative outcome and returns normally without RTC writes. Zero calendar fields
in its log are unobserved placeholders. Evidence:
`rtc-returning-outcomes-20261018/physical/observation.json`.
Clock-source support for this observed state is under source review; calendar
progression, alarms, accuracy and retention remain physically unqualified.

## Sources and board prerequisites

The pinned Orange Pi kernel revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` identifies the controller as
`allwinner,rtc-v201` in `arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi`,
lines 962–975: base `0x07090000`, SPI 196 (GIC interrupt 228).
The matching `bsp/drivers/rtc/rtc-sunxi.c` uses TypeA one-based calendar days,
absolute day/hour/minute/second alarm targets, and write-one-to-clear status.
Its source identities are recorded in `apps/rtc/sources.json`. The native
calendar and register operations adapt that GPL-2.0-or-later implementation.

The A733 User Manual V1.00, section 9.1.7, pages 391–398, defines the calendar,
alarm and clock register fields. Its PDF SHA256 is
`cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d`.
Section 9.1.4.3 describes a seconds counter, and section 9.1.5.3 says writing zero
clears alarm pending. Those descriptions contradict the register tables and
matching driver; the application follows the tables and driver. It treats day 1
as 1970-01-01 and refuses day 0 rather than repairing the inherited calendar.

Manual section 9.1.7.1, pages 391–393, defines clock control source bits 1/0,
crystal gate 4, calendar-write busy bits 8/7, oscillator gain 3/2 and automatic
switch controls 15/14. Section9.1.7.2 defines external-source status 0, automatic
switch pending 1 and oscillator failure 2. The diagnostic requires the external
32768 Hz source, no busy/fault/pending state, and stable source/gain/switch fields.
It never changes those fields.

The board schematic V1.2, SHA256
`4e0e92afbb1cca97a9c8593b8f2679178d9738f242ea431ed3650e893fe28ed0`,
shows Y2 `32.768K-12pF-20ppm`, C151/C158 12 pF and R11 10 MΩ on sheet 8
“08_CORE1”, connected to U2G X32KIN W37 and X32KOUT V37. Sheet 6
“06_POWER_AXP318” connects U1A M15 VRTC through RTCLDO-PMU to VCC-RTC;
L15 BKUPBAT is marked unconnected. This supports a powered-retention hypothesis,
not retention after removal of every supply. The schematic does not identify
the fitted board revision. The pinned zero3w-named DTS instead uses an
orangepi-4-pro compatible and HYM8563 on s_twi2. That external-chip variant
remains unresolved; this application does not assume it is fitted.

## Sealed ownership and startup writes

The ordinary board hook authenticates the full session against the copied boot
contract. The RTC hook copies and authenticates its own 768-byte session before
any RTC mutation. Four evidence hashes bind initial ownership, clock/calendar,
electrical conditions and manual recovery to the target and exact image.
Nonzero hashes alone do not prove those facts.

Before cached startup masks all SPIs, the primary EL3 hook requires interrupt 228
disabled, inactive and unpending. Under admitted exclusive RTC/R_CCU ownership
and exclusion of other cores and remote writers, it sets only bits 16/0 of
R_CCU `0x0701020c`, preserving the other bits and checking exact readback.
The matching kernel `ccu-sun60iw2-r.c` and manual section 4.2.5.22 define these
as reset release and bus gate. No scheduler mutex exists at this stage.
If reset was inherited asserted, release cannot prove preservation of an earlier
calendar. Invalid calendar state subsequently refuses the operation.

After bus enable, alarm enable, IRQ enable, pending and wake output must all be
disabled. An exposed pending event causes retained refusal without an
acknowledgement. After GIC affinity routing is enabled and before secondary
release, the hook configures only interrupt 228 as masked, level-triggered,
nonsecure Group 1, priority 0xe0, routed to core 0. Partial failure retains all
ownership; it does not gate the bus, assert reset, clear inherited events or
restore uncertain state.

Task access holds existing clock stability leases and the shared register mutex.
The interrupt handler only masks 228, checks session lifetime and publishes an
atomic notification. The owner waits for distributor mask propagation and an
inactive interrupt before RTC access. It acknowledges only its own pending bit
with an exact write 1. Cancellation may race its own alarm; an observed pending
event wins and is reported as an alarm. Saved targets are restored only after
confirmed RTC disable/acknowledgement. Copied publication additionally requires
GIC quiescence and an unexpired, live session. Ownership lasts until manual
whole-board power removal even after success.

## Remaining RTC modes and physical evidence

The explicit mode initializes the fixed crystal source and sets the calendar
at boot. A finite owner-task mode also sets a calendar target while providers
are running. Repeated updates, composition with subsequent alarms, concurrent
clients, oscillator calibration, alarm wake from low power and retention
qualification remain required. Both modes preserve DCXO,
LDO and general-purpose/boot words. No authoritative hardware reset-cause field has been identified;
software boot markers are not decoded as reset cause. Warm-reset retention and
complete supply-removal retention require separate physical evidence.

## Frozen software evidence and launch

The affected supervised image and source/dependency records are retained under
`~/.cache/sunstoneos/evidence/rtc-native-20261018/`. Its ELF SHA256 is
`bd1043d087bdcc1293cb298439f441643d4b493cbbd39fe4fa2b690604a87ca8`.
The generated build ID is
`8e4b39b653dbfc8371d24124b35036675bffbd7e900191b84ecbc5634eb947dd`.
The evidence records 174 compiled repository inputs, 417 dependency inputs and
2,496 pre-run input hashes. Those hashes still match after execution.

The new checks cover TypeA calendar/leap-day/midnight and alarm ownership,
partial boot failures, complete session sealing and publication refusal,
core-0 IRQ notification, deferred immediate-interrupt enable/readback,
private copied output and revocation, protected packaging, launcher admission,
and host framing/capture/RTC authority. The driver executes over explicit MMIO
callbacks. Linked native instructions execute over modeled hardware and cache
publication boundaries. Host frames are synthetic and carry the snapshot copied
by the native instruction fixture; native USB serialization is source/link-only
coverage. No physical timing, oscillator accuracy or retention result is implied.

`apps/rtc/prepare.py` creates a protected bundle from the exact image manifest,
a session template and reviewed target-specific physical admission records.
The software model template in the evidence directory is nondeployable.
`tools/check-a733-rtc-supervised.py` requires a coordinator-owned queue claim,
the sealed bundle and manual recovery admission. Request byte 0 observes a
result; byte 1 requests cancellation. The host tool's `--cancel` option sends
that request once. A successful 64-byte header carries session identity and a
SHA256 digest followed by the immutable 88-byte calendar/alarm snapshot.
Pending or failed replies expose no snapshot. Raw refused frames remain durable.
After any attempted run, ownership remains retained until whole-board power
removal. Disconnecting USB removes all power when USB is the only supply; any
additional supplies must also be removed. The coordinator verifies the newly
connected device instance before resuming.

## Explicit source and calendar initialization

A separate 784-byte session, version 2, appends four little-endian 32-bit words
to the existing adoption session: flags, day, packed HMS and zero reserved.
Flag 1 authorizes source initialization; flag 2 authorizes calendar setting.
Both may be selected. Version1 remains 768 bytes and cannot reach either write
path. The complete copied extent is bounded to 768 or 784 bytes before validation
or hashing. Initialization consumes that retained authenticated setting.

The clock evidence must authorize the requested change, identify the fitted
onboard crystal, and exclude inherited users of the shared 32k source and remote
source writers. Calendar authority binds the requested day and HMS. Absence of
FreeRTOS clock leases before the scheduler does not establish this exclusion.
The single-primary EL3 hook performs initialization after existing inactive
alarm/GIC and bus ownership admission, before other cores or provider startup.

Source initialization uses keyed preserve-only updates of RTC control bits 14/15
for automatic fallback, bit 4 for crystal enable, and bits 1/0 for osc32k and
external-source selection. Gain bits 3/2 remain unchanged. Manual section 9.1.7.1
and the pinned CCU mux declarations define bit 1=0 as osc32k selection.
Section 9.1.5.1 contains conflicting bit 1 source-selection prose. The pinned CCU
probe also polls statusbit 4, whereas the manual defines oscillator health at
status bit 2, clear when healthy. This implementation follows the register table:
four healthy samples, selection status bit 0 and no remaining fault/pending state.
It records inherited source pending before acknowledging only W1C bit1 under
explicit source authority. It does not copy vendor DCXO/fanout writes.

The source-readiness budget is one second. Calendar setting follows pinned
`sunxi_rtc_settime`: wait for HMS access clear, write HMS, wait for completion,
wait for DAY access clear, write DAY, and wait for completion. Each of the four
busy waits allows one second as specified by manual section 9.1.7.1, with the
vendor's two-millisecond settling delay after each clear. All waits and delays
also check the absolute startup deadline. A partial update retains ownership;
no rollback asserts that the former calendar or source has been restored.

The RTC target selects six seconds of startup within the existing 60-second
session. One second for source readiness, four one-second busy waits and eight
milliseconds of settling leave 992 milliseconds for remaining startup work.
Other board targets retain the default one-second startup interval. The RTC
launcher, native validator and board session-start derivation use matching
intervals, preserving the original total session deadline. These are failure
bounds; physical oscillator startup and successful boot within them remain
unqualified.

Calendar verification compares a coherent absolute day/HMS timestamp with the
requested timestamp and the measured interval beginning at the HMS write.
If HMS crosses midnight before DAY is written and the DAY write loses that
carry, verification refuses the resulting earlier date. It does not infer a
repair. The requested calendar is explicit user input, not proof of accurate
current UTC. Hardware continues from that value between later reads.

`apps/rtc/setting.py` creates this session template from an adoption template,
a reviewed clock-evidence file, optional `--initialize-source`, and an explicit
`--calendar YYYY-MM-DDTHH:MM:SS`. The normal preparation command then binds
physical admission and the exact final image. The clock record must authorize
that target and source operation; software-model records are nondeployable.

Version2 result frames retain the 64-byte header and append 136 bytes on success:
the existing 88-byte alarm/calendar snapshot plus 48 bytes of inherited/final
initialization fields, requested flags, stage, attempted-write count and
elapsed microseconds. Stage 6 means complete initialization; earlier stages
identify a retained partial failure. Boot failures do not start USB or publish
a successful result. The diagnostic remains in admitted RAM for a separately
reviewed observer; no physical observer result is claimed.

The new image and focused checks pass under
`~/.cache/sunstoneos/evidence/rtc-calendar-native-20261018/`. Its ELF SHA256 is
`e7c83e371ff81f8a5e62b07ba6cae517003a01e24ea9e2301345b58dbe751817`;
the generated build ID is
`1a4684e9692b6f6db65451e3d637336cf0674b770559859b9d67c33c106c46ec`.
All 177 compiled repository inputs, 420 dependencies and 2,535 pre-run input
hashes match after execution. Independent source/artifact review accepted the
results. The frozen adoption image and its passed evidence above remain intact.

The new cases execute the initializer over explicit callbacks and linked native
instructions over modeled MMIO. They cover source-only/calendar-only operation,
600-millisecond calendar writes, partial/source/busy failures, lost midnight
carry, exact private session binding, no initialization writes in adoption mode,
combined copied output, default/RTC startup validators, launcher and host
admission. The combined copy contains the actual native initializer report and
an explicitly modeled later alarm/calendar snapshot. Earlier alarm/IRQ evidence
is reused. Physical source readiness, UTC accuracy, oscillator calibration,
retention and native USB serialization remain unqualified.

## Finite runtime calendar setting

Session version 3 retains the 784-byte setting layout. Flag 4 requests one owner-task
calendar update; optional flag 1 initializes the crystal source at the existing
EL3 boundary. Cold calendar flag 2 is excluded from this profile. Versions 1/2
cannot authorize runtime writes. The host template tool accepts `--runtime`
with `--calendar`, and normal admission binds the exact target and final image.

The boot hook verifies source readiness and preserves raw calendar fields,
including invalid values awaiting repair. In this profile its stage 6 report
means source-ready and calendar-pending. It does not mean that the calendar was
set or valid. The owner task starts after provider construction and scheduler
startup, acquires its permanent clock stability lease, masks interrupt 228 and
requires it disabled, inactive and unpending before any calendar transaction.
RTC alarm enable, IRQ enable, pending and wake output must all remain disabled.
An unexpected active/pending alarm causes retained refusal without acknowledgement.

The sealed authority excludes every other calendar/source writer for the entire
operation, including remote firmware. Releasing a register mutex during waits
does not release that authority or the clock lease. Each RTC access takes and
releases the existing shared register mutex; contention follows its bounded
scheduler behavior. Busy and settling waits call the task delay API with no
mutex held. Runtime writes are restricted to DAY/HMS. Crystal, reset, GPR,
DCXO and LDO writes remain outside this task path.

The callable driver operation reuses HMS→DAY ordering, one-second busy bounds,
two-millisecond settling and coherent absolute-time verification. Every resume
and each transaction checks session/deadline/clock authority. Calendar/source
writer exclusivity spans the yielded intervals. A failed transaction retains
its partial state rather than asserting restoration. Verified midnight carry
uses the same absolute-time rule as the cold operation.

Cancellation is graceful only before the first attempted HMS write. The native
write callback rechecks it under the mutex immediately before that boundary.
Its special no-write return proves that no hardware store was attempted. Any
other failed write callback is treated as possibly mutated hardware. After the
first attempt, graceful cancellation is recorded while bounded completion and
verification continue under valid authority. Session revocation, clock loss or
deadline expiry stop further MMIO immediately. A completed late cancellation
is reported separately; no alarm is armed afterward.

Version 3 frames have the existing 64-byte header and 104-byte payload. The payload
contains a 56-byte runtime operation/outcome followed by the 48-byte source-ready
boot report. Runtime outcomes are applied, cancelled without a write, applied
after late cancellation, and retained failure. It contains no ordinary alarm
snapshot. Header status 1 means a terminal copied report; clients must inspect
the runtime outcome before interpreting it as successful calendar setting.
A still-live controlled partial failure may publish a retained-failure report.
Shared revocation/deadline loss prevents publication and copied access. The host
records cancellation, late application and retained failure distinctly from
observed success and preserves raw frames before decoding.

The runtime image and focused checks pass under
`~/.cache/sunstoneos/evidence/rtc-runtime-native-20261018/`. Its ELF SHA256 is
`e8125a65f44281076b91ae4d8fa064ba122028db98ca8cf535e7b4c2515bf28e`;
the generated build ID is
`0b7ef02d9a7ca2c40e35c47c20782f38302b66382d103f00b8c44d6b74c39b4c`.
All 178 compiled repository inputs, 421 dependencies and 2,584 pre-run hashes
match after execution. Independent source/artifact review accepted the results.
The frozen adoption and cold-initialization images above remain unchanged.

The new native cases execute owner construction, task dispatch, runtime MMIO
callbacks and copied output through modeled public clock/mutex/task-delay
boundaries. They distinguish cancellation under the first-write mutex from a
failed callback after an actual HMS store, and cover late cancellation, partial
DAY timeout, pending/active alarm refusal, clock loss and deadline expiry with
no subsequent RTC MMIO. Task-delay observations require the mutex released.
No alarm is armed in these cases.

Successful calendar reports now publish the same coherent day/HMS pair used
for verification, preserving reserved fields. They do not substitute later raw
diagnostic reads that can straddle midnight. A focused driver case covers this
common initializer correction, and a native runtime/host case forces the same
rollover boundary. Prior cold/alarm/IRQ/default-interval evidence is reused;
those suites were not replayed.
Physical calendar timing, UTC accuracy, native USB serialization, wake,
calibration and retention remain unqualified. This finite consumer does not
complete repeated updates or calendar/alarm composition for the full OS.

## One owner serializes reusable application requests

The service profile uses the existing 784-byte sealed session with version 4
and the runtime-calendar flag. Its clock evidence also authorizes the two fixed
native calendar clients. The session target seeds the application demonstration;
registered clients may request any valid TypeA day/HMS through the setting API.
The second demonstration target is one day later, so the seed must be below
65535. Older profile modes retain their existing operations and extents.

Two core-0 application tasks call the task-context interface in
`include/a733_rtc_service.h`. The current FreeRTOS task handle identifies each
client. Each client has one fixed slot and one outstanding generation. A static
two-entry FreeRTOS queue copies slot indices to the sole core-0 owner. Request
reservation and zero-wait enqueue occur in one short critical section. Failed
enqueue releases only its unpublished reservation; generations never repeat.
No application pointer survives submission.

A client copies its immutable terminal result and explicitly releases the
matching generation before submitting another request. Old generations cannot
copy, cancel or release newer results. Another client's generation never grants
access. Queue admission refuses an occupied slot, invalid request, unregistered
task, poisoned service or expired session. Requests contain calendar values or
a bounded alarm delay; they cannot supply register offsets or source settings.

The same owner performs coherent reads, calendar setting and non-waking alarms.
It retains exclusive calendar/source writer authority and the permanent clock
lease across idle periods, released result slots and yielded waits. Every native
transaction checks session, deadline and clock health, then uses the existing
shared register mutex. The calendar setter releases that mutex during busy and
settling delays. It writes only DAY/HMS; clock initialization remains at the
sealed boot boundary.

A coherent read is allowed during the owner's active alarm. Calendar setting
and a second alarm return busy until that alarm ends. The alarm's client may
cancel only its matching generation. Queued cancellation completes without
starting hardware. Setting cancellation before the first attempted HMS write
is clean; later cancellation follows bounded verified completion and reports
late application. Authority loss stops further accesses and copied publication.
Cancellation racing alarm completion reports the observed alarm if it has fired.

Normal alarm completion disables and acknowledges only the owned source,
restores the saved disabled target and verifies its readback. The owner masks
the SPI and confirms it inactive before touching driver state. It separately
waits for pending/RWP propagation after source completion, without repeating
that completed source operation. Private driver state is reset only after the
source is disabled/unpending, the GIC is masked/inactive/unpending and the ISR
notification has been consumed. Uncertain failures retain every reservation,
permanently refuse later requests, and terminalize waiting clients while live.
There is no speculative rollback or automatic retry.

The native application demonstrates two distinct sets, repeated reads, one
completed alarm and a separately owned cancelled alarm. The second client reads
and receives a busy setting result during the first client's alarm. Both clients
then reuse released slots. The copied observer transcript holds at most twelve
200-byte results behind a 16-byte header. The demonstration freezes ten results;
per-client operation slots remain the application interface. A service frame
uses wire version 4, the existing 64-byte identity/digest header and a 2416-byte
payload. Host cancellation cannot impersonate a native client's request token.

The focused service checks use the same owner, client tasks, API and RTC driver
bodies with the pinned FreeRTOS POSIX port, task scheduler and queue. The fixture
models only the hardware/provider boundary. A separate final-image check covers
actual native sealing, task construction and MMIO callbacks. Synthetic USB
framing does not establish native serializer operation. Physical clock accuracy,
calendar timing, wake, backup retention and power-loss behavior still require
board evidence. Source switching, runtime multi-client ownership outside the
two registered tasks and wake-policy integration remain separate requirements.

The service image and focused checks pass under
`~/.cache/sunstoneos/evidence/rtc-service-native-20261018/`. Its ELF SHA256 is
`9c5277abb1c518a2ba69d656a853072615bb0db254c61678e6e1d05720621eac`;
the build ID is
`36b4d3d58e8eb27a81affdb7b47b89b8e570f552aa8acce3f72f4df86e93e911`.
Independent review reconciled 182 compiled repository inputs, 425 dependencies
and 2,661 preserved runtime input hashes. The scheduler cases cover successful
composition, delayed GIC deassertion, live retained partial failure,
authority loss after an actual HMS store, queued cancellation and late setting
cancellation followed by another read. Stale and cross-client generations are
refused. The native check covers actual sealed admission, registered task
construction/affinity, task privilege admission and register callbacks.

The POSIX fixture configures a one-second alarm cap; the native producer
admits two seconds. One-second requests fit that admitted range, but these are
distinct scheduling configurations and establish no timing parity. A failed
host compilation exposed missing POSIX feature declarations. A failed native
fixture assertion assumed the one-second cap instead of reading the admitted
two-second value. Both failures, original inputs and corrected recipes remain
preserved. Only affected or unexecuted steps resumed; the successful scheduler
cases and native image were reused. Older RTC suites were not replayed.

## Native USB serialization executes with the frozen service image

The USB integration check reuses the service ELF above without rebuilding it.
Evidence is preserved under
`~/.cache/sunstoneos/evidence/rtc-usb-native-20261018/`. The five focused cases
pass, and all 109 sealed inputs match after execution. Earlier scheduler,
calendar, alarm and IRQ results are reused.

The check executes the actual USB task, RTC copied-result API, serializer,
TinyUSB initialization and descriptor configuration, vendor FIFOs, device event
queue and transfer-completion callbacks. Controller events enter through the
actual DCD event handler. The fixture models controller initialization, endpoint
opening, transfer ownership and completion; it does not access physical USB
registers, hardware FIFOs or the PHY. It never substitutes a host-generated RTC
frame or directly invokes vendor/application completion callbacks.

The completed 2416-byte transcript comes from the preserved real-FreeRTOS
scheduler case. The fixture adopts those immutable bytes and their digest into
the final ELF's private service layout. The actual native copy and serializer
then produce the 2480-byte response. Defining compilation units and DWARF
locations resolve private variables whose names occur elsewhere in the image.
The prepared USB provider uses admitted owner identities and exact native object
pointers. Its final platform poll and health consumers execute, while lower
clock, rail and PMIC status results are explicit model boundaries. This does
not replay or establish hardware initialization or real lease-ledger health.

A pending query emits a private 64-byte header with zero digest and no payload.
Two successful copies are identical to the adopted bytes. Actual TinyUSB write
returns accept 2048 bytes followed by 432 bytes for each response. Each response
completes as four 512-byte transfers and one 432-byte transfer. With completion
withheld, a second OUT request waits in the real RX FIFO. It cannot replace the
first response or invoke another copy until that response finishes. Endpoint
backing remains unchanged while a modeled transfer owns it.

The service-cancel request reaches the actual shared fatal latch; native clients
retain their scoped cancellation API. Provider failure and deadline expiry stop
further submissions while retaining the outstanding transfer. An injected poison
store during the actual transcript copy exercises its final refusal check and
produces a 64-byte failure frame without payload or digest. These are selected
instruction interleavings, not a concurrency stress or timing qualification.

Preserved failures include two declaration-parser setup errors and one DCD hook
that compared unspecified upper register bits for a narrow endpoint argument.
The corrected hook follows the declared argument widths. Only affected or
unexecuted cases resumed. No production source, image or prior evidence changed.
Physical USB operation, clock accuracy, wake/retention and recovery qualification
remain outstanding.

## Boot-CPU service startup has focused software evidence

`apps/rtc-single-core` builds `sunstone-a733-rtc-single-core.elf` with the
existing RTC owner, two native clients, and USB transcript serializer. It uses
the supervised single-core monitor and FreeRTOS SRE port already used by SD,
GPU and NPU applications. Secondary CPUs remain off; their power state is
checked before RTC bus access. No secondary startup or SMP barrier is linked.

The monitor authenticates both the full RTC session and its board prefix before
provider construction. Target-local patches place these copies, early RTC
initialization state and the runtime contract in retained RAM. The monitor clears
that RAM before admission; EL1 BSS clearing preserves it. EL1 cannot read secure
monitor state. RTC and GIC registers use identity-mapped, outer-shareable Device
memory. IRQ228 remains disabled until the RTC owner rearms its existing binding.
Static task construction is followed by DAIF masking before USB provider binding
and again before scheduler startup.

The host preparation and capture tools accept the distinct
`rtc-single-core-supervised-v1` profile. Preparation checks immutable ELF geometry,
entry, excluded SMP symbols and exact image hash. It binds USB ROM PIO takeover
mode 2 to this profile; the existing profile requires mode 1. Platform evidence
must cover actual boot-CPU cache entry, secondary OFF state, physical instruction
publication, USB takeover and exclusive resource ownership. Frozen condition
bytes are retained with the bundle and validated without reopening original paths.

Twelve changed startup/profile cases and five packaging/capture cases pass under
`~/.cache/sunstoneos/evidence/rtc-single-core-20261018/`. They cover authenticated
session retention, actual descriptor walks, native owner/client/provider
construction, admission refusals, profile binding, launcher compilation, frozen
evidence and capture with a retained native service transcript. The descriptor,
GIC, RTC, counter and system-register responses are modeled. Existing service,
calendar and USB evidence is reused without replaying those operations.
The frozen ELF SHA256 is
`078c6e40a3eb30a31b2a3d7e5085c5e0300bb6ee5b0a7a42d76f2becefef4bc2`.
Focused independent source/ELF review found no remaining substantive blocker.

Physical RTC timing, calendar changes and USB service capture remain unqualified.
The coordinator may propose one RAM-only attempt with six seconds for startup
and a 60-second native lifetime, using the existing 59-second host capture.
Actual image-bound evidence must establish DDR/cache publication, exclusive RTC
and R_CCU ownership, a stable admitted source and supply, client calendar-write
authority, USB ROM takeover and complete-power-removal recovery. A successful
capture must contain the existing ten-result service demonstration. Stop after
the first transport failure or disappearance and retain every resource until
confirmed whole-board power removal. Software fixture bundles supply no physical
authority.
