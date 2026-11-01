# GPU jobs execute in the caller and complete through interrupts

This design assumes familiarity with FreeRTOS tasks, device drivers and shared
memory. A fence is the monotonically increasing firmware timeline value written
after a job finishes. A client command buffer (CCCB) carries compute and timeline
commands; the shared kernel command buffer (KCCB) carries their KICK command.

## Costs and acceptance budgets

The reference recognition workload spends 339.188 ms inside 154 GPU jobs in
entry `1793560651607761780-3cb221f5`. Separate firmware timestamps in entry
`1793561022387122671-06be5388` place 94.6% of aggregate job time inside device
scheduling and shader execution. Each completion interrupts the blocked service
task before its backup poll expires. The measured recognition penalty therefore
cannot be attributed to tick polling. The reference path still adds service
messages, command construction and output collection around every job.

The initial budgets are 5 microseconds of submission CPU time, 20 microseconds
from completion interrupt to the waiting task running, 50 microseconds for the
99th percentile empty-job round trip and 10,000 jobs per second. These are targets;
board measurements determine qualification. Explicit caller wait timeouts retain
their public meaning; completion never depends on those timeouts expiring.

## Firmware contract

The pinned reference is the Orange Pi kernel revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`, under
[`bsp/drivers/gpu/imagination`](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/gpu/imagination).
Its existing source records and SHA-256 values are in [GPU sources](../gpu.md).
The cache is `sources/gpu/board-2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/imagination/`
under `~/.cache/sunstoneos/`. This change adapts the MIT-licensed host interface;
the existing firmware runs on the GPU's MIPS processor, with its retained license.

| Contract | Reference |
| --- | --- |
| Compute command followed by UPDATE, command IDs 205 and 213 | `pvr_queue.c:697–705`; `pvr_rogue_fwif.h:1532,1564–1579` |
| One unused KCCB slot and serialized command publication before the doorbell | `pvr_ccb.c:194–204,243–283` |
| Clear MIPS interrupt before acquiring timelines, then recheck pending status | `pvr_device.c:275–283`; `pvr_fw_mips.c:176–186` |
| Firmware priorities low, normal and high, encoded 0, 1 and 2 | `pvr_context.h:23–27`; `pvr_queue.c:1071–1072` |

UPDATE has no explicit interrupt-request field. The reference interrupt handler
scans active timelines after ordinary completion interrupts. Hardware must confirm
that this firmware reliably interrupts for the submitted jobs. A missing interrupt
requires recovery. The sources
provide no bounded compute-preemption guarantee.

## Ownership and bounded work

The context retains its GPU mappings, program and firmware objects for its
lifetime. Four fixed job slots each retain independent PDS, CDM and argument
storage until the caller collects their fence. Submissions serialize the CCCB
and shared KCCB with the same context lock used by service control. Lock admission
is nonblocking; contention returns BUSY. No allocation or service request precedes
a successful job's kick. READY admits exact retained uncached KCCB regions and
the doorbell register once. Job publication uses aligned stores into those
regions, preserving the command, write-offset and kick barriers. A stale firmware
read cursor returns BUSY; an impossible advance latches its predicate and cursor
inputs before retaining the failed job. A batch publishes at most four jobs in
one caller entry, with one KICK per job.

Application tasks run at EL0. A fixed supervisor-call gateway enters EL1t on
the same task stack, calls the GPU dispatcher and restores EL0 before returning.
The port accepts entry and exit only at the gateway's immutable instruction
sites. IRQs and ordinary task preemption remain enabled during the operation.
The dispatcher verifies the admitted caller and the fixed argument's EL0
permissions. This uses the OS's existing trusted shared-address-space contract.

Batch submission returns the accepted prefix, including an attempted job whose
doorbell result is uncertain. Each returned fence remains context-bound.
Collection can select a specific fence or the oldest record. Context priorities
normal, low and high map to firmware values 1, 0 and 2, respectively.

The interrupt handler acknowledges the device before reading the uncached
timeline. It publishes the completed sequence and directly notifies registered
waiters whose fences have passed. Its bound is two clients; it does no cache
maintenance, event draining, ring scan, allocation or waiting. Interrupts arriving
after the timeline snapshot remain pending for another interrupt. Firmware events
and failures wake the service task for bounded slow-path handling.

Wait registration uses publish, recheck and block: a completion between
registration and blocking leaves a task notification pending. An interrupt before
registration is caught by the recheck. Output cache acquisition runs in the
collecting caller, after its fence, before returning its record. Notification
index 1 belongs to GPU waits; index 0 remains available to other task users.

The service retains startup, shutdown, thermal parking, recovery and cleanup.
It seals direct admission before power changes and drains accepted fences before
releasing device ownership. A watchdog deadline schedules failure handling only;
it does not turn a missing fence into successful completion. Failed isolation
retains every slot and buffer. The GPU remains powered while accepted work exists;
idle power policy and voltage/clock transitions remain with the lifecycle owner.
The slow watchdog checks CPU fence state once per tick. It never acquires a
firmware timeline to complete a job. A missing completion interrupt therefore
faults at the job deadline and follows the existing isolation protocol.

## Mapping, cache and validation

Mappings and initial MMU invalidation already happen once at context creation.
Per-job maintenance covers declared dirty input ranges and acquired output ranges.
CPU output translations remain read-only. Large dirty ranges can exceed the
submission budget independently of ring cost; measure ring publication separately
and include maintenance in the end-to-end CPU measurement. Compare retained
write-back payloads with uncached payloads on the board before selecting a mode.

Host checks must model delayed firmware reads, timeline writes and interrupts,
including coalescing, an interrupt during waiter registration, ring wrap/full,
multiple clients, timeouts and retained failures. They must reject a service hop
or tick poll in submission/completion. Board 3-3 runs a control first, then at
least 10,000 empty jobs, a pipeline, queue-add and application recognition. Record
image hash, revision and queue entry beside each distribution and throughput.
Timing instrumentation compiles out when disabled. Host timings do not establish
board latency or accelerator speedup.

## Measured limits

The zero-element vector shader still loads its argument block. It measures a
minimal real compute dispatch with a fence, rather than a timeline-only command.
On port 3-3, shortening KCCB publication reduces median submission wall time
from 13.458 to 3.833 microseconds. The same 10,000-job workload retains a roughly
104-microsecond median round trip and 9,540 jobs per second at depth four.
These observations do not isolate a firmware lower bound. The
[GPU status](../status/gpu.md) binds distributions to images and queue entries.

The measured IRQ C-entry to notification return has a 2.292-microsecond median,
8.167-microsecond 99th percentile and 118-microsecond maximum. It excludes the
vector/dispatcher prefix. Submission wall time includes preemption and counter
overhead; its 51.875-microsecond maximum does not establish a five-microsecond
CPU bound. The fixed GPU loops have bounded operation counts, but FreeRTOS's
shared notification and scheduler locks still need a bounded contention proof.
The CPU timing record on `work/latency-1101`, `docs/status/latency.md`, records
long waits on those locks. It does not identify the origin of this GPU run's
tail samples.

The initial 50-microsecond round-trip, 10,000-job throughput and 20-microsecond
maximum wake targets remain unmet. Default payloads remain write-back: uncached
payloads save only 0.208 microseconds at the median in the paired measurement
and provide no measured tail improvement. Idle timeout and hysteresis remain
an integration requirement for the lifecycle owner.
