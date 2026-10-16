# GPU workload tables have native publication and reclamation

The [native workload-memory adapter](../platform/a733/gpu_vm_memory.c) connects
the existing [GPU table builder](gpu-vm.md) to AArch64 translation checks and
cache maintenance. One privileged FreeRTOS worker exercises the adapter in QEMU.
Physical GPU translation and cache visibility remain unqualified.

The platform supplies reserved table storage, immutable physical-page addresses,
and an uncached 32-byte firmware memory-context record. Tables use writable,
inner-shareable Normal write-back CPU mappings. The context uses writable,
shareable Normal Non-cacheable memory and fits within one page. Initialization
verifies each CPU mapping against its declared physical address using the
processor's address-translation instructions.

The adapter requires EL1 execution with enabled MMU/data cache and no stage-two
remapping. The existing CPU helper preserves the translation-result register and
interrupt masks. These checks cannot prove allocator ownership or exclude hidden
aliases. The platform must reserve RAM, pin mappings, and exclude every conflicting
allocation, including cacheable aliases of the context page.

## Publication advances one table page per poll

Initialization copies descriptors without modifying backing or performing cache
maintenance. Build accepts only an offline allocation. It validates mappings,
rejects workload data overlapping the context's entire physical page, and calls
the existing table builder. The builder also excludes table backing from workload
data. An invalid mapping plan leaves outputs unchanged; lost CPU mappings latch
failure.

Successful build enters `PREPARED`. Publication then validates and cleans one
4 KiB table page to the point of coherency per poll. A final step checks the
uncached context and completes a full-system barrier. Only the resulting
`PUBLISHED` state permits the caller to attach the tables to a device context.

The owner freezes mappings and bytes during publication and device use. The GPU
must only read table pages. The adapter allocates no memory and does not register
the firmware context, submit jobs, publish workload data, or invalidate GPU
translation caches. The service must enforce deadlines between polling steps;
initial validation and offline construction remain synchronous operations.

## Reclamation requires independent isolation

Reclamation begins only after another component proves that GPU access is blocked
and outstanding transactions have drained, and detaches the firmware context.
The adapter does not establish those conditions. It validates and invalidates one
table page per poll before returning to `OFFLINE`. It leaves the uncached context
unchanged, preserving fields that firmware may have written.

A mapping or cache-operation failure retains every allocation and latches
`FAILED`. Later calls perform no translation, cache maintenance, or shared-memory
access. There is no forced-release or restart path. Power-off alone cannot
authorize reclamation.

## One focused emulator session covers native operations

The [fixture](../checks/a733-gpu-vm-memory.c) runs at EL1t under the two-core
FreeRTOS port. It rejects cached context memory, incorrect CPU/physical mappings,
absent mappings, read-only tables, and workload aliases of the context page.
It integrates the actual builder, publishes three table pages, preserves a
simulated firmware context update through reclamation, and rebuilds offline.

During a second publication, the fixture makes table storage read-only after
one page completes. The next poll fails and retains its cursor and backing.
Terminal calls also refuse work when the fixture substitutes an unmapped table
address. Allocation guards remain intact. The existing table-format reference
checks were reused because the builder did not change.

QEMU executes the actual translation and cache instructions, but does not model
physical GPU access or prove board cache coherence. No GPU firmware or workload
executes in this check. Native compilation produces 1,980 bytes of code/read-only
data and no static writable data; the largest direct frame is 96 bytes, excluding
callees.

Build only the affected target when inputs change:

```sh
cmake --build "$HOME/.cache/sunstoneos/build/qemu-virt" --target gpu-vm-memory-model
```

The exact QEMU command, executable, source hashes, and result are retained under
`~/.cache/sunstoneos/evidence/gpu-vm-memory-20261016/`. The target uses the existing
GPU emulator platform configuration. Live table edits, invalidation integration,
firmware context attachment, data-buffer ownership/publication, and physical
qualification remain required work.
