# Design: loading, switching and sharing compiled NPU models at runtime

Status: **design**; no code exists. Section numbers refer to
[docs/performance-architecture.md](../performance-architecture.md).

The runtime succeeds the concurrent-session code in
[npu_session.c](../../drivers/npu/npu_session.c). That code moves into
`drivers/npu/a733_npu_runtime.c`, and its interface moves from
`include/npu_session.h` to `include/npu_runtime.h` with the prefix
`a733_npu_runtime_`. The rename is required because
[apps/npu-convolution/session.h](../../apps/npu-convolution/session.h) uses the
same include guard and also defines `struct npu_session`, so no single-core NPU
image can include both headers. Up to eight client tasks each own one **slot**:
fixed memory and a command-table entry that hold one model at a time. A client
can switch its slot to another model while other slots run jobs, and jobs from
every slot share the NPU with round-robin starts.

Each slot has three regions. The **read-only region** holds NN descriptors,
coefficients, shader instructions and the command stream. The **read window**
holds inputs. The **write window** holds the working pool and the outputs. The
image fixes the number of slots and every region's size.

The first image consumer is `apps/npu-runtime-large`, built with the published
NN graph option (`A733_NPU_NN_POOL_GRAPH`). It moves from `npu_convolution` to
the runtime with one slot and can switch among the catalogue models the image
carries. This needs `apps/npu-convolution/native.c` to construct the runtime
instead of the convolution, a change listed below. Until that change lands, the
host model is the only consumer and the runtime builds for emulation only.

## Data flow and ownership

1. **LOAD.** The client sends LOAD with a catalogue index or, in upload builds,
   a reference to the staging area. The worker accepts LOAD only when the slot
   has no job, marks the slot LOADING and holds the reply until the slot is
   READY or the load is refused. Only the worker, at EL1, writes the read-only
   region.
2. **Single read.** The worker reads each model section it uses once, into a
   4 KiB worker-private buffer, and checks and consumes the bytes in that
   buffer. Each catalogue entry records a SHA-256 for every section the loader
   reads, including the header and directory. The slot becomes READY only when
   every section digest matches, so a change to the model bytes during the
   load cannot reach the device. Upload builds verify the NBG format checksum
   over the file first and compute no SHA-256, because nothing consumes an
   upload's identity.
3. **Assembly.** The worker writes descriptors, coefficients and shader
   instructions in ascending address order and builds the command stream from
   the load-state records. Each record must match an accepted template, which
   stops the stream from programming the NPU MMU or any register outside the
   template set. Each relocated field the worker writes must lie in the region
   of its role: descriptor, instruction and command pointers in the read-only
   region, input addresses in the read window, pool and output addresses in the
   write window. For catalogue models this is a self-check of the assembler's
   relocation output, which the host fixtures also cover. For uploads it covers
   only the fields the loader writes; addresses inside descriptor fields that
   the loader does not parse are unchecked. After the new stream, the worker
   writes END pairs up to the end of the previous stream, because the whole
   command allocation is the NPU's prefetch span.
4. **Job.** The application writes inputs in place and sends SUBMIT. The reply
   is held until the job is DONE or FAULT; the read and write windows are
   device-owned in between. The client then reads outputs in place, and USB0
   transmits from the output window. Outputs stay client-owned until the next
   SUBMIT, LOAD or CLOSE, which replaces WAIT and REAP. A client has one
   request outstanding, so a queued job cannot be cancelled; the start bound
   below plus its deadline limits it.

**Staging,** in upload builds only, is one area sized to the largest file the
image accepts; uploads arrive one at a time through the USB0 loop. Only EL1
code writes its state word. In state USB, the USB0 handler writes chunks for
one slot. After the last chunk the state becomes SEALED and the handler refuses
further chunks. When that slot's LOAD ends, the state returns to FREE. The
worker refuses LOAD unless staging is SEALED for that slot.

Admission runs once per load. The per-job checks are those whose inputs change
per job: slot state, input count and input lengths.

## Interrupts and polling

The runtime requires the NPU interrupt (INTID 97) binding through
`npu_service_bind_irq`; it has no tick-polled mode. Until board evidence
qualifies delivery, the existing one-tick poll stays behind the interrupt as the
timeout path while a job runs (section 1.3). Clients block on their reply
queues. On core 0 the worker runs at priority 5, above the USB0 loop at 4 and
the single-core image's client at 2. A USB0 burst of up to 189 µs per tick
therefore cannot delay completion handling or the next start. The `vTaskDelay(1)` loops
in `apps/npu-runtime-large` become notifications from the USB0 request handler.

Bulk worker work (loads, job preparation and post-job digests) runs in steps of
at most 8 KiB hashed, 256 KiB maintained or zeroed, or 4 KiB of records parsed.
Between steps the worker handles interrupt progress and dispatch, so bulk work
delays a reap or a start by at most one step. C denotes the longest step.

## Memory placement and cache policy

One parent allocation comes from the write-back cached DDR device pool, mapped
at EL1 with 2 MiB blocks; loads allocate nothing. Regions are grouped by CPU
permission: the read-only regions with the NPU table arena, the read windows,
and the write windows. Each group starts and ends on a 2 MiB boundary. The NPU
page tables are built once over every region and never rebuilt; unmapped device
pages separate regions. The NPU MMU entries of the read-only group carry no
write permission, but whether the hardware enforces that bit is unqualified
([docs/npu-mmu.md](../npu-mmu.md)). Device pages stay 4 KiB (section 2,
item 8), because the memory policy verifies only 4 KiB entries and large-page
encodings are unqualified. Each slot's command allocation is sized to the
image's largest accepted stream and rounded to 4 KiB, which is one page for
ShuffleNetV2 instead of 512 KiB; the 208-byte bootstrap stream also gets one
page.

Each input and output tensor is its own 4 KiB-aligned allocation and is padded
to whole 64-byte lines, so no client data shares a line with an output.

The NPU is not coherent; maintenance is by virtual address to the point of
coherency:

- **LOAD** cleans the read-only bytes it wrote.
- **Job preparation** runs once the job is queued and may overlap the previous
  job. It zeroes the pool with `DC ZVA` and cleans and invalidates it, cleans
  the input bytes, and cleans and invalidates the output bytes, so a dirty
  client line cannot be evicted over NPU output.
- **Completion** invalidates the output bytes.

Every model clears the pool per job. Catalogue images drop the clear once the
conformance package's SDK-simulator run shows that no catalogue model's output
depends on the pool's initial contents; upload builds always keep it. A CPU
write to a device-owned window between preparation and trigger, an interval of
at most one job deadline D, is a stray write. For the pool and outputs it goes
undetected until the fixed mappings below make the write group read-only.

**Fixed mappings,** requested from the primary agent, are set when the CPU
tables are built. The read-only and write groups are read-only at EL0 and EL1;
the read group is read-write; an EL1-only read-write alias of the read-only and
write groups is used only by the worker's load and pool-clear code. The tables
stay sealed, so there is no runtime TLB maintenance and no mapping generation.
A stray write then faults at the writing instruction, which removes the
interim checks below and the output maintenance at preparation.

**Interim integrity checks,** until the fixed mappings exist:

- *Before each trigger,* in the gap G that ends at `a733_npu_submit` and
  starts at the previous job's completion notification, or at SUBMIT when the
  NPU is idle: a scan of every NPU table word, a digest of the next
  slot's command payload and an END check of its command allocation. These
  bytes control the NPU MMU and register programming, so the check runs
  immediately before the trigger. One scan serves both the finished job and
  the next one.
- *After each job,* while the next job runs: a digest of the slot's
  descriptors, coefficients, instructions and command payload. The held SUBMIT
  reply is released only after it matches.
- *Reference digests* are computed during LOAD over the bytes the worker
  writes, from its own buffer, and kept in worker-private slot fields. A stray
  write that changes a reference produces a mismatch, which fails closed.

## Synchronization across cores

Clients run at EL0: on core 0 in the single-core consumer, on cores 1 to 5 in
multicore images, and on cores 6 and 7 once the multicore work admits them.
Each client is the only sender for its slot and has one request pending, which
serializes LOAD and SUBMIT for that slot without a lock. Only the worker writes
the slot state word; the client library reads it with acquire ordering.
Worker-written and client-written slot fields sit on separate 64-byte lines
(section 2, item 6). The USB0 handler and the worker update the staging state
word with release stores and read it with acquire loads.

Queue operations order a client's input writes before the worker cleans them,
and cleaning by address is broadcast to the inner-shareable domain, so core 0
cleans lines dirty on core 6. The worker completes the output invalidation with
a `DSB` before it releases the reply.

Round-robin dispatch starts a queued job after at most one job from each other
busy slot. With S busy slots, every deadline capped by an image constant D, a
per-start gap G and a longest step C, the wait before a start is at most
(S − 1) × (D + G) + C. A held SUBMIT reply arrives within that bound plus D
plus H, where H is the post-job digest of the slot (interim only). Clients
size their collect timeouts from this sum. NPU jobs cannot be preempted, so
time-weighted fairness is not offered.

## Reuse and removal

Moved from `npu_session.c`: one-parent placement with guard pages, session
groups, catalogue admission, handoff, binding, startup and stop polls, and
tensor metadata. Reused: the shared service mode with round-robin dispatch,
held replies, interrupt binding and pre-device refusal path; the memory policy;
the native adapter and controller; `nbg_model_open`; the checksum verifier; the
general layout assembler; the device-pool reservation; and the shared USB0
loop. The service in `drivers/npu/npu_service.c` gains:

- a SUBMIT flag that holds the reply until the runtime releases it;
- LOAD as a control operation with a held reply;
- a runtime hook, called once per worker pass, that performs one step of bulk
  work;
- a callback before `a733_npu_submit` that runs the checks in G, so a refusal
  faults only that slot.

Removed from the job path: per-job translations, the client's pool `memset`,
the input and output copies, WAIT and REAP, and the END scans of 512 KiB
command allocations. Removed from loading: the private copy that
`npu_session.c` keeps of every model and its digest at startup, activation and
stop. The single-read rule replaces the copy, and the post-job digest of each
slot's last job covers the bytes the stop digest covered. Also removed:
tick-polled completion and any per-model choice of pool clear.

The fixed assembler in [nbg_sh_nt.c](../../drivers/npu/nbg_sh_nt.c) is folded
into the general assembler once the relative-shift fixture assembles to
identical bytes. The assembler gains a table of load-state templates per
operation kind: NN (kind 2) and neural transpose (kind 8) as today, plus shader
and tensor-processor (kind 3) operations. Each template enters with a
pinned-compiler fixture, its SDK capture and a host check against the Python
reference assembler. The shader template's capture must include the shader
instruction-cache invalidation the SDK issues when instructions change; the
flush state in today's command prefix and footer is not shown to cover it.

## Budgets and measurement

Baselines are counted from the code for ShuffleNetV2 (964,224-byte model,
150,528-byte input, 2-byte output, 1,093,632-byte pool allocation). They are
the same in `npu_convolution`'s published NN graph build and in `npu_session.c`
with ShuffleNetV2 as its only model, because both use the same eight
allocations. In `npu_session.c` each further model adds translations to every
job of every model: two per page, four per write page. Budgets are for one
catalogue slot; table figures grow by 4,096 words per 16 MiB of device
addresses.

| Metric | Baseline | Interim budget | With fixed mappings and the pool result |
| --- | --- | --- | --- |
| Requests and replies per inference | 3 and 3 (SUBMIT, WAIT, REAP) | 1 and 1 | 1 and 1 |
| Client wake-ups per inference | 3, plus one per tick in `apps/npu-runtime-large` while idle | 1 | 1 |
| `AT` translations per inference | 2,126 | 0 | 0 |
| NPU table words read per inference | 4,352 and 3,072 padding bytes | Same | 0 |
| Bytes digested per inference | About 920,000, and an END check of about 65,000 words, before the trigger | 4,096 command bytes digested or END-checked before the trigger; about 920,000 after the job, overlapped with the next | 0 |
| CPU bytes cleared or copied per inference | 1,244,162 | 1,093,632 zeroed by `DC ZVA`; none copied | 0 |
| Cache maintenance bytes per inference | 2,347,008 | 1,244,288 | 150,592 |
| Bytes read or maintained in G | Every row above except the client copies | 24,640 (20,480 table, 4,096 command, 64 output) | 64 |
| Largest worker step C | No bulk work in the worker | 8 KiB hashed, 256 KiB maintained or zeroed, or 4 KiB parsed | Same |
| One ShuffleNetV2 catalogue load | Not supported after startup | At most 964,224 bytes hashed once; 0 translations, 0 allocations | Same |
| One 964,224-byte upload through USB0 | Not supported | At most 200 ticks (about 5 MB/s, the shared loop's figure) | Same |
| Start order with S busy slots | Period S | Period S, also while another slot loads | Same |

*Estimated,* with portable SHA-256 at 15–25 cycles per byte and the SHA-2
instructions at 2–3: G takes about 0.05 ms at 1.8 GHz and 0.2 ms at 494 MHz,
and C at most about 0.15 ms and 0.5 ms. A ShuffleNetV2 load takes 2–5 ms at
1.8 GHz with the SHA-2 instructions and 10–17 ms without. The post-job digest H
takes about 1.5 ms with the SHA-2 instructions and 8–14 ms without; it adds
client latency and worker time but no NPU idle time. The shared digest helper
uses the SHA-2 instructions when `ID_AA64ISAR0_EL1` reports them.

[checks/npu-session.c](../../checks/npu-session.c) and
[checks/npu-session.py](../../checks/npu-session.py), with two QEMU cores and a
register model that reads descriptors through the NPU tables, move to the
runtime and keep their cases. Their counters go in first and run against
`npu_session.c` to record the baseline. They count translate and cache
callbacks with bytes per operation; digest, `memset`, `DC ZVA` and `memcpy`
bytes through `--wrap`, attributed to worker, client or G; the largest worker
step; service sends and collects; and `vTaskDelay` calls. The register model
completes a job on a credit, so a case can complete a job before or after the
next job's preparation and count G both ways. New cases:

- load model A, run, load the shorter-stream model B into the same slot, run;
  the END tail is checked;
- three slots running while a fourth loads, with start period three;
- a catalogue byte changed between two section reads of a LOAD: refused, slot
  never READY;
- a staging chunk after SEALED, and a LOAD while staging is not SEALED: both
  refused;
- a command record outside the templates, and a relocated field outside its
  role's region: refused at LOAD;
- a command byte changed while its slot is idle: refused before the trigger,
  other slots continue;
- a coefficient byte changed while its job runs: post-job mismatch, every slot
  retained;
- CPU writes to the output and pool before SUBMIT: QEMU models no caches, so the
  case checks that preparation cleans and invalidates both extents before the
  trigger.

## Failure classes (section 1.8)

| Failure | Class | Retained |
| --- | --- | --- |
| LOAD refused: format, checksum, section digest, template, role, size or staging state | 1 | Nothing; slot EMPTY |
| SUBMIT refused: slot not READY, wrong input count or length | 1 | Nothing |
| Command bytes of the next slot changed, found in G | 1 | That slot is FAULT until reloaded; its last job passed the post-job digest, so the device has not read the change |
| NPU table words changed, found in G | 3 | Every slot and the controller; the change may have happened while the last job ran |
| Post-job digest mismatch | 3 | Every slot and the controller |
| Job deadline or controller fault | 3 | Every slot and the controller until the hang-reset package supplies a verified quiesce |

## Changes needed from other owners, as part of this change

- **Session code:** delete `drivers/npu/npu_session.c`; point
  `include/a733_npu_session_reserved.h` and
  `platform/a733/npu_session_reserved.c` at `npu_runtime.h`; move
  `drivers/npu/npu_session_models.inc` to
  `drivers/npu/a733_npu_runtime_models.inc`, with per-section SHA-256 from
  `tools/npu-session-catalogue.py`; rewrite `docs/npu-session.md` for the
  runtime.
- **Single-core providers:** `apps/npu-convolution/native.c` constructs the
  runtime for `apps/npu-runtime-large`.
- **Memory policy** (`drivers/npu/a733_npu_memory.c`): a per-buffer maintained
  extent, settable while the buffer's group has no job; a preparation call
  that performs a group's job-start maintenance outside publication; mapping
  verification once at initialization, covering the physical address and
  write-back cacheability of each 2 MiB block, which the fixed mappings never
  change; and contract text allowing an idle group's CONSTANT and COMMAND bytes
  to be rewritten when they are cleaned before its next publication. Without
  the extent and the preparation call, the cache and G budgets fail and the
  runtime does not merge.
- **Shared digest helper** (`storage_digest_bytes`): the SHA-2 instructions
  when `ID_AA64ISAR0_EL1` reports them.
- **Primary agent:** the fixed mappings above, and A76 admission for client
  affinity.
- **USB0 evidence channel:** a host-to-device case in
  `checks/usb0-service-burst.py` for the upload budget.
- **Hang-reset package:** a verified quiesce and reset, making controller
  faults class 2.
- **Conformance package:** the pool-pattern comparison and the numerical
  conformance of new operators.

## Left out

- **Isolation between uploaded models.** Per-job page-directory switching needs
  an unqualified NPU MMU flush sequence. In one device address space an
  uploaded model can read every slot and write every write window. Upload is a
  build option that stays emulation-only until the board shows the guard-page
  and write-permission faults listed below.
- **Software (CPU) layers and compilation on the device.** Loads refuse them;
  they need CPU kernels and a buffer hand-off in the middle of a graph.
- **Cancelling a queued job,** which would need a second outstanding request
  per client.
- **The crypto engine for digests.** It is a separate DMA master with its own
  clock and session ownership, and the large digests already overlap NPU work.
- **Weaker MMIO barriers,** which need the board qualification in
  [docs/shared-helpers.md](../shared-helpers.md).
- **A board image derived from the supervised board image,** which would
  construct a second resource manager (supervisor-apps-10 in
  [docs/audit/README.md](../audit/README.md)).

Board evidence that would confirm the design:

- two models with different shader programs, loaded alternately at the same
  slot address, each match their SDK outputs, so the NPU keeps no stale state
  or shader instructions across a reload;
- every job completes by interrupt, with zero fallback-poll completions;
- a descriptor that addresses a guard page raises an NPU MMU fault;
- a descriptor that writes into a read-only page raises an NPU MMU permission
  fault;
- the start order on the board matches the host model;
- back-to-back runs without a pool clear match the reference outputs.
