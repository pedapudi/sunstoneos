# SD owners wait for requests and controller completion

Status: implemented and host-tested. One native read-only workload passes with
interrupt completion, exact file verification and zero retired-loader wakes.
Open-device idle, storage writes and fault recovery remain unqualified. The
[owner evidence](../evidence/sd-owner-events.jsonl) defines the measured scope.
The intended readers are FreeRTOS driver developers familiar with the SD
platform and its retained-fault
contract in [the card reference](../sd-card.md).

The SD service owns all controller access. Its idle task waits indefinitely for
a request notification. A caller opening that service waits for the ready
publication or its existing ten-second absolute deadline. The caller uses the
runtime notification index selected for its task. Application index zero remains
available to the application. Asynchronous loader requests notify the loader
when they finish; their poll callback blocks until that publication.

The read-only loader owns the controller directly. It uses the existing SMHC0
interrupt handler for completed DMA reads. INTID 193 is level high; the handler
accumulates masked status, acknowledges the same raw bits and notifies the
owner. This is the sequence of the pinned board kernel's `sunxi_mmc_irq`, as
cited in [the card reference](../sd-card.md#2-layers-and-ownership). The loader
rechecks completion after each notification. Its read deadline remains 500 ms.
A missing completion notification ends at that deadline and preserves the
platform's existing fault and descriptor ownership rules.

PMIC startup publishes a notification to the subscribing loader. Identification,
shared-provider requests, GIC register-write propagation, controller reset, FIFO
retirement and card busy use precise timer deadlines where the current driver
interface exposes no completion notification. Core-declared timer
holds retain their duration. Other register checks use 100-microsecond retry
deadlines, matching the core's initial busy-retry cadence and bounded by the
operation deadline. They never form a tight poll loop.
The platform stops its bounded core-step batch as soon as an interrupt or timer
hint appears. GIC propagation retains its existing 100 ms availability budget.
Each loader remains suspended after completion because the deadline facility
retains its registered task handle.

No periodic wake is required while either owner is idle. The host regression
observes zero idle wakes over one second and no completion wake before an
injected interrupt. The startup wait must have one timer deadline and no
50 ms health polls. Host models establish software wait behavior; physical
evidence applies only to the recorded image, card input and conditions.

The read-only loader publishes the existing storage statistics interface after
its close proof. The statistics include completed waits, sleep duration, owner
placement, interrupt entries and interrupt waits that expire. Time after the
loader retires counts as asleep. Reading this record performs no device access.

The short region diagnostic uses the existing 40-second manifest with the normal
application source. It returns the normal success tag after the file passes its
digest check, the memory and task-switch measurements finish, and the retired
loader completes a one-second idle observation. Two storage-statistics snapshots
occupy reserved bytes of its existing 512-byte result. The diagnostic decoder
checks both snapshots and the interrupt totals. The public application ABI and
runner capture geometry are unchanged.

`checks/event-sd.py` exercises the production waits and retirement statistics
with FreeRTOS and the precise deadline facility. `checks/event-sd-app.py` checks
the application control flow, native result layout and decoder with modeled
storage and memory effects. These checks establish no physical qualification.
