# GPU operating-point changes in the OS

Status: implemented and checked in software. The design baseline is `f10a04d4`;
the final build includes the reviewed NPU deadline interface at `5ef97f43`.
This change adds the GPU to the existing OS performance command and thermal
owner. The default remains 1008 MHz at 960 mV. The optional low point is
600 MHz at 800 mV, from the same vendor `vfdefault` column. CPU, GPU and NPU
retain their own device state and use the existing shared providers. No model,
inference engine, placement policy or native toolchain change is needed.

The GPU remains firmware-driven during a normal operating-point change. A
software timeout neither establishes GPU idle nor permits buffer reuse. This
design does not authorize hardware execution or establish physical DVFS timing.

## Effective source and actual missing mechanisms

The reference kernel is `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`.
Its BXM build defines `SUPPORT_LINUX_DVFS`; `sysconfig.c:356–369` selects
`SetFrequency`, `SetVoltage` and `bIdleReq = IMG_TRUE`.
The active [devfreq target](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/modules/gpu/img-bxm/linux/rogue_km/services/server/env/linux/pvr_dvfs_device.c#L97)
has SHA-256 `1f32114c38a92aa8c14de9f7e63a83e601911c9938a75f65b803b44bda4fb420`.
It is cached under `~/.cache/sunstoneos/sources/gpu-performance-2ac08e8c/`.
This is a host reference, licensed MIT OR GPL-2.0-only; it does not run in
SunstoneOS. It is pinned with the existing GPU references in `sources.lock.json`.

| Effective reference | Required native behavior |
| --- | --- |
| `pvr_dvfs_device.c:97–181`; `power.c:1313–1420` | Keep one power owner across acknowledged idle, voltage/rate change, firmware notification and idle cancellation |
| `rgxpower.c:1519–1670` | Clear the power sync, send FORCE_IDLE, require a fresh acknowledgment and FORCED_IDLE state; CANCEL_FORCED_IDLE also needs a fresh acknowledgment |
| `sunxi_platform.c:405–485`; `ccu-sun60iw2.c:1020–1043` | Above 800 MHz use PLL_GPU0; 600 MHz uses PERI0's 600 MHz output, selector 2 and divider 1; preserve PLL_GPU0 while using PERI0 |
| `rgxpower.c:1160–1215`; `rgxtimecorr.c:275–338` | Publish the runtime clock word and a new time-correlation record before CORECLKSPEEDCHANGE and idle cancellation |
| `axp2101-regulator.c:383–408`; `sunxiSetVoltage` | After a 160 mV change wait at least 1200 microseconds before the next dependent operation |

The remaining references are already cached under `gpu-fw-start-ddk-2ac08e8c`,
`gpu/bxm-2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` and `dma-platform`.
The existing [performance design](performance-modes.md) records the operating
points and source selection. The alternate `SUPPORT_PDVFS` implementation is
corroboration, not the active-path evidence.

The baseline had three contracts that prevented a correct switch. The GPU supply consumer
retained a shared DCDC4 token and a completed PMIC request at exactly 960 mV;
it could not write a selector. The GPU service had no live idle/cancel sequence.
Finally, the performance engine assumed HOLD made no device writes and ignored
RELEASE errors. The changes below address those contracts together.

## One owner from admission through completion

Performance-enabled GPU images reserve DCDC4 exclusively under the existing
GPU firmware owner before first power-on. Fixed-rate images keep their current
shared consumer. This requires a small, explicit DCDC4 mode in the existing
consumer and platform, not a general lease-upgrade API. Upgrading the current
token would otherwise have to preserve its completed-request borrow and
power-domain pin while changing ledger identity. Reserving the intended writer
at admission avoids that additional mechanism and any unowned interval.
Acquire it only after the power-domain owner's initial supply-verification pass
has completed successfully for DCDC4. That pass needs a shared DCDC2/DCDC4
token. After observing its published success, wait for its token release under
the acquisition deadline; an earlier permanent exclusive token would prevent
verification and GPU ON. A failed verification refuses GPU startup.

The GPU platform remains the only DCDC4 writer. It retains the token through
healthy OFF/reopen and faults. A completed sample remains borrowed until the
next serialized sample or write; uncertain provider work is never reaped.
Published voltage changes only after write readback, settling and a final fresh
snapshot. NPU DCDC2 and CPU DCDC3/5 are disjoint. CPU-only performance readback
must obtain GPU voltage through this owner once it owns DCDC4 exclusively;
acquiring another shared DCDC4 token would deadlock. Before GPU power-up, the
ordinary read-only snapshot remains available.
The PMIC DCDC4 write predicate admits the existing high-row voltages and
the explicit 800 mV low point, preserving the high-row helper's meaning. Keep the resource manager's initial
960 mV acquisition proof; live low-point authority belongs to the performance
owner, and low-mode reopen first establishes the high startup supply.

The GPU also holds a stable PERI0 clock lease for the performance-enabled
lifetime, under its existing identity, before selecting low mode. Verify the
PLL_REF ancestry, the 1200 MHz PERI0 2X output, and the 600 MHz branch's
software/automatic gates (CCU `+0x1908`, effective status `+0x1988`, bits 11 and
9). Do not reprogram PERI0: other devices use it. Existing GPU LOCAL ownership
protects PLL_GPU0 and the core mux; the fixed GPU resource set does not protect
PERI0. PCK, fabric and domain ownership remain unchanged.

The thermal task remains the performance-policy owner. Bind it to a second
existing GPU service client before scheduler start; thermal already starts
before `gpu-linear` in `os_applications`. Its nonblocking performance actions
use the existing request/reply queues with an immutable action, value, epoch
and absolute deadline. The GPU worker alone drives firmware, clock and supply
changes. No new task, scheduler, heap allocation or request queue is needed.
Replies distinguish accepted work from completed work; late replies cannot
turn an expired operation into success.

## Bounded live transition

1. HOLD closes new Mimus submission with an atomic slot reservation. A producer
   that already reserved or submitted its slot wins the race and completes
   under its existing deadline. The worker continues normal IRQ, event and
   completion processing until that job ends, then owns the empty slot. A
   recognizer encountering the performance hold waits within its existing
   bounded caller operation instead of treating it as CPU fallback. Previously
   lent sums remain mapped and unchanged while CPU readout workers finish.
2. With job admission closed and no GPU job outstanding, clear the power sync,
   send FORCE_IDLE through the existing kernel CCB, and require fresh sync 1
   plus firmware FORCED_IDLE. Continue bounded control/event processing; the
   interrupt handler still only masks/wakes, and never programs clocks. HOLD
   completes only after this acknowledgment. A timeout after publication is a
   retained failure, not a clean busy refusal.
   Record potentially device-writing HOLD ownership when FORCE_IDLE is
   published, even before HOLD returns DONE. Engine failure accounting must
   preserve that ownership when the acknowledgment never arrives.
3. On a downshift, select PERI0 600 MHz with the existing register lock, require
   the update bit to clear and decode the live clock, then compare/write DCDC4
   to 800 mV. On an upshift, compare/write 960 mV, settle and snapshot first,
   then select the still-enabled, verified 1008 MHz PLL_GPU0. Revalidate held
   ownership and clock state immediately before lowering voltage. Every PMIC
   submission, reap, register operation and wait uses its captured deadline.
4. RELEASE writes the actual rate into runtime configuration, publishes a new
   time-correlation record, submits CORECLKSPEEDCHANGE, then clears the sync
   and submits CANCEL_FORCED_IDLE. A fresh cancellation acknowledgment
   establishes resume, as in `RGXCancelForcedIdleRequest`; it does not impose
   an unsupported ON-only firmware-state predicate. Ordered CCB completion settles
   the preceding clock notification too. Only then reopen the Mimus slot.
   A failed or late resume retains the slot, GPU backing, command identities,
   leases and failed service. No reset, rollback or CPU fallback follows.

The selected native `pvr_rogue_fwif.h` is the command/layout authority:
POW is 107 and CORECLKSPEEDCHANGE is 204. The DDK uses different command IDs
and supplies the transition order only. Native runtime configuration places
`core_clock_speed` at byte 12. Its time-correlation structure has four
64-bit fields followed by the 32-bit rate and reserved word.

The time-correlation ABI is already reserved: 256 records of 40 bytes at the
start of the utilization allocation, followed by the sequence word. Each
record contains host nanoseconds, monotonic nanoseconds, GPU CR timer, the
conversion factor `(256000000 << 20) / (rate_hz / 1000)`, rate and reserved
word. Read the 48-bit CR timer at GPU `+0x160` adjacent to the architectural
counter sample. As in `RGXReadHWTimerReg`, compare the duplicated bit 31 at
bit 63 and reread once on rollover; inconsistent second observation fails.
Publish the complete record before its sequence with the
existing non-cacheable shared-memory ordering. Verify these offsets against
the pinned firmware headers and native resource extents. There is no new
firmware buffer. Host calibration history is unnecessary for the two exact
programmed rates; no measured-frequency claim follows from register decoding.

HOLD retains the existing shared 1.5-second accelerator bound. Each following
action uses the existing 50-millisecond accelerator bound, capped by the board
session and never renewed while polling. A GPU transaction also captures an
outer bound covering HOLD and its voltage/rate/resume actions, so a missing
next action cannot leave a silently live hold. The existing two-second caller
bound is not extended. PMIC settling is 1200 microseconds within the captured
action bound; timer regression and equality at the deadline fail.

The performance engine must wait for asynchronous RELEASE and clear a held bit
only on DONE. A FAILED action stops the plan, preserves held ownership and
publishes STOPPED before the thermal task fails the board. A clean refusal
before a command is published may reopen its reserved slot. A thermal change
between successful actions stops further voltage/rate changes; acknowledged
resume at the known current safe rate remains necessary before normal healthy
CLOSE. In particular, a raised rail at the old low rate or a lowered rate at
the old high voltage is safe to resume and report as mixed. Uncertain clock,
rail or firmware state never reaches this cleanup.

## Startup, thermal parking and host composition

The existing boot performance request selects CPU domains only. Initialize
the GPU's selected point directly from the same boot-mode option: HIGH by
default, or LOW when explicitly selected. Its stopped-firmware startup path
applies that selection independently, so the CPU-ready wait never depends on
GPU OPEN. Runtime GPU switches require a healthy READY session. Thermal
WARM still parks the GPU through the existing acknowledged CLOSE, while
critical/fatal handling retains incomplete work. The recognizer's CLOSE must
wait for an accepted performance action to resolve, under its own unchanged
bound; a new performance action cannot race an accepted CLOSE.

Low mode must survive healthy thermal OFF/reopen. OFF can gate the current
600 MHz source, then normalize the stopped mux under retained ownership.
The existing high-point power-on sequence may run while firmware remains
stopped, with 960 mV established first; for a selected low mode, switch and
settle to 600 MHz/800 mV before starting firmware. Serialize the selected clock
into the new firmware state before startup. Do not start firmware configured
for 600 MHz while hardware is still at 1008 MHz. Preserve the ordinary high
startup sequence and the existing physical cold-start prerequisites.

Use the existing PERF, CPU_CLOCK, GPU and A76 options and USB operation 8.
Allow their supported recorded two/eight-worker compositions, including NPU,
without changing session sizes or weakening TOP, B1, frozen GPU evidence or
cold-boot authority. This increment uses the existing supervised performance
service. Resident GPU images retain their fixed-rate support and the existing
PERF refusal pending separate cold performance admission and lifecycle work.
Add linked GPU performance capability and point metadata; host preparation
and frozen verification compare it with the actual selected features.
The exact-image GPU entry also requires explicit runtime authority for both
600/1008 MHz and the exclusive DCDC4 lease at 800/960 mV; fixed-rate entry
evidence alone does not admit live performance changes.
Old GPU images remain fixed-rate and cannot
gain performance authority by changing manifest booleans. The existing tool
already names GPU in the performance block mask; it must report actual
readback and terminal refusal rather than assuming ownership.

## Implementation scope and focused evidence

GPU ownership is released by the resident GPU worker at `cd05c326`; this branch
has integrated its `f10a04d4` result. Changes belong in the existing GPU service,
backend, memory access, platform and supply consumer, with a concrete GPU
clock-transition helper. OS changes are limited to GPU/performance integration,
the thermal task's pre-scheduler client binding, and the corresponding
CMake/link/host capability checks. The existing performance engine needs the
bounded RELEASE/failure contract correction. No other accelerator lifecycle
or model code changes are planned.

The added work occurs only on a requested transition, readback or thermal
reopen. Budget three firmware commands per live transition, one 40-byte
correlation record plus its sequence and runtime word, one clock-mux update,
one PMIC compare/write and final snapshot, and the existing bounded drain.
No workload-buffer copy is added. The results below account for executable, read-only and mutable bytes,
stack headroom, queue sizes and per-job admission cost. Reuse the
accepted recognition, eight-worker join and ordinary bounded-caller evidence.

Focused checks exercise the actual controller/provider path for both
directions, both initial-verifier/GPU acquisition orders, an outstanding job
and racing submission, thermal interruption,
idle denial or missing acknowledgment, PMIC compare/readback/settle failure,
clock-update mismatch, late notification/cancel and retained cleanup. Include
the actual performance engine's asynchronous RELEASE and failure cases plus
low-mode healthy OFF/reopen. One current combined native image establishes
effective flags, resource bounds and linked host capability. Focused host
mutations cover false capability, stale entry and unchanged fixed-rate images.
No inference replay, broad benchmark or hardware run is required for this
software increment.


## Software results and costs

The selected image is supervised recorded recognition with eight admitted A76/A55
workers, CPU_CLOCK, GPU, ShuffleNet model 3 and PERF_BOOT=low. Its SHA-256 is
`b55a710b2bff0dcb77e5ff1b66eb75b193e3c281c67643bcc73e667b48f49a11`.
The model, weights and worker placement are unchanged. Its linked contract is
1176 bytes; GPU metadata records both supported points and the selected low boot.
A separately compiled GPU translation unit verifies the default high boot record.
The accepted fixed-rate combined image passes the current host validator.

| Focused evidence | Result |
| --- | --- |
| Selected native header probe | Command IDs, 88-byte command length, power/clock payload offsets, firmware state and all time-correlation fields match |
| Firmware controller | Nine fresh-acknowledgment, ordering, publication and retained-failure cases |
| Actual PMIC/platform providers | Ten cases, including both initial-verifier schedules, both rate/voltage orders, low OFF/reopen, public supply-end renewal, selector mismatch and settle expiry |
| Shared performance engine and OS owners | 33 engine cases, 21 CPU-owner cases and 22 CPU/NPU-owner cases; asynchronous RELEASE errors retain ownership |
| Actual FreeRTOS GPU service | Five cases: transition/reopen, accepted-job drain, missing idle acknowledgment, missing cancel acknowledgment and clean drain refusal |
| Actual linked OS bridge | Six cases covering timely output, deadline equality, late reply, wrong task, revocation and an invalid pending reply |
| Host preparation and frozen entry | 17 outcomes, including both-record-removal refusal; exact points, boot mode and runtime authority are bound to the image |

These are software fixtures. The service fixture uses synthetic data for one
admitted job to check slot reuse; it does not replay recognition or substitute
production models. Earlier service cases remain valid after adding the drain
case and cost instrumentation: their production service, clock and memory code
is unchanged. Normal fixed-rate OFF/reopen also passes against the changed
supply-consumer implementation.

Against the accepted eight-worker CPU/GPU/NPU image without PERF, allocated
executable bytes increase by 28,480 and read-only bytes by 224. Mutable input
sections increase by 1,612 bytes, absorbed by existing output-section padding;
the allocated mutable extent is unchanged. This comparison includes the existing
CPU/NPU performance owner and the reviewed NPU deadline dependency. GPU-specific
input code contributes 13,168 bytes; shared performance code contributes 10,396;
other CPU/NPU/USB integration contributes 3,692. Code alignment accounts for the
remaining 1,224 bytes. The full image has 734,601 bytes left in its 2 MiB CODE region.
The 4 MiB heap, two 2 MiB GPU pools, 8 MiB NPU pool and fixed inference regions
keep their bounds. No new task, stack, heap allocation or workload-buffer copy
is introduced.

The native GPU service grows from 53,544 to 54,040 bytes. Its request item grows
from 56 to 72 bytes and reply item from 512 to 520 bytes, within the existing
fixed queues. The service fixture retains 28,384 untouched bytes of its existing
32,768-byte worker stack after transition and reopen. Its modeled HOLD takes
3,506 microseconds and RELEASE 4,733; platform callbacks in that fixture are
modeled, so those times do not establish hardware latency. Actual PMIC ordering
and settling are checked separately through the real provider state machines.

Normal submission retains its existing single slot compare/exchange. The new
HELD return branch adds no atomic operation on successful submission. Each
ordinary READY advance adds two `performance_controlling` state checks, one
no-ticket branch in `performance_step`, and one atomic release store to the
published readiness word. No new device access occurs for an idle performance
owner. A requested transition
adds the three firmware commands and one 40-byte correlation publication
specified above. GPU voltage change uses one compare/write request and one
fresh settled snapshot; the engine's final GPU readback takes another snapshot.
All provider waits retain their original absolute transaction bound.

Evidence is frozen under
`~/.cache/sunstoneos/evidence/resume-gpu-performance-20261026/`.
Physical live GPU frequency changes, thermal drain/recovery and heterogeneous
performance remain unqualified. Resident performance admission is a separate
integration; this change preserves the resident fixed-rate boundary.
