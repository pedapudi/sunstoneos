# GPU jobs execute in the caller and complete through interrupts

This design assumes familiarity with FreeRTOS tasks, device drivers and shared
memory. A fence is the monotonically increasing firmware timeline value written
after a job finishes. A client command buffer (CCCB) carries compute and timeline
commands; the shared kernel command buffer (KCCB) carries their KICK command.

## Costs and acceptance budgets

The supplied board baseline spends 338 ms submitting and collecting 154 jobs,
or 2.195 ms per job. It contains no latency distribution or queue entry identity.
The current implementation sends each submission through the GPU service task,
which advances the transport and posts a completion queue record. Its bounded
bursts and tick waits add scheduling to both halves of a job.

The initial budgets are 5 microseconds of submission CPU time, 20 microseconds
from completion interrupt to a runnable waiting task, 50 microseconds for the
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
is a recovery failure, not permission to restore completion polling. The sources
provide no bounded compute-preemption guarantee.

## Ownership and bounded work

The context retains its GPU mappings, program and firmware objects for its
lifetime. Four fixed job slots each retain independent PDS, CDM and argument
storage until the caller collects their fence. Submissions serialize the CCCB
and shared KCCB with the same context lock used by service control. Lock admission
is nonblocking; contention returns BUSY. No allocation or service request precedes
a successful job's kick. A batch publishes a bounded number of jobs in order.

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
