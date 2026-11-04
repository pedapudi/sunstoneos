# NPU resources: declared, admitted and reserved

Status: **qualified on the board** for declarations, admission, reservation, run-time graph registration, a declared number of slots (400 on the host) with a ring of depth 64 and backpressure, tensor links after the scheduler starts, and externally supplied models ([NPU status](../status/npu.md); records in [npu-resources.jsonl](../evidence/npu-resources.jsonl)). **Open:** service priority between applications, and a pool in upper memory.

An application states what it needs from the NPU in its manifest. The operating system admits the declaration against what the NPU and the memory offer and reserves the resources at admission. An application loads graphs it hands over at run time, with no build-time list.

## Principles

1. **No count of completed jobs.** A robot submits jobs for as long as it runs. A session never fails because it has run a certain number of jobs. The job counters are statistics; the `jobs` field of the session record only sets how many jobs the diagnostic client runs.
2. **Declare, admit, reserve.** The application declares what it keeps resident and how deep its queue is. The operating system admits it only if the declaration fits the real capacity and reserves the resources at admission. A running application cannot fail on a resource it declared, and nothing is allocated on the job path, so job latency stays deterministic.
3. **Limits come from a source.** Each fixed array or limit derives from a bound (memory, page-table entries, command space, a hardware field) or does not exist beyond memory. A compile-time array is sized from the declarations and the bound, not from a round number.
4. **Backpressure on a full ring.** When the declared in-flight depth is full, a blocking submit waits for a completion fence and a non-blocking submit returns busy (`ss_npu_queue_submit` returns 0). Neither ends the session.

## What bounds each number

| Number | Where | Bound |
| --- | --- | --- |
| Slots, clients, queue handles | none fixed. Two allocator pairs, `include/npu_pool.h` | The declared slots. Tables are carved at prepare, sized from the declaration (slots, graphs, tensors of each graph, links), through two allocate and release pairs. **Device memory** (`struct npu_pool`, over the pool) holds what the NPU reads and writes and what EL0 writes by design: the regions, the bootstrap and staging, the page-table arena and the queue ring's handle, group and requested-edge tables. **EL1 metadata** (`struct npu_meta`) holds everything the EL1 checks trust: the service's clients, one permission bit for each command in each client, members, commands and request storage, the runtime's slot, layout, target, tensor, plan and buffer tables, the memory layer's tables and the page-table builder's region list, and the queue adapter's edge table and command records. It comes from memory no EL0 mapping reaches and the NPU's page tables do not map: a task that could write it could grant itself another client's commands. The OS has no EL1 allocator that grows at run time, so `apps/os/npu.c` backs the pair with an arena in EL1 image data and bss, and the build links exactly the bytes the image's declaration needs: `tools/npu-meta-header.py` runs the runtime's own sizing call (`npu_runtime_meta_bytes_with`, through `checks/npu-pool-size.py`) on the image's graphs, one slot for each graph under a queue and one slot otherwise, and writes `os-npu-meta.h` (216,256 bytes for the 80-graph image, 221,376 with its queue's edge records). There is no size setting and no default; an image without the NPU links no arena; `SUNSTONE_OS_NPU_LINKS` names the links the application may ask for at run time, which the queue reserves room for. A session that declares more than was linked is refused at start. The unified memory work (`docs/designs/unified-memory.md`, step 1) replaces the arena with kernel-owned ledger buffers, and only that definition changes. Two bounds apply, both memory: the pool and the metadata arena (the image's own declaration, linked at its exact size), and the NPU's 4 GiB of device addresses (four regions and their guard pages for each slot, below). When one is reached, prepare refuses the declaration, leaves the resource in `npu_runtime.refusal` (`NPU_DECLARATION_MEMORY` for the pool or the arena, `NPU_DECLARATION_DEVICE_SPACE` for the address space) and the start record, and the application does not run. Host runs: 400 slots, 29,360,128 bytes of pool and 1,236,672 bytes of metadata (`checks/os-npu-service.py`, which also checks that every trusted table is outside the pool, the buffers the NPU maps and its page tables, and that placing one in the pool is refused). |
| Ring depth (the one bounded structure) | `SS_NPU_QUEUE_MAX_DEPTH` (64 default) | Array size in `runtime/npu_app_queue.c`, no hardware limit. A full ring is backpressure, not a limit on what can run: a blocking submit waits for a fence and a non-blocking one returns busy. The image is built with the manifest's `in_flight` (`-DSUNSTONE_OS_NPU_QUEUE_DEPTH`); a session that declares more is refused as "image tables". |
| Tensors per graph | none fixed. The graph's header counts them | The header gives the input and output counts as 32-bit fields, which the decoder checks against the size of the tensor sections, so the model file bounds them. A layout's table has one entry for each tensor, the four fixed allocations and the dynamic-input allocation (`nbg_assembly_layout_entries`), is carved from the pool for the graph and the slot that holds it, and costs 12 bytes an entry, plus 24 bytes of target and about 160 of tensor record in the slot. A graph registered after prepare is bounded by the declaration: every tensor takes at least a page, so it has at most as many entries as the declared regions hold pages, and a graph with more is refused at registration. |
| Operations per graph | none | No array is sized by it. The model container (at most 256 MiB) and, through the command stream, the slot's memory bound it. |
| Command space | `NPU_RUNTIME_COMMAND_BYTES`, `A733_NPU_FETCH_BYTES` | One fetch is at most 524,280 bytes: the doorbell word carries the fetch size in 8-byte units in 16 bits (`vipdMAX_CMD_SIZE` is 65535 * 8). A longer stream is cut between operations into segments joined by the vendor's LINK command ([command formats](../hardware/npu-command-formats.md), "Command segments"), so stream length is bounded by the slot. Each slot's command allocation is the stream of the largest resident graph (with its links) plus one END pair, rounded to a page, and the doorbell word is the allocation in 8-byte units. A reservation that also holds run-time graphs uses 524,288 bytes per allocation, because a later graph's stream length is unknown; a longer later stream is refused by the fit check. |
| Pool | `os_npu_pool_prepare` (`apps/os/npu.c`) | The largest extent the NPU can address inside one 4 GiB window, taken from the ledger at admission and trimmed to what the runtime places. |
| NPU virtual address space | `a733_npu_runtime.c`, `place` | 32 bits. Regions start at `0x10000000`, each followed by a guard page. |
| Page-table memory | `A733_NPU_MMU_MAX_BYTES` (4096 + 256 * 16384) | One 16 KiB leaf table per 16 MiB of device addresses a region touches, 256 at most. |
| Mapped regions, registered buffers | none fixed. `A733_NPU_MMU_MAX_BYTES` bounds the tables | The builder takes the caller's list of regions, and the memory layer's tables (`a733_npu_memory_tables_bytes`) hold the sorted buffers, their groups and the builder's list for the buffers registered: 1 + 4 for each slot. The leaf tables are the one hardware bound, the 256 directory entries of 16 MiB that a 32-bit device address selects, 4 GiB. A region outside the range or an arena over `A733_NPU_MMU_MAX_BYTES` is refused. |
| Patches | none per graph. At most the descriptor's pointer fields per operation | One NN descriptor has seven pointer fields (weights, input, output, their ends, a second input and its end: `POINTER_COUNT`, a property of the descriptor format) and one command state. The relocation writer takes any count up to the words of its destination buffer, because each destination is a distinct word. |

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
2. slots: at least 1 (the pool and the device address space bound the rest, at start); an image without a queue has one client task and so exactly 1;
3. in-flight depth and graphs: at least 1;
4. the declaration against what the image links: at least the slots, graphs and links it uses;
5. the declaration against the tables the image holds.

**At start** (`os_npu_start`) the memory is computed from the declaration and the linked graphs (`npu_runtime_reservation`). A graph that is not admissible, a device address space that the regions overflow, or a reservation beyond the pool is refused with the resource's code in the start record. The pool is carved for the declared slots, the tables are built and the runtime is prepared; nothing is allocated afterwards. A queue opened deeper than the declared in-flight depth is refused at open.

The graph catalogue, plans and digests and every table named in the first row above are carved in the order `lay_out` walks the declaration, the device-visible ones from the pool and the trusted ones from the metadata arena; the regions follow the pool's tables. The arithmetic is one function for sizing and for prepare, so the pool size that `checks/npu-pool-size.py` reports covers the tables. The peak stack of the start-up and loading tasks is 4,144 bytes for `os_npu_start` and at most 3,856 for the other start and load entries (`npu-resources-stack-depth`).

## The pool

The pool is the DRAM that the NPU runtime reserves for its slots: per slot a command allocation, a read-only region (descriptors, coefficients, dynamic input), a read region and a write region, with an unmapped page after each. The pool also holds the page tables, the bootstrap stream and the staging area. Its size follows from the declaration.

**Per-slot sizing.** When slot i holds one graph for the whole run (a queue image keeps graph i resident in slot i, `npu_runtime_bounds.per_graph`), the regions of slot i are sized from that graph alone. Otherwise every slot is sized for the largest graph, because any graph may be loaded into any slot. A graph with large scratch costs that scratch once, not in every slot. LOAD refuses a graph that does not fit the regions of its slot. A graph registered at run time needs the uniform layout, so `per_graph` and run-time capacity exclude each other. For 31 slots of add or relu the pool use is 6,291,456 bytes of 8 MiB (32,768 bytes per slot).

**Placement.** `os_npu_pool_prepare` runs when the application is admitted, from `os_large_prepare`, after the manifest regions and large regions have taken their memory. It reserves the largest extent that the NPU can address inside one 4 GiB physical window (the device-width table of [unified-memory.md](unified-memory.md) gives the NPU's working limit), starting on a 16 MiB boundary. `npu_runtime_prepare` places the declaration from the start of that extent and records the end of its placement (`placed_bytes`); the part beyond goes back to the ledger (`umem_trim`) before the application's space is built, so no mapping ever covers the returned memory. The extent belongs to the application and is mapped read-write in its tables, because the tensor and ring windows are in it. Sizing the reservation first would run every graph's layout twice before the scheduler starts, and the secondary cores wait one second for it. The OS publishes the trimmed range for the return (`apps/os/device-ranges.h`). Three bounds apply:

- NPU side, device addresses: 32 bits from `NPU_RUNTIME_DEVICE_BASE` (`0x10000000`) with a guard page after every allocation; the total of the three groups and the guards must stay below 4 GiB (`place()` refuses otherwise). The pool size does not change the device layout.
- NPU side, physical addresses: the page-table builder maps physical addresses below 2^40 with the tables in one 4 GiB window (`a733_npu_mmu_build`), so the extent does not cross a 4 GiB boundary. The working limit of the NPU's physical addresses is one entry of the device-width table; a board run that places the pool above 4 GiB raises it.
- Session: the NPU extent of the session names no pool (its former pool words are zero and the image refuses any other value).

## Graphs from the graph

The runtime takes every fact about a graph from the graph itself: tensor counts, shapes, formats and quantization through `nbg_model_tensor`, operation counts and sizes through the loader's layout. A graph loaded from a file needs no entry in `tools/npu-fixtures.json` and no rebuild of the operating system:

1. The manifest declares the graphs and the bounds of those it loads at run time. The reservation covers `graphs` minus the linked ones more entries, each model of at most `graph_bytes` with no tensor beyond `tensor_bytes`, so a registered graph cannot fail on memory.
2. The application calls `ss_npu_register_graph(bytes, size, &index)`; `os_npu_register` calls `npu_runtime_register`, which admits the bytes like a linked graph (structure, commands, addresses), checks that the regions fit what was reserved, takes the SHA-256 and publishes the entry with release order. `ss_npu_run(index, ...)` then runs it. The OS compares the tensor sizes of a run-time graph with the request, because the application-side description table holds only the linked ones.
3. The caller keeps the bytes alive and unchanged. LOAD copies them into the staging area and refuses a copy whose digest differs from the one taken at registration.

The build-time tool `apps/os/npu-catalogue.py` links graphs into the image and fills the SDK description; it is not part of admission.

## Models from outside the image

An image does not have to link its models. A model is given to the runtime by an address, a length and a SHA-256 (`struct npu_runtime_model`, `npu-catalogue.py --external`, cmake `SUNSTONE_OS_NPU_EXTERNAL_MODELS`); the memory at the address was filled by something else. Two sources exist: a runner input region in the application window (`tools/npu-external-pack.py` packs models and test vectors into two regions and writes the maps; the manifest pins the digests of the regions). A second source, planned, is a large region that the large-memory loader reads from the card into upper memory.

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
