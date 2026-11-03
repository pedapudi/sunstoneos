# Keep cyclic receive copies outside retained DMA memory

Status: **host-tested**. Physical cyclic ordering is open.

The cyclic receive controller copies a completed period into caller-supplied staging memory. Output admission excludes all retained allocations and metadata.

Before observing progress or touching hardware, output admission compares the output range with the controller and every retained transfer. It rejects overlap with transfer structures, segment and mapping arrays, descriptor storage and payload allocations. A non-null transfer pointer defines retained ownership, including channels in STOPPING or RECOVERY. Rejection returns minus one without changing memory, controller state or callback counts.

The output address changes per call, so the check runs at each copy. It uses previously admitted immutable metadata and adds no translation, lock, barrier, allocation or copy. At most 16 channel slots and 128 mappings per retained channel are examined. A four-period RX ring with one four-period TX peer requires eight payload comparisons. Invalidation, copy and post-copy progress checks remain required.

## Checks

The [focused check](../../checks/dma-rx-copy-ownership.py) runs under AddressSanitizer and UndefinedBehaviorSanitizer. It rejects 26 outputs (both transfers' metadata, descriptors, RX and TX buffers, retained faults, pointer overflow) and compares callback counts and memory before and after each rejection. An adjacent valid private staging allocation receives the expected 64-byte period. A valid copy makes 15 register reads, two clock samples, one invalidation and two barriers, and writes no register and validates no translation. The finite controller, descriptor, cyclic encoder and memory-policy checks pass with sanitizers, and the DMA service and twelve selected cyclic-audio cases pass in FreeRTOS under cached QEMU. Figures: record `dma-rx-copy-ownership-host` in [the evidence file](../evidence/dma-rx-copy-ownership.jsonl).

## Open work

1. Physical DMA0 copies, FIFO transfers, interrupts, drain and cache visibility are unqualified; physical cyclic ordering remains an audio qualification requirement. This design establishes only CPU output ownership.
2. DMA1 lacks source-backed CPUS request routing and physical ownership and security authority.
