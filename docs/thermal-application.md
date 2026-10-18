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
