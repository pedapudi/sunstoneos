# DMA service buffers remain reserved for the image lifetime

The native DMA pool constructor binds the existing DMA0 service to linker-owned
write-back memory, the common clock registry, the common register mutex and the
physical counter. It prepares fixed RAM-copy jobs for an admitted subset of the
controller's sixteen channels. Hardware operation remains unqualified.

The reusable constructor is in
[platform/a733/dma_pool_session.c](../platform/a733/dma_pool_session.c), with its
contract in [a733_dma_pool_session.h](../include/a733_dma_pool_session.h).
[The copy application](../apps/dma-copy/main.c) is a separate integration entry.
It selects every admitted channel, runs one copy per channel, checks the copied
bytes and untouched padding, reaps each result, closes each channel and suspends.
The service and its resources remain allocated afterward.

## Each job owns complete descriptor and data pages

Construction acquires one parent allocation from the actual write-back pool.
Each job receives three disjoint 4 KiB pages: hardware descriptors, source data
and destination data. The native policy contains separate banks for those roles.
The service adapter admits only a complete registered page with its canonical
CPU address, physical address and role. Adjacent accelerator allocations,
partial-page mappings and altered physical addresses are refused.

The allocation retains its actual owner, slot, generation and complete extent.
Its allocator state becomes `DEVICE` before native adapter construction.
Every device callback checks the permanent parent record and the single admitted
session identity. The adapter uses the existing native address-translation and
cache-maintenance instructions. Its MMIO callbacks require the actual privileged
service worker and completed startup.

The parent never returns to the allocator. Successful completion, result reaping
and channel closure govern a job's buffer access, while the parent remains
`DEVICE`. The controller must observe its stop and drain conditions and complete
destination cache invalidation before the service reports a result. The client
consumes a successful result before submitting `REAP`.

An uncertain drain keeps the controller transfer and service buffer hold live.
The client must not inspect that destination or reuse its pages. A constructor
failure after allocation marks the parent `RETAINED`. Later faults preserve its
permanent `DEVICE` reservation without mutating allocator state.

## Boot construction precedes concurrent use

Call the constructor once at EL1 during serialized primary boot, before the
scheduler releases application tasks. The actual image mapping must already be
established on every participating CPU. All pool allocation and state mutation
occur during this interval. Later readers inspect one permanent slot that has
no writers; they do not invoke allocator APIs concurrently.

The session, callback state, borrowed counter, clock registry and register locks
must retain platform lifetime. They remain private and immutable after
construction, except for their documented owner transitions. Session copies and
changed lease identities are refused. These checks detect accidental misuse;
the current shared EL0 mappings do not isolate arbitrary memory corruption.

The constructor checks identity translations for its inputs and private state,
and excludes the pool from all such storage. Service protection also excludes
the rest of the image and the secure monitor reservation. Other device pools
receive no DMA mapping authority. The complete native mapping, boot image
reservation and absence of aliases remain platform prerequisites.

The designated bootstrap task runs the actual DMA startup state machine. It
borrows the image's common providers, obtains the real DMA0 clock lease and
performs register operations under the shared mutex. Activation requires the
existing authenticated startup callback. A bounded startup deadline includes
activation contention. Only the bootstrap and service worker receive EL1t
through `a733_dma_pool_task_privileged`; application clients remain EL0.

The acquired clock token remains private and permanently held. After activation,
the worker reads the actual atomic counter, registry and register-lock fault
indicators before each device or cache operation. An observed fault publishes a
failed session and invokes the nonreturning fatal handler, preserving every
allocation and active transfer. It performs no token-status retries or device
operations after observing failure. Ordinary registry contention does not alter
the permanent token or interrupt this bounded check.

## The copy image requires actual platform inputs

The application entry requires an externally supplied immutable
`a733_dma_copy_handoff`. The record must describe independently established
remote-user exclusion, prior drain, IRQ masking and watchdog recovery, with a
future absolute deadline. No default handoff is linked.

The cached platform must provide its actual primary preparation guard before
this entry runs. The guard checks that all cores reached the preparation barrier
and the kernel release flag remains unpublished. The dedicated copy image owns
one common counter, registry and lock set. A larger OS instead passes its existing
providers to the reusable constructor.

Central build integration selects this application entry, enables the linker
pool and applies the existing privileged-task patch to a private cached port.
It must rebuild all consumers against the same FreeRTOS and DMA service layouts.
The copy application does not replace the full hardware and mimus requirements.

## Capacity and remaining hardware work

The maximum sixteen-job layout consumes 48 pages, or 192 KiB. Existing prepared
NPU and GPU sessions consume 399 write-back pages, leaving 113 pages in the
initial 512-page pool. Adding all sixteen copy jobs leaves 65 pages, or 260 KiB.
A real CE session consumes another page. These figures describe allocation
capacity; they do not establish safe concurrent device execution.

One 640-by-480 NV16 camera frame requires 600 KiB. Full concurrent camera, video,
DMA and accelerator workloads therefore need additional qualified DRAM
reservations. This constructor does not expand memory capacity or infer the
board's usable DRAM from its advertised size. It explicitly refuses a pool
outside the initial image reservation until that profile's protected-image
ranges are integrated.

The current profile uses finite RAM transfers on DMA0. Continuous I2S capture
and playback must register their rings with this same service owner and narrow
memory policy. Peripheral shutdown must precede DMA drain where required.
DMA1, cyclic transfers, interrupts, IOMMU operation and wider peripheral modes
remain part of the [DMA scope](dma.md). Physical cache coherence, descriptor
fetches, bus completion, external-user exclusion and recovery need board evidence.

## Selected composition check

[The focused runner](../checks/dma-pool-session.py) builds one two-core FreeRTOS
image using the actual linker pool, native address translation, cache operations,
counter, clock registry, shared locks, startup, controller and copied-job service.
Only A733 registers and DMA bus movement are modeled.

One channel completes a copy and returns its buffer through the result/reap
protocol. A second channel retains a busy condition after disable and must
preserve its transfer and destination ownership. The same run checks adjacent
allocation refusal, full-page role checks, lease/copy refusal, the startup
authority requirement and absence of constructor MMIO. It does not rerun the
earlier DMA protocol or scheduler suites.

The final step retains the actual clock registry after activation. The next
worker operation must fail before further DMA or cache work and preserve the
uncertain channel's transfer and buffer hold.

The runner also compiles the production application entry and constructor with
the cached eight-core configuration. Those objects retain the unresolved real
handoff and platform guard requirements. The emulator execution does not qualify
the complete native image, A733 cache coherence or physical DMA completion.

The composed execution and production compilation passed on 16 October 2026.
The selected target built once and executed once, with no compile or runtime
correction. The executed ELF has SHA-256
`c2994c7a9f1de4825051742c0f4160f024dc53db256c53a3e87c76b15fd2feff`.
Input snapshots, build commands, production objects, linker layout and execution
logs are under
`~/.cache/sunstoneos/evidence/dma-pool-session-20261016/batch1/`.
The parent directory records the frozen stream dependency import and its
unused-helper removal before this build. No board or performance operation ran.
