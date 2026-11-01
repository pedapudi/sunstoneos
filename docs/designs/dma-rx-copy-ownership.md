# Keep cyclic receive copies outside retained DMA memory

The cyclic receive controller copies a completed period into caller-supplied
staging memory. Its output admission excludes its own raw buffers, descriptors
and controller, but omits transfer metadata and other channels' allocations.
A copy can therefore overwrite the metadata used to track a live DMA lease.

Before observing progress or touching hardware, output admission will compare
the output range with the controller and every retained transfer. It will reject
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

Host checks will compare callback counts and memory before and after rejected
outputs. Cases cover both transfers' metadata, descriptors, RX and TX buffers,
retained faults, pointer overflow, and an adjacent valid staging allocation.
The model will also copy an independently initialized completed period into
valid staging memory. Physical cyclic ordering remains an audio qualification
requirement; this change establishes only CPU output ownership.
