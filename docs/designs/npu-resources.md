# NPU resources: declared, admitted and reserved

Status: **design; the lifetime job quota is removed, the rest is not built.** This document covers how an
application states what it needs from the NPU, how the operating system admits that against what the NPU and
the memory really offer, and which numbers in the code are limits of the hardware and which are choices of a
fixed array. It also fixes the end state for graphs: an application loads a graph that it hands over at run
time, with no build-time list.

## Principles

1. **No count of completed jobs.** A robot submits jobs for as long as it runs. A session never fails because
   it has run a certain number of jobs. The job counters that exist are statistics.
2. **Declare, admit, reserve.** The application declares what it keeps resident and how deep its queue is. The
   operating system admits the application only if the declaration fits the real capacity and reserves the
   resources at admission. After that a running application cannot fail on a resource it declared. Nothing is
   allocated on the job path, so job latency stays deterministic.
3. **Limits come from a source.** Each fixed array or limit is either derived from a bound (memory, page-table
   entries, command space, a hardware field) or it does not exist beyond memory. Where an array is only a
   compile-time choice, it is sized from the declarations and the bound, not from a round number.
4. **Backpressure, not refusal.** When the declared in-flight depth is full, a blocking submit waits for a
   completion fence and a non-blocking submit returns busy. Neither ends the session.

## What bounds each number today

| Number | Where | What actually bounds it |
| --- | --- | --- |
| Job quota `jobs` (8) | `apps/os/admission.c`, `apps/os/npu.c`, `apps/os/npu-queue.c` | Nothing. It was sized for the finite test sessions. **Removed.** The field remains in the session record and now only sets how many jobs the diagnostic client runs. |
| `NPU_SERVICE_CLIENTS` and `NPU_RUNTIME_SLOTS` (8) | `include/npu_service.h` | Each slot owns a command buffer, a read-only region, an input region and an output region (`a733_npu_runtime.c`, `place`). Memory bounds the count. In code, a client's command mask is 32 bits (`NPU_SERVICE_COMMANDS`), and command zero is the bootstrap, so at most 31 slots. The static arrays follow from the declaration, up to that. |
| `SS_NPU_QUEUE_MAX_HANDLES` (8) | `include/sunstone_npu_queue.h` | One handle per slot, so the slot count. |
| `SS_NPU_QUEUE_MAX_DEPTH` (64) and `SS_NPU_QUEUE_MAX_TENSORS` (32) | same | Array sizes in the queue (`runtime/npu_app_queue.c`). No hardware limit. The depth is the declared in-flight depth. |
| `NPU_RUNTIME_MODELS` (16) | `include/npu_runtime.h` | An array of descriptors and plans in the runtime. The models are resident in DRAM; memory bounds them. |
| `NBG_ASSEMBLY_MAX_TENSORS` (8) and `NBG_ASSEMBLY_MAX_NN` (1024) | `include/nbg_assembly.h` | Array sizes of the loader's plan (stack and static). A graph beyond them is refused today. |
| Command space per job (524,288 bytes) | `NPU_RUNTIME_COMMAND_BYTES` | A hardware field. The doorbell word carries the size in 8-byte units in 16 bits (`vipdMAX_CMD_SIZE` is 65535 * 8 in `vip2/inc/vip_drv_share.h`), so 524,280 bytes; the buffer is rounded to 512 KiB. |
| Pool (8 MiB) | `OS_NPU_POOL_BYTES` | The DRAM range that the board reserves for the NPU. It is a choice of the board layout and can grow with it. |
| NPU virtual address space | `a733_npu_runtime.c`, `place` | 32 bits. Regions start at `0x10000000` and each is followed by a guard page, so about 3.75 GiB. |
| Page-table memory | `A733_NPU_MMU_MAX_BYTES` (4096 + 256 * 16384) | One 16 KiB leaf table per 16 MiB of device addresses that a region touches, 256 at most. |
| Mapped regions | `A733_NPU_MMU_MAX_REGIONS` (512) and `A733_NPU_MEMORY_MAX_BUFFERS` (64) | Array sizes of the table builder and the memory layer. |

The vendor driver has no event, semaphore or fence identifiers that limit the count of resident graphs or queued
jobs; its asynchronous ring is sized by configuration (`vpmdTASK_PARALLEL_ASYNC`). The hardware limits that exist
are the command size, the 32-bit device address space and the page-table layout above.

## Declarations

The manifest of an application gains an `npu` section:

```json
"npu": { "slots": 2, "in_flight": 4,
         "graphs": [ {"name": "encoder", "bytes_max": 3145728}, {"name": "policy", "bytes_max": 262144} ],
         "links": [ {"from": "encoder.0", "to": "policy.0"} ] }
```

- **slots** is the number of graphs that run concurrently (one command buffer and one set of regions each).
- **graphs** names the graphs kept resident and bounds the size of each. A graph handed over at run time is
  declared by its bound, not by a build-time entry; a graph named in a build-time list is an optional
  convenience.
- **links** names the tensors that one graph passes to another without a copy.
- **in_flight** is the most jobs kept submitted and not yet completed.

The memory the declaration needs is computed, not declared: for each slot, the command buffer, the largest
constant region, the largest input and output regions among its graphs, and the staging area for the largest
graph, plus the page tables for the device range these use (`npu_runtime_requirements`).

## Admission and reservation

The operating system adds the declaration to the session record that it already seals for each application. At
admission it checks, in this order, against the real capacity: the slot count against the memory and the
command-mask bound; the sum of the computed regions against the pool and against the device address space and
page-table limits; the links against the tensor shapes; and the in-flight depth against the queue's array.
A declaration that does not fit is refused with a reason that names the resource. An admitted declaration is
reserved: the pool is carved at admission, the tables are built, the queue is allocated at its declared depth.
The job path then allocates nothing.

## Graphs from the graph

The runtime takes every fact about a graph from the graph itself: tensor counts, shapes, formats and
quantization through `nbg_model_tensor`, operation counts and sizes through the loader's layout. The SDK model
description (`struct ss_npu_model`) is filled from them when the graph is registered. A graph that an
application loads from a file, for example into a large region by the microSD loader, is registered with its
bytes and (optionally) the digest the application supplies; it needs no entry in `tools/npu-session-models.json`
and no rebuild of the operating system. The build-time tool `apps/os/npu-catalogue.py` stays as a convenience
that links graphs into the image and fills the same description; it is not part of admission.

## In-flight depth and backpressure

The queue already answers busy when the ring is full (`ss_npu_queue_submit` returns 0). The synchronous SDK path
(`ss_npu_run`) is one job at a time and returns pending until it completes. The default suggestion for the
in-flight depth comes from a measurement: with about 10 us of submit cost against the job time, the depth that
keeps the NPU busy is `ceil(job time / submit cost) + 1`; the measurement is to be made on the board for the
encoder graph and recorded here.

## Several applications

Contention between applications that use the NPU is a matter of priority and order (a control-critical model
before a background one), not of count limits. The service will order requests by a priority that the session
grants, with a bounded wait for the lower priorities. This is the next step after this document and is not built.

## Plan and cases

1. **Done:** remove the lifetime quota in the three places above. Host case 26 of `checks/os-npu-service.py`: a session whose `jobs` field
   is 1 runs 120,000 jobs through the real wrapper, runtime and service without failing, and the same wrapper with the
   quota restored fails the case. The admission bound of 8 is removed with it (`apps/os/admission.c`).
2. Add the declarations to the manifest and the session record; admission and reservation; host cases for
   refusal at each capacity (slots, pool, page tables, depth) and for the default declaration.
3. Register a graph at run time and fill the model description from it; host cases with a graph that no list
   names, loaded after the scheduler has started.
4. Backpressure measurements and the default depth.
5. Linking after the scheduler has started, from declared links.
