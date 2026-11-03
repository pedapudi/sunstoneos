# NPU resources: declared, admitted and reserved

Status: **qualified on the board** for declarations, admission, reservation, run-time graph registration, 31 slots with a ring of depth 64 and backpressure, tensor links after the scheduler starts, and externally supplied models ([NPU status](../status/npu.md); records in [npu-resources.jsonl](../evidence/npu-resources.jsonl)). **Open:** service priority between applications, and a pool in upper memory.

An application states what it needs from the NPU in its manifest. The operating system admits the declaration against what the NPU and the memory offer and reserves the resources at admission. An application loads graphs it hands over at run time, with no build-time list.

## Principles

1. **No count of completed jobs.** A robot submits jobs for as long as it runs. A session never fails because it has run a certain number of jobs. The job counters are statistics; the `jobs` field of the session record only sets how many jobs the diagnostic client runs.
2. **Declare, admit, reserve.** The application declares what it keeps resident and how deep its queue is. The operating system admits it only if the declaration fits the real capacity and reserves the resources at admission. A running application cannot fail on a resource it declared, and nothing is allocated on the job path, so job latency stays deterministic.
3. **Limits come from a source.** Each fixed array or limit derives from a bound (memory, page-table entries, command space, a hardware field) or does not exist beyond memory. A compile-time array is sized from the declarations and the bound, not from a round number.
4. **Backpressure on a full ring.** When the declared in-flight depth is full, a blocking submit waits for a completion fence and a non-blocking submit returns busy (`ss_npu_queue_submit` returns 0). Neither ends the session.

## What bounds each number

| Number | Where | Bound |
| --- | --- | --- |
| Slots, clients, queue handles (31) | `NPU_SERVICE_CLIENTS` (`include/npu_service.h`), `NPU_RUNTIME_SLOTS`, `SS_NPU_QUEUE_MAX_HANDLES` | A client's command mask has 32 bits and command 0 is the bootstrap. Each slot owns a command buffer, a read-only region, an input region and an output region; memory bounds the rest. |
| Ring depth | `SS_NPU_QUEUE_MAX_DEPTH` (64 default) | Array size in `runtime/npu_app_queue.c`, no hardware limit. The image is built with the manifest's `in_flight` (`-DSUNSTONE_OS_NPU_QUEUE_DEPTH`); a session that declares more is refused as "image tables". |
| Tensors per graph (8 inputs, 8 outputs) | `NBG_ASSEMBLY_MAX_TENSORS` (`include/nbg_assembly.h`), `SS_NPU_QUEUE_MAX_TENSORS` | The loader's own choice: the format does not bound them. It follows from the size of `struct nbg_assembly_layout` (24 bytes per tensor, kept per graph). An image is built for the largest graph its application declares (`-DSUNSTONE_OS_NPU_MAX_TENSORS=N`); a larger graph is refused at admission. |
| Operations per graph | none | No array is sized by it. The model container (at most 256 MiB) and, through the command stream, the slot's memory bound it. |
| Command space | `NPU_RUNTIME_COMMAND_BYTES`, `A733_NPU_FETCH_BYTES` | One fetch is at most 524,280 bytes: the doorbell word carries the fetch size in 8-byte units in 16 bits (`vipdMAX_CMD_SIZE` is 65535 * 8). A longer stream is cut between operations into segments joined by the vendor's LINK command ([command formats](../hardware/npu-command-formats.md), "Command segments"), so stream length is bounded by the slot. Each slot's command allocation is the stream of the largest resident graph (with its links) plus one END pair, rounded to a page, and the doorbell word is the allocation in 8-byte units. A reservation that also holds run-time graphs uses 524,288 bytes per allocation, because a later graph's stream length is unknown; a longer later stream is refused by the fit check. |
| Pool (default 8 MiB) | `OS_NPU_POOL_BYTES` | The DRAM range the board reserves for the NPU; a build parameter. |
| NPU virtual address space | `a733_npu_runtime.c`, `place` | 32 bits. Regions start at `0x10000000`, each followed by a guard page. |
| Page-table memory | `A733_NPU_MMU_MAX_BYTES` (4096 + 256 * 16384) | One 16 KiB leaf table per 16 MiB of device addresses a region touches, 256 at most. |
| Mapped regions, registered buffers | `A733_NPU_MMU_MAX_REGIONS` (512), `A733_NPU_MEMORY_MAX_BUFFERS` (128; 125 usable: the bootstrap and four regions for each of 31 slots) | Array sizes of the table builder and memory layer. |

The vendor driver has no event, semaphore or fence identifiers that limit the count of resident graphs or queued jobs; its asynchronous ring is sized by configuration (`vpmdTASK_PARALLEL_ASYNC`).

## Declarations

The manifest of an application has an `npu` section:

```json
"npu": { "slots": 2, "in_flight": 4,
         "graphs": [ {"name": "encoder", "bytes_max": 3145728}, {"name": "policy", "bytes_max": 262144} ],
         "links": [ {"from": "encoder.0", "to": "policy.0"} ] }
```

- **slots** is the number of graphs that run concurrently (one command buffer and one set of regions each).
- **graphs** names the graphs kept resident and bounds the size of each. A graph handed over at run time is declared by its bound; a graph named in a build-time list is an optional convenience.
- **links** names the tensors that one graph passes to another without a copy.
- **in_flight** is the most jobs kept submitted and not yet completed.

The memory the declaration needs is computed: for each slot, the command buffer, the largest constant region, the largest input and output regions among its graphs and the staging area for the largest graph, plus the page tables for the device range these use (`npu_runtime_requirements`).

## Admission and reservation

**Where the declaration lives.** `slots`, `in_flight` and `graphs` are required and `links` defaults to 0 (`tools/sunstone-manifest.py`). The session record carries them in the NPU extent, version 3 (`apps/os/npu-session.h`): four full-width 32-bit fields after the evidence digests, plus the bounds of the graphs the application loads at run time (`graphs`, `graph_bytes`, `tensor_bytes`). Earlier versions are not admitted. `apps/os/host.py` reads the manifest (`npu_declaration`) and upgrades a template (`upgrade_services`).

**Checks at session admission** (`apps/os/admission.c`, `os_npu_declaration_admitted`). The first failure names the resource and is left in the first-failure record as the value of a record in group NPU:

1. a record that declares nothing is refused as missing; there is no default;
2. slots: at least 1 and at most 31; an image without a queue has one client task and so exactly 1;
3. in-flight depth and graphs: at least 1;
4. the declaration against what the image links: at least the slots, graphs and links it uses;
5. the declaration against the tables the image holds.

**At start** (`os_npu_start`) the memory is computed from the declaration and the linked graphs (`npu_runtime_reservation`). A graph that is not admissible, a device address space that the regions overflow, or a reservation beyond the pool is refused with the resource's code in the start record. The pool is carved for the declared slots, the tables are built and the runtime is prepared; nothing is allocated afterwards. A queue opened deeper than the declared in-flight depth is refused at open.

The graph catalogue, plans and digests are placed at the start of the reservation (`npu_runtime_tables_bytes`, 336 bytes a graph). The peak stack of the start-up and loading tasks is 4,144 bytes for `os_npu_start` and at most 3,856 for the other start and load entries (`npu-resources-stack-depth`).

## The pool

The pool is the DRAM that the NPU runtime reserves for its slots: per slot a command allocation, a read-only region (descriptors, coefficients, dynamic input), a read region and a write region, with an unmapped page after each, plus the page tables, the bootstrap stream and the staging area. Its size follows from the declaration.

**Per-slot sizing.** When slot i holds one graph for the whole run (a queue image keeps graph i resident in slot i, `npu_runtime_bounds.slot_model`), the regions of slot i are sized from that graph alone; otherwise every slot is sized for the largest graph, because any graph may be loaded into any slot. A graph with large scratch costs that scratch once, not in every slot. LOAD refuses a graph that does not fit the regions of its slot. A graph registered at run time needs the uniform layout, so `slot_model` and run-time capacity exclude each other. For 31 slots of add or relu the pool use is 6,291,456 bytes of 8 MiB (32,768 bytes per slot).

**Placement.** `OS_NPU_POOL_BYTES` is the build parameter `SUNSTONE_OS_NPU_POOL_BYTES`, in blocks of 2 MiB from 8 to 48 MiB. `checks/npu-pool-size.py` computes the bytes that a model list and a number of slots need from the real runtime (`npu_runtime_reservation_with`), and the image refuses at start if the declaration does not fit its pool. An 8 MiB pool is at `0x47400000..0x47c00000`. A larger pool ends at `0x47000000`, the bottom of the monitor interval, and starts at `0x47000000 - bytes`. Four bounds apply:

- CPU side: the pool must be write-back RAM that EL0 reaches, which is `0x40000000..0x48000000` in the cached profile (DRAM above `0x48000000` is closed to EL0). It must not overlap the image RAM (`0x41000000..0x42200000`), the application regions (`0x44000000..0x47000000`, so a session's application regions must end at the pool start) or the monitor interval `0x47000000..0x47400000`. The largest pool (48 MiB) fills the application window.
- NPU side, device addresses: 32 bits from `NPU_RUNTIME_DEVICE_BASE` (`0x10000000`) with a guard page after every allocation; the total of the three groups and the guards must stay below 4 GiB (`place()` refuses otherwise). The pool size does not change the device layout.
- NPU side, physical addresses: the page-table builder maps physical addresses below 2^40 with the tables in one 4 GiB window (`a733_npu_mmu_build`); every pool address is below 4 GiB.
- Session binding: the NPU extent names `pool_start` and `pool_bytes`, and admission refuses a session that differs from the image; the host side of the normal return (`apps/os/cpu-return-host.py`) accepts the legacy extent and a sized one.

## Graphs from the graph

The runtime takes every fact about a graph from the graph itself: tensor counts, shapes, formats and quantization through `nbg_model_tensor`, operation counts and sizes through the loader's layout. A graph loaded from a file needs no entry in `tools/npu-session-models.json` and no rebuild of the operating system:

1. The manifest declares the graphs and the bounds of those it loads at run time. The reservation covers `graphs` minus the linked ones more entries, each model of at most `graph_bytes` with no tensor beyond `tensor_bytes`, so a registered graph cannot fail on memory.
2. The application calls `ss_npu_register_graph(bytes, size, &index)`; `os_npu_register` calls `npu_runtime_register`, which admits the bytes like a linked graph (structure, commands, addresses), checks that the regions fit what was reserved, takes the SHA-256 and publishes the entry with release order. `ss_npu_run(index, ...)` then runs it. The OS compares the tensor sizes of a run-time graph with the request, because the application-side description table holds only the linked ones.
3. The caller keeps the bytes alive and unchanged. LOAD copies them into the staging area and refuses a copy whose digest differs from the one taken at registration.

The build-time tool `apps/os/npu-catalogue.py` links graphs into the image and fills the SDK description; it is not part of admission.

## Models from outside the image

An image does not have to link its models. A model is given to the runtime by an address, a length and a SHA-256 (`struct npu_runtime_model`, `npu-catalogue.py --external`, cmake `SUNSTONE_OS_NPU_EXTERNAL_MODELS`); the memory at the address was filled by something else. Two sources exist: a runner input region in the application window (`tools/npu-external-pack.py` packs models and test vectors into two regions and writes the maps; the manifest pins the digests of the regions), and, planned, a large region that the large-memory loader reads from the card into upper memory.

The digest is checked once, when LOAD stages the copy. Prepare does not hash a model that carries its digest, because hashing megabytes before the scheduler would outlast the one second that the secondary cores wait for the primary core (`platform/a733-smp-cached/continue.c`, exit status 3, detail `0x203`). A queue image loads its resident graphs when the ring opens, after the scheduler has started (`npu_runtime_load_resident`: the service worker stages and assembles them, the owner waits). Eighteen graphs of 15.3 MB start inside the one second and load well inside the six seconds the application waits for the ring (`npu-resources-external-models`).

## Chains of graphs

A network compiled as several graphs runs as a chain: graph g takes the output of graph g - 1 as its first input. Each graph has its own fixed-point position for its input and output (16-bit dynamic fixed point), so the application rescales between two graphs: code o at position f_out becomes code round(o * 2^(f_in - f_out)) at position f_in, clamped to 16 bits. The rescale runs on the CPU (a shift per element). The chain application (`apps/npu-graphs`, `NPU_GRAPHS_CHAIN`; data region format in `chain.h`) registers a handle for every graph, submits graph after graph, rescales each output into the next input and takes one greedy token per frame of the last output. For 18 graphs of 92 frames a chain takes 24.63 ms, of which 23.19 ms is the 18 jobs with the SDK handshake and 1.10 ms the 17 rescales (`npu-resources-chain`).

## Figures

| Figure | Value | Record |
| --- | --- | --- |
| Quota-free session | 120,000 jobs on one session with the `jobs` field at 1 (host) | `npu-resources-host-cases` |
| Pool use, 31 slots of add or relu | 6,291,456 of 8,388,608 bytes | this document, runtime accounting |
| 18 resident graphs, one pass | 22.75 ms | `npu-resources-external-models` |
| 18-graph chain | 24.63 ms | `npu-resources-chain` |

## Open work

1. Choose the default in-flight depth, `ceil(job time / submit cost) + 1`, from a board measurement for the encoder graph (`npu-resources-depth-default`).
2. Order requests between applications by a priority that the session grants, with a bounded wait for lower priorities. Not built.
3. The pool and the EL0 tensor windows are limited to the 128 MiB below `0x48000000` (the write-back RAM that EL0 reaches in the cached profile; DRAM above it is mapped for EL1 only), so the pool ends at `0x47000000` or `0x47c00000` and cannot exceed 48 MiB. Moving them to upper memory through the per-application tables of the large-region work would bound the pool by the installed DRAM. Not started.
