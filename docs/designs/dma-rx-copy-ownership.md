# Keep cyclic receive copies outside retained DMA memory

Status: implemented and host-verified; physical cyclic ordering remains open.
The cyclic receive controller copies a completed period into caller-supplied
staging memory. Output admission excludes all retained allocations and metadata.
The previous check omitted transfer metadata and other channels' allocations,
allowing a copy to overwrite the metadata tracking a live DMA lease.

Before observing progress or touching hardware, output admission compares
the output range with the controller and every retained transfer. It rejects
overlap with transfer structures, segment and mapping arrays, descriptor storage
and payload allocations. A non-null transfer pointer defines retained ownership,
including channels in STOPPING or RECOVERY. Rejection returns minus one without
changing memory, controller state or callback counts.

The output address changes per call, so this check belongs at each copy.
It uses previously admitted immutable metadata and adds no translation, lock,
barrier, allocation or copy. At most 16 channel slots and 128 mappings per
retained channel are examined. A four-period RX ring with one four-period TX
peer requires eight payload comparisons. Existing invalidation, copy and
post-copy progress checks remain required.

Host checks compare callback counts and memory before and after rejected
outputs. Cases cover both transfers' metadata, descriptors, RX and TX buffers,
retained faults, pointer overflow, and an adjacent valid staging allocation.
The model also copies an independently initialized completed period into
valid staging memory. Physical cyclic ordering remains an audio qualification
requirement; this change establishes only CPU output ownership.

## Validation preserves the valid-copy cost

The [focused check](../../checks/dma-rx-copy-ownership.py) passes under
AddressSanitizer and UndefinedBehaviorSanitizer. Its 26 rejected outputs leave
controller state, metadata, buffers and callback counts unchanged. The previous
controller fails this regression. An adjacent private allocation receives the
expected 64-byte period.

The same valid-copy case, compiled against both controller versions, makes 15
register reads, two clock samples, one invalidation and two barriers. Neither
version writes registers or validates a translation during the copy.

The existing finite controller, descriptor, cyclic encoder and memory-policy
checks pass with sanitizers. The DMA service and twelve selected cyclic-audio
cases pass in FreeRTOS under cached QEMU. The service and audio fixtures explicitly
initialize the audio-route field to zero for I2S0. The integrated fixtures
preserve the route used by the earlier build-local validation.

Raw logs, exact commands, the baseline failure and before/after callback counts
are under `~/.cache/sunstoneos/build/memory-dma-1031/checks/`. Independent design
and code review found no blocking issue. Physical DMA0 copies, FIFO transfers,
interrupts, drain and cache visibility remain unqualified. DMA1 still lacks
source-backed CPUS request routing and physical ownership/security authority.
