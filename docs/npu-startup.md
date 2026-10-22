# Bind NPU preparation to privileged service activation

The [startup binding](../drivers/npu/a733_npu_startup.c) connects NPU platform
preparation to shared resource leases, FreeRTOS mutexes, and the inherited I2C
controller. It reads the DCDC2 configuration before permitting NPU startup.
The [service](npu-service.md) can remain inactive until the designated bootstrap
task verifies the prepared resources and its registered backend.

Implementation and two focused FreeRTOS emulator modes pass. Physical PMIC
transactions, NPU startup, inference, and recovery remain unqualified for this
binding. The initial 492 MHz profile retains the inherited-clock requirements
and limitations of the [platform controller](npu-platform.md).

## Startup reserves shared resources before device access

A private startup record fixes the board controller, platform configuration,
backend callbacks and context, MMU image metadata, and bounded command table.
Initialization copies the record before scheduling starts. The command table
contains at most `NPU_SERVICE_COMMANDS` entries. Its command streams and backing
allocations must already have trusted admission and reserved memory.

The record identifies prior evidence for NPU and shared-SRAM quiescence,
masked interrupt delivery, secure access attribution, inherited I2C setup,
remote-firmware exclusion, and independent watchdog recovery. Flags and evidence
numbers cannot establish those physical facts. They bind this operation to
private startup authority; applications cannot supply replacement records.
The handoff must exclude I2C clock, pin, and controller reconfiguration for the
startup lifetime. The transaction mutex cannot discover or stop an unregistered
configuration writer. General I2C clock/pin lease integration remains separate work.

Clock acquisition grants the common registry's NPU-local token with stable
fabric and selected-parent dependencies. A separate shared `A733_POWER_DCDC2`
token reserves the rail. This rail scope uses the existing registry and has no
clock ancestors or audio-profile effect. Every DCDC2 voltage or enable writer
must acquire exclusive authority; serializing I2C transactions alone cannot
preserve a voltage between transactions.

PCK-clock stability uses the existing global GPU resource manager. Its stable
PCK and oscillator lease excludes the manager's change guards. When the NPU
fabric uses PERI0, startup also holds that manager's parent lease. This prevents
legacy GPU-manager changes from bypassing the common PERI0 reservation.
The production binding consequently requires the established 400 MHz PERI0
output profile whenever PERI0 is selected by the fabric.

The shared-resource initializer creates one global GPU/PCK manager with native
operations and the common counter provider. GPU clients must reuse that manager.
Creating a second manager for the same resources would invalidate the exclusion
contract. The added GPU lease observer checks the exact client, token, mask,
and stable mode under its nonblocking metadata lock. It performs no time sample,
MMIO, or lease release.

The registry, shared locks, NPU binding, and GPU/PCK manager use one
`a733_counter_clock` object. Initialization rejects a different provider identity.
The existing native GPU and I2C transactions retain their bounded internal
physical-counter operations. Common samples before and after those operations
maintain the shared monotonic bound and reject late completion.

## DCDC2 readback establishes the configured rail profile

After acquiring all resource authority, startup takes the common PMIC/I2C mutex
and opens the inherited I2C controller. It reads AXP8191 address `0x36` in this
order:

| Register | Meaning | Admission requirement |
| --- | --- | --- |
| `0xff` | Register page | Page zero |
| `0x0e` | Chip identity | Value 3 |
| `0x0f` | Revision | Value 0 |
| `0x10` | DCDC enables | DCDC2 bit 1 set |
| `0x13` | DCDC2 voltage selector | Low seven bits equal 30 |
| `0xff` | Register page | Still page zero |

The source voltage table starts at 500 mV with 10 mV steps in this range, so
selector 30 represents 800 mV. Startup preserves unrelated enable bits and
performs no PMIC data writes. It closes the bus, verifies restored control,
idle status, and line state, then releases the mutex.

The stable DCDC2 lease remains held across settling. The board device tree gives
500,000–1,540,000 microvolts, a 250-microvolt-per-microsecond ramp constraint, and
a 1,000-microsecond enable delay. The full-range guard is therefore
`(1,540,000 − 500,000) / 250 + 1,000 = 5,160 microseconds`.
Startup waits at least that long after the first snapshot completes, then takes
and validates another snapshot. This verifies configuration stability under the
source timing assumptions. It does not measure analog voltage or qualify PMIC
fault behavior.

An invalid byte, failed transaction, or uncertain bus restoration retains the
PMIC mutex and all admitted leases. Later shared transactions refuse admission.
No subsequent NPU register access occurs. A failure after a cleanly restored
and unlocked transaction still retains the resource tokens.
The existing I2C transaction may perform its bounded STOP cleanup internally;
startup issues no later transaction after a failure.

## One startup call runs until a step waits

`a733_npu_startup_poll` runs acquisition steps in one call until a step has to
wait. A step waits on a PLL that is not yet ready, a pending PMIC provider
request, the 5,160-microsecond DCDC2 settle, a busy owner, or the platform's
own power and settle waits. When acquisition completes, the platform runs its
locked register pass in the same call. Each step repeats the entry checks of
a call: owner task, deadline, PMIC binding and health, PLL binding and the
adopted clock token. A further step runs only after the previous step
advanced the acquisition stage, and the stage only advances through six
values, so at most seven steps run in one call.

The owner task, the EL1 exception level and the scheduler state are checked
at the start of each step and again when the register mutex is taken.
The register and clock callbacks that the platform makes inside a step
take no ownership check of their own. Register access still requires the
register mutex, which is taken only after that check.

The two-core fixture counts calls with one call per tick. From the first
acquisition step to READY, startup takes 9 calls and 13 ticks; with PLL
adoption, PLL preparation and startup take 6 and 11 calls and 18 ticks. The
5,160-microsecond DCDC2 settle accounts for about six of those calls. The
native convolution application hashes its private model, constant and command
bytes once per startup call, so fewer calls hash fewer bytes. The
[measurements](designs/npu-platform-measurements.md) compare these counts with
the previous binding.

## Deferred activation authenticates the complete service registration

Platform integration uses this sequence:

1. Initialize the common counter, clock registry, and shared locks before scheduling.
2. Initialize the global GPU/PCK resource binding and copy authenticated NPU startup metadata.
3. Create and bind one privileged bootstrap task. Keep its handle and private storage alive.
4. Call `npu_service_init_deferred`, using `a733_npu_startup_prepared` as the activation callback, and bind application clients.
5. After scheduling starts, let the bootstrap task poll startup within its absolute deadline. Yield between pending polls.
6. Activate the service only after preparation succeeds, then admit client requests.

The [header](../include/a733_npu_startup.h) defines the resource and handoff
objects. Initialization requires disjoint private storage; service, callback,
clock, mutex, lease, and handoff objects must be excluded from device mappings.
Task deletion, handle reuse, or resetting a live startup object is unsupported.
Task abandonment retains resources.

Deferred initialization creates the static service queues and worker without
calling the backend. The inactive worker does not sample the backend clock or
poll the controller. Client sends refuse without consuming a ticket.
The existing `npu_service_init` API preserves immediate activation for callers
whose hardware preparation is already complete.

Activation checks the calling task before reading its exception level. Only the
copied bootstrap authority can proceed at EL1. An atomic claim prevents overlapping
prepared callbacks. The callback compares every registered backend function,
context, MMU metadata field, and command descriptor with its private copy.
It also requires successful platform preparation and exact live clock, rail,
and PCK tokens in healthy shared state.

Metadata-lock contention keeps activation pending. Uncertainty permanently
retains the inactive service. Successful activation publishes readiness with
release ordering before notifying the worker; the worker observes it with
acquire ordering. Notification cannot be lost between its state check and wait.
Unauthorized calls do not invoke the prepared callback. After activation or
retained failure, subsequent calls cannot invoke it.

Resource readiness does not validate command semantics or broaden model
admission. The compiled convolution retains its
[fixture-specific preparation limits](nbg-conv-loader.md). Physical execution,
numerical acceptance, and safe shutdown remain separate requirements.
No automatic rail change, shared PLL retune, or fault reset is introduced.

## Focused composition evidence

The [fixture](../checks/npu-startup.c) runs on two QEMU AArch64 cores with real
FreeRTOS tasks, priority-inheriting mutexes, the common physical-counter provider,
both resource managers, NPU platform logic, and deferred service activation.
Only physical register/I2C boundaries and native resource initialization are
modeled. The successful mode checks PMIC lock contention, actual readback-byte
validation, settling, exclusive-change conflicts, metadata copying, exact
command registration, observer contention, and cross-core client admission.

The failure mode supplies an 810 mV selector. Startup retains the clock, rail,
and PCK tokens plus the PMIC mutex. NPU platform I/O and all worker callbacks
remain absent. Repeated activation does not reenter the failed preparation.
Both modes used one archived image, each ran once, and both passed.
The success mode admits STATUS after activation; it executes no NPU command.
Existing controller, model, I2C, GPU-resource, and earlier registry evidence is
reused for unchanged lower-layer behavior.

The isolated AArch64 target is defined in
[cmake/npu-startup.cmake](../cmake/npu-startup.cmake). It compiles the production
startup/service code and unchanged native I/O adapters. Root build integration
must include this fragment after SMP and NPU setup.
The image, commands, outputs, dependency snapshots, and pre-build source copies
are retained under `~/.cache/sunstoneos/evidence/npu-startup-20261016/attempt-1/`.
The ELF SHA-256 is
`0bfbfcf6d8b38162f576a8627affe5cffe04e88d7f0dad27ff978f6d801ea453`.

The adjacent `source-reference.json` records the DCDC2 encoding and settling
arithmetic. Its board source is pinned to kernel revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`; its PMIC table/header are pinned to
bootloader revision `b791be842935b27268ae3d00e943a9075495f30a`.
Cold PLL provisioning, physical handoff proofs, production memory/command
registration, and board qualification remain required integration work.
No board access or performance measurement occurred.

## Register failure and release boundary

The platform publishes a failed readback or deadline check before invoking the
register-release callback. Startup retains the actual shared register mutex
when that failure is present, including when the MMIO callback itself returned
success. It also checks its deadline immediately before releasing the mutex.
Further polling and service activation perform no device access after failure.

A successful mutex release is irreversible. If the closing clock observation
first detects expiry after release, startup becomes terminal and retains its
clock, rail, and PCK leases; the register mutex is no longer owned. That outcome
is not evidence of a retained mutex. In either case the NPU service remains
inactive and there is no automatic retry or resource reclamation.

The focused follow-up uses one two-core image with three terminal modes:
incorrect readback after a successful write, expiry during the final read,
and expiry after successful unlock. The first two preserve the shared mutex;
the third verifies that it was released. Each mode checks retained leases,
refused service activation, and absence of later device callbacks. Evidence is
under `~/.cache/sunstoneos/evidence/npu-startup-retention-20261016/`. The earlier
success and invalid-rail results remain applicable and were not rerun.

## Local shutdown preserves startup authority

`a733_npu_startup_stop_poll` runs only on the original privileged bootstrap,
after the convolution client collects a successful controller CLOSE reply.
It preserves the startup binding's live PMIC-consumer, PLL-binding and
transferred-clock-token checks. Each poll uses the original absolute deadline,
register mutex and retained-resource failure path. Its bounded local shutdown
preserves shared SRAM, the ON power domain, PLL and rail leases.

The pinned VIPLite driver treats END followed by all-module idle
(`0x7fffffff`) as completion before leaving RUN state in `vip_drv_pm.c`.
Its wait-link stop also writes END and waits for that same idle mask in
`vip_drv_hardware.c`. The native controller adds a clear event check, interrupt
and MMU disable with readback, a barrier and a final idle check before CLOSED.
The local gate/reset sequence follows `npu_clk_uninit` in the pinned Allwinner
platform driver: module off, bus off, then AHB, AXI and core reset asserted.
This supplies the source-backed normal completion and stop contract. Gating or
reset cannot substitute for a failed CLOSE, deadline or completion check.

The source revision remains
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`, under
`bsp/drivers/npu/aw_nna_vip/vip2`. Physical engine behavior and inference return
remain unqualified. Resources and private memory remain retained through ROM
restoration; normal shutdown introduces no fault recovery or automatic retry.
