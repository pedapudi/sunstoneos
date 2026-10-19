# Supervised native thermal observation

The thermal application composes the existing five-sensor controller and native
MMIO adapter with the board's shared clock registry, register mutex, calibrated
counter, USB provider and FreeRTOS tasks. It collects two fresh sets of latest
values and reports calibration, raw codes, temperatures and alarm events through
copied USB output. Physical acquisition, temperature accuracy and interrupt
delivery remain unqualified. The board is quarantined; no device operation is
part of this software integration.

The image uses supervised RAM-only workload 2. Its complete 808-byte session binds
the board prefix, owner, authority, APB1 profile, GPADC adoption/configuration,
five threshold triples and four physical-evidence digests. EL3 authenticates and
publishes a private full copy before provider construction. The common board seal
independently supplies the same authenticated prefix. Both application hooks
reject a prefix disagreement before using the extension.

The thermal task borrows exclusive THS ownership and stable GPADC24M ownership;
explicit cold GPADC configuration requires exclusive GPADC24M instead. FABRIC,
REF and OSC stay reserved, with PERI0 also reserved when APB1 selects it. Every
register operation uses the actual shared register mutex and native thermal
allowlist. No parent PLL is retuned, voltage changed or eFuse programmed.

The local EL3 boot patch configures interrupt 71 as Group1NS, level-high, routed
to the primary CPU at an API-safe priority, initially masked. The ISR only masks
that interrupt and marks deferred work under the same SMP critical section used
by task rearm. It never reads sensor status or takes the register mutex. The
owner calls the existing bounded read/W1C service and rearms only while live.
Periodic reads also collect conversion flags; interrupt notification is not a
conversion count. Pending notification is consumed before servicing so an ISR
arriving during service remains pending.

Each output set includes all five channels observed valid since the preceding
set, with an observation time for each channel. Channels are latest values, not
synchronous conversions. First/last controller sequence numbers identify each
set's collection interval. Alarm, alarm-off and critical masks record event
presence during that interval; coalesced events are not counted. The operation
finishes after two sets to establish repeated acquisition. It masks interrupt 71
and publishes an immutable 224-byte private payload: 32 bytes of calibration and
two 96-byte observation records. The 128-byte transport header binds the entire
session, calibration and payload by SHA-256.

Cancellation, expiry and uncertain operations revoke copied-output eligibility.
The sole owner checks the driver fault latch even when the controller returns
BUSY, then retries only clock-token retention metadata after failure; it
never continues a healthy controller operation to induce a fault. Controller,
clock and any uncertain mutex ownership remain held until confirmed whole-board
power removal. Successful completion also keeps every lease live and does not
stop sensors or reset the controller. Software supervision cannot recover a
stalled CPU or interconnect and is not an independent reset mechanism.

## Prepare and capture

Configure the normal board dependency/toolchain pins with
`SUNSTONE_BOARD_SUPERVISED=ON` and `SUNSTONE_A733_THERMAL=ON`, then build
`sunstone-a733-thermal`. The final image manifest is in
`thermal-application/manifest.json`. `apps/thermal/prepare.py` creates an immutable
launch bundle from that image, an 808-byte session template and reviewed entry
records. Its required arguments are `--image`, `--manifest`, `--admission`,
`--template`, `--output`, `--clang` and `--lld`.

`apps/thermal/capture.py` uses the existing queue, claim, port lock, discovery,
backup and retained-failure transport. It requires `--bundle`, `--fel`,
`--evidence`, `--database`, `--claim`, `--owner` and
`--supervised-manual-reconnect`. PID `0xefe8` is shared with supervised images;
the product and serial identify thermal separately. Copied success requires
complete session identity, exact lengths, hashes, calibration consistency,
ordered sequences and valid five-channel records. Raw frames and the complete
payload become durable before success is recorded.

The focused software batch passes the actual full-session/contract validators,
sanitized copy/publication checks, thirteen linked integration cases and four
host cases. The linked cases execute the application, clock registry, thermal
controller/native callbacks and copied-frame code with modeled MMIO values,
time, translations, RTOS boundaries and USB transport. They cover two fresh
sets, deferred notifications, contention/failure retention and output revocation.
The SRAM launcher is compiled and packaged; existing shared launcher evidence
is reused. No physical acquisition or interrupt result is established.

Exact inputs, generated boot/ISR code, linked objects, compiler dependencies,
recipes, failures and successful checkpoints are under
`~/.cache/sunstoneos/evidence/thermal-application-20261018/`. Fixture corrections
supply the actual const image identity and guarded assembly declaration, restore
the emulator's PC after an injected ISR, and derive modeled routing identity
from the authenticated bundle. Only affected or pending stages resumed.
Existing thermal arithmetic, calibration and controller evidence is reused.

## Physical request

The coordinator may prepare one bounded RAM-only attempt after quarantine and
recovery are resolved. Entry records must bind the exact image and board port
to exclusive THS/interrupt 71 and remote-writer exclusion, absence of an inherited
thermal service, security permissions, readable SID shadows, Device mappings,
stable AVCC/ADC/digital supplies, selected clock parents and a continuous counter.
Cold GPADC setup additionally excludes every GPADC/THS consumer; adoption requires
the supported enabled 24 MHz profile. Existing board USB/PMIC, CPU/memory/coherence
and storage-preservation prerequisites remain required.

Expected output is two fresh five-channel sets with valid factory calibration,
raw codes and converted temperatures, or a bounded retained failure. No heating
or forced alarm/critical event is requested. An alarm-free result cannot qualify
alarm interrupt delivery. After success, failure or lost transport, retain the
claim and all backing until confirmed full board power removal. Do not retry a
failed instance or treat cable reconnection alone as resource recovery.

Automatic throttling, shutdown policy and physical accuracy require later work;
this finite observation path does not implement thermal policy.

## Boot-CPU startup preserves the five-sensor service

`apps/thermal-single-core` builds `sunstone-a733-thermal-single-core.elf` using
the existing single-core monitor, cached mappings and FreeRTOS SRE port. Its
profile is `thermal-single-core-supervised-v1`. The image retains the existing
five-sensor controller, calibration, thresholds, shared clock/mutex ownership,
two-set acquisition and copied USB output. It links no secondary startup or
SMP barrier and requires all seven secondary CPUs to remain off.

The monitor authenticates the board prefix and full thermal session before
provider construction. Target-local patches retain both snapshots and the
bounded runtime contract outside EL1-cleared BSS. The monitor clears this state
before admission. EL1 cannot access secure monitor state. Thermal, calibration,
clock and GIC registers use the required identity-mapped, outer-shareable Device
memory. IRQ71 retains the accepted Group1NS, level-high, priority and boot-CPU
route. The single-core IRQ dispatcher calls the existing ISR, which masks its
source and defers sensor service. Task/ISR critical sections use the SRE port.
DAIF masking is restored after static task construction, before provider binding
and scheduler startup.

The existing thermal preparation and capture tools accept the distinct profile.
Preparation checks exact ELF identity, immutable geometry, entry and absence of
SMP startup symbols. USB ROM takeover mode two is required for this profile;
the existing profile retains mode one. The common single-core platform record
binds actual instruction publication, secondary OFF state, USB takeover and
resource ownership. Thermal-specific electrical, clock, security and retention
evidence requirements remain in force. Frozen platform condition bytes are
validated from the protected bundle.

Thirteen startup/interrupt cases and six host cases pass under
`~/.cache/sunstoneos/evidence/thermal-single-core-20261018/`. The startup cases
execute actual session admission, descriptor walks, provider/thermal/task
construction and IRQ71 dispatch. The ISR check observes deferred notification
without sensor-register reads. System registers, counter and GIC responses are
modeled. Host capture reuses the accepted calibrated two-set payload with a new
session binding. Existing calibration, arithmetic, controller and service cases
are reused without replay. Legacy frozen-bundle validation also passes.
Focused independent source/ELF review found no new blocker. Physical acquisition,
accuracy and interrupt delivery remain unqualified.

The coordinator may submit one RAM-only attempt on a freshly admitted target,
using the existing one-second startup, 60-second lifetime and 59-second capture.
Actual memory/publication, secondary OFF, USB takeover and thermal-specific
clock, supply, security, IRQ and ownership evidence must bind the frozen image.
Expected output remains two fresh valid five-channel records with factory
calibration. No heating, forced threshold crossing or threshold change is
requested. Stop at the first transport failure or disappearance and retain every
resource until confirmed whole-board power removal. Software fixtures grant no
physical admission; alarm-free output does not qualify IRQ delivery.

## Calibration shadows are readable on the second board

A returning FreeRTOS payload reads all three thermal calibration shadows in
nonsecure EL1. The observed words are `0x66961102` at `0x03006244`, `0x19689689`
at `0x03006248`, and `0x00000496` at `0x0300624c`. The payload makes no fuse,
thermal-controller, clock or supply writes. The accepted monitor returns to ROM
with automatic USB access. Evidence: `thermal-sid-shadow-20261018/runtime/`.

This establishes the shadow-access prerequisite for the native thermal image.
It does not establish calibration accuracy, sensor acquisition or interrupts.
The payload reuses fourteen accepted monitor/platform/FreeRTOS objects; only its
three-read task was compiled. Its focused model checks exact read order, raw
logging and normal exit. Earlier controller and host checks were reused.

The existing calibration decoder accepts the observed encoding. It reports a
factory reference of 25.8 °C, sensor codes `[2401,2406,2408,2408,2401]` and
corrections `[2044,2048,2050,2050,2044]`. One focused sanitized check uses the
actual decoder and these physical inputs; no earlier cases were repeated.
Evidence: `thermal-clock-reference-20261018/physical-candidate-adopt-116/calibration-116/`.
These are factory calibration data, not current sensor readings.

## Finite polling can return through the demonstrated small platform

The [returning thermal client](../apps/thermal-returning/README.md) implements
five-channel calibrated observations with the existing driver and common
clock/mutex providers. It gathers two fresh sets, performs configured disable
and reports through the RAM log before normal platform return. It retains
clocks, leases and controller configuration; configured disable does not prove
analog drain or restore peripheral state. Physical acquisition remains untested.
The coordinator must establish current board, supply, security and ownership
facts independently. This path needs neither native USB takeover nor multicore
startup. The existing interrupt-capable application remains separate.
