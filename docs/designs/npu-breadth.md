# Design: loading, switching and sharing compiled NPU models at runtime

Status: **design**; no code exists. Section numbers refer to
[docs/performance-architecture.md](../performance-architecture.md).

The runtime, `drivers/npu/a733_npu_runtime.c` with the interface
`include/npu_runtime.h`, replaces the concurrent-session code in
[npu_session.c](../../drivers/npu/npu_session.c), whose header name and `struct
npu_session` collide with
[apps/npu-convolution/session.h](../../apps/npu-convolution/session.h). Up to
eight client tasks each own one **slot**: fixed memory and a command-table
entry that hold one model at a time. A client can switch its slot to another
model while other slots run jobs, and jobs from every slot share the NPU with
round-robin starts.

Each slot has three regions. The **read-only region** holds NN descriptors,
coefficients, shader instructions and a 524,288-byte command allocation. The
**read window** holds inputs. The **write window** holds the working pool and
the outputs. The image fixes the number of slots and every region's size.

The first image consumer is `apps/npu-runtime-large` with one slot, built with
the published NN graph option (`A733_NPU_NN_POOL_GRAPH`). Its client switches
among the catalogue models the image carries. Core 0 runs at Boot0's 494 MHz
unless the CPU clock driver raises CPU-L ([docs/cpu-clock.md](../cpu-clock.md)),
so every time estimate below is for 494 MHz.

## Data flow and ownership

1. **LOAD.** The client sends LOAD with a catalogue index. The worker accepts
   it only when the slot has no job, marks the slot LOADING, and holds the
   reply until the slot is READY or the load is refused. Only the worker, at
   EL1, writes the read-only region.
2. **Single read.** The worker reads every source byte once. Sections it
   parses (header, directory, load-state records, NN descriptors) pass through
   a 4 KiB worker-private buffer, and the worker parses only that buffer.
   Coefficients and shader instructions are copied directly into the slot
   through the worker's EL1 mapping, and the worker hashes the destination
   bytes, which are the bytes the device reads. Each catalogue entry records
   one SHA-256 per section. These digests replace the entry's whole-model
   digest and its derived-count fields, because LOAD names the entry by index.
   The slot becomes READY only when every section digest matches. The worker
   also captures each tensor's address and length into worker-private slot
   fields, and the LOAD reply carries them to the client.
3. **Assembly.** The worker writes descriptors and shader instructions and
   builds the command stream from the load-state records. Each record must
   match an accepted template, so the stream cannot program the NPU MMU or a
   register outside the template set. Each relocated field must lie in the
   region of its role: descriptor, instruction and command pointers in the
   read-only region, inputs in the read window, pool and outputs in the write
   window. Catalogue and upload builds share this check because it costs one
   comparison per relocated field (659 for ShuffleNetV2) and needs no build
   condition. After the stream the worker
   writes END pairs up to the slot's high-water mark, the highest command
   offset any earlier LOAD wrote, including a refused one, and then sets the
   mark to the new stream end. The NPU prefetches the whole 524,280-byte span
   on every trigger, so every byte after the stream must be an END pair.
4. **Job.** The client writes inputs in place and sends SUBMIT. The reply is
   held until the job is DONE or FAULT; the read and write windows are
   device-owned in between. The client then reads outputs in place, and USB0
   transmits from the output window. Outputs stay client-owned until the next
   SUBMIT, LOAD or CLOSE, which replaces WAIT and REAP. A client has one
   request outstanding, so a queued job cannot be cancelled; the start bound
   below and the job deadline limit it.

Admission runs once per load. The per-job checks are those whose inputs change
per job: slot state, input count and input lengths.

## Interrupts, polling and the worker loop

The runtime requires the NPU interrupt (INTID 97) binding through
`npu_service_bind_irq`; it has no tick-polled mode. Until board evidence
qualifies delivery, the one-tick poll stays as the timeout path while a job
runs and no bulk work is pending (section 1.3). Clients block on their reply
queues. The `vTaskDelay(1)` loops in `apps/npu-runtime-large` become
notifications from the USB0 request handler.

Bulk work (loads, job preparation and the interim post-job check) runs in
steps. **The worker does not block while a step remains.** Between steps it
receives from its request queue with a zero timeout, handles interrupt
progress, and runs the next step. It blocks, with the interrupt notification
or the job deadline as its timeout, only when no step remains. Steps run in
this order:

1. preparation of the job that round-robin dispatch starts next;
2. post-job checks of finished jobs, which release their replies;
3. pool preparation of slots with no queued job;
4. LOAD steps.

Each step takes at most C = 100 µs at 494 MHz: 1 KiB of portable SHA-256
(8 KiB when `ID_AA64ISAR0_EL1` reports the SHA-2 instructions), 64 KiB of
CRC32C, 32 KiB maintained or zeroed, or 1 KiB of records parsed. The worker
reads `ID_AA64ISAR0_EL1` once at initialization. The work done before each
trigger, G below, runs as one block because the NPU is idle while it runs.

On core 0 the worker runs at priority 3, below the USB0 loop at 4 and above the
single-core image's client at 2. USB0 preempts the worker for at most one
200 µs burst per 1 ms tick, so completion handling waits at most C + 200 µs and
the worker keeps at least 80 % of core 0. Core-0 tasks below priority 3 wait
while bulk work remains, up to one load; the single-core client waits on that
load's reply anyway.

## Memory placement and cache policy

One parent allocation comes from the write-back cached DDR device pool, mapped
at EL1 with 2 MiB blocks; loads allocate nothing. Regions are grouped by CPU
permission: the read-only regions with the NPU table arena, the read windows,
and the write windows. Each group starts and ends on a 2 MiB boundary. The NPU
page tables are built once and never rebuilt; unmapped device pages separate
regions. The NPU MMU entries of the read-only group carry no write permission,
but whether the hardware enforces that bit is unqualified
([docs/npu-mmu.md](../npu-mmu.md)). Device pages stay 4 KiB (section 2,
item 8), because the memory policy verifies only 4 KiB entries.

Each slot's command allocation and the bootstrap allocation stay 524,288 bytes,
and the controller keeps its fixed fetch count of 65,535 eight-byte units
([docs/npu-controller.md](../npu-controller.md)); smaller counts are
unqualified. Each input and output tensor is its own 4 KiB-aligned allocation
padded to whole 64-byte lines, so no client data shares a line with an output.

The NPU is not coherent. Maintenance is by virtual address to the point of
coherency through `aarch64_device_cache`:

- **LOAD** cleans the read-only bytes it wrote.
- **Pool preparation** starts when the slot's previous job has passed its
  post-job check. It zeroes the pool with `DC ZVA` and cleans and invalidates
  it. The worker reads `DCZID_EL0` once at initialization, uses its block size,
  and refuses initialization when it reports `DC ZVA` prohibited.
- **Job preparation** starts at SUBMIT. It cleans the input bytes and cleans
  and invalidates the output bytes, so a dirty client line cannot be evicted
  over NPU output. The input clean (about 0.1 ms per ShuffleNetV2 job) stays
  on core 0, because a client-side clean needs `SCTLR_EL1.UCI` for every EL0
  task; it moves to the client first if the board shows W > D (below).
- **Completion** invalidates the output bytes.

Barriers: the last maintenance batch of a preparation ends with the `DSB SY`
in `aarch64_device_cache`, which section 1.2 permits after cache maintenance
and which completes it before the `a733_npu_submit` register write. The
controller keeps its existing barrier between the command-address write and
the trigger write. After the completion invalidation the worker issues
`DSB ISH` before it releases the reply, since only CPU observers remain.

Every model clears the pool per job. Catalogue images drop the clear once the
conformance package's SDK-simulator run shows that no catalogue model's output
depends on the pool's initial contents.

**Fixed mappings,** requested from the primary agent, are set when the CPU
tables are built: the read-only and write groups read-only at EL0 and EL1, the
read group read-write, and an EL1-only read-write alias of the read-only and
write groups for the worker's load and pool-clear code. The tables stay
sealed, so there is no runtime TLB maintenance. A stray CPU write then faults
at the writing instruction.

## Interim integrity checks

Until the fixed mappings exist, the parent allocation has one set of CPU
permissions, and the interim CPU tables provide no task isolation
([docs/cpu-coherence.md](../cpu-coherence.md)). Any task that can write a
read-only region can also write every slot's windows directly, and the NPU
tables confine the device to that allocation. A writer that chooses its bytes
therefore gains nothing by going through the NPU, so the interim checks need to
detect accidental writes only. CRC32C suffices for that, and its instructions
are mandatory on the Armv8.2 A55. NPU table words and the command tail are
compared word by word. All of the following sit in
`drivers/npu/a733_npu_runtime_interim.c` behind `A733_NPU_RUNTIME_INTERIM`,
which every image sets, and are deleted when the fixed mappings land. The
right-hand column names the check that covers the same failure afterwards.

| Interim item | Covered afterwards by |
| --- | --- |
| Table scan T: every NPU table word compared after each completion, before any reply, and again before a trigger that does not follow a completion directly | Table arena in the read-only group: a CPU write faults |
| Pre-trigger check A of the next slot: CRC32C of its address-bearing bytes (NN descriptors, shader instructions, command payload; 27,056 bytes for ShuffleNetV2) and a word-by-word END compare of its command tail | Read-only group mapping |
| Post-job check H, run while the next job runs: CRC32C of the slot's read-only bytes and its END tail compare | Read-only group mapping |
| The service flag that holds a SUBMIT reply until the runtime releases it after H, and the H term in the reply bound | The service's existing hold until DONE or FAULT, which stays |
| Reference CRC32C fields per slot, computed at LOAD | Not needed |
| Output clean and invalidate at job preparation | Write group read-only at EL0 and EL1 |
| The three failure rows marked *interim* below | A write fault at the writing instruction |

H reads through the CPU cache without invalidating, so it detects CPU writes
only. NPU writes to read-only bytes are excluded by the LOAD role check, which
places every pool and output address in the write window and which A keeps
valid, and by the pending board check of NPU MMU write permission. A CPU write
to the pool or outputs between preparation and trigger goes undetected until
the fixed mappings.

The reference CRC32C values come from the LOAD pass that writes the bytes: for
coefficients and shader instructions, the same pass that computes the section
SHA-256; for relocated descriptors and the command stream, one CRC32C over the
written bytes. No byte is hashed twice at LOAD.

## Synchronization across cores and the start bound

Clients run at EL0 on cores 1 to 5 in multicore images, on cores 6 and 7 once
the multicore work admits them, and on core 0 in the single-core image.
Each client is the only sender for its slot and has one request pending, which
serializes LOAD and SUBMIT for that slot without a lock. Only the worker writes
the slot state word; the client library reads it with acquire ordering.
Worker-written and client-written slot fields sit on separate 64-byte lines
(section 2, item 6). Queue operations order a client's input writes before the
worker cleans them, and maintenance by address is broadcast to the
inner-shareable domain, so core 0 cleans lines dirty on core 6.

Per-job terms: D is the image's job deadline cap, G = T + A the work before a
trigger, P the job's pool and job preparation, H the post-job check, and
W = P + H the worker's bulk work per job. While W ≤ D, the worker's per-job
work hides behind the NPU; otherwise the worker sets the start period to
G + W. With S busy slots, the wait before a start is at most

(S − 1) × (G + max(D, W)) + P + G + C,

and a held reply arrives within that wait plus D + G + H + C. Every worker
term is multiplied by 1.25 where USB0 shares core 0. A host check computes
both bounds for each image configuration from its region sizes and the
per-byte rates at 494 MHz (the estimated upper rates until the board measures
them) and refuses an image whose client collect timeout is shorter than the
reply bound. NPU jobs cannot be preempted, so time-weighted fairness is not
offered.

## Reuse and removal

Moved from `npu_session.c`: one-parent placement with guard pages, catalogue
admission, handoff, binding, and startup and stop polls. Reused: the
service's round-robin dispatch, held replies and interrupt binding; the memory
policy; the controller; `nbg_model_open`; the checksum verifier; the general
assembler; the device-pool reservation; and the shared USB0 loop. The service
in `drivers/npu/npu_service.c` gains LOAD as a control operation with a held
reply, a runtime hook that runs bulk steps under the worker rule above, and a
callback before `a733_npu_submit` that runs G, so a refusal faults only that
slot.

Removed from the job path: per-job translations, the client's pool `memset`,
the input and output copies, WAIT and REAP, and the per-job SHA-256 of the
model. Removed from loading: the private copy of every model (964,224 bytes for
ShuffleNetV2) and its digests at startup, activation and stop. Also removed:
tick-polled completion and any per-model choice of pool clear.

The fixed assembler in [nbg_sh_nt.c](../../drivers/npu/nbg_sh_nt.c) is folded
into the general assembler once the relative-shift fixture assembles to
identical bytes. The assembler gains load-state templates for shader and
tensor-processor (kind 3) operations beside NN (kind 2) and neural transpose
(kind 8). Each template enters with a pinned-compiler fixture, its SDK capture
and a host check against the Python reference assembler. The shader capture
must include the instruction-cache invalidation the SDK issues when shader
instructions change.

## Budgets and measurement

Figures are for ShuffleNetV2 in one slot (964,224-byte model, 150,528-byte
input, 2-byte output, 1,093,632-byte pool, 27,056 address-bearing bytes, about
893,000 coefficient bytes). Baselines are counted from the code and are the
same in `npu_convolution`'s published NN graph build and in `npu_session.c`
with ShuffleNetV2 as its only model. Time estimates assume, at 494 MHz,
portable SHA-256 at 15–25 cycles per byte, the SHA-2 instructions at 2–3,
CRC32C at 4–8 bytes per cycle and an END compare at 4–8 bytes per cycle.

| Metric | Baseline | Interim | With fixed mappings and the pool result |
| --- | --- | --- | --- |
| Requests and replies per inference | 3 and 3 (SUBMIT, WAIT, REAP) | 1 and 1 | 1 and 1 |
| Client wake-ups per inference | 3, plus one per tick in `apps/npu-runtime-large` while idle | 1 | 1 |
| `AT` translations per inference | 2,126 | 0 | 0 |
| SHA-256 bytes per inference | About 920,000, before the trigger | 0 | 0 |
| CRC32C bytes per inference | 0 | 27,056 before the trigger; about 920,000 after the job | 0 |
| END words compared per inference | About 65,000, before the trigger | 65,153 before the trigger and 65,153 after the job | 0 |
| NPU table bytes scanned per inference | 20,480 | 20,480 per scan, one or two scans; at most about 36 KiB per scan with eight slots | 0 |
| CPU bytes cleared or copied per inference | 1,244,162 | 1,093,632 zeroed by `DC ZVA`; none copied | 0 |
| Cache maintenance bytes per inference | 2,347,008 | 1,244,288 | 150,592 |
| G, before each trigger, any slot count | Every row above except the client copies | T + A; estimated 0.15–0.3 ms | 0 |
| NPU idle per inference with S = 1, estimated | 30–50 ms | 0.9–1.9 ms (T + H + P + A, plus client time) | 0.05–0.1 ms (input clean) |
| Largest worker step C | No worker bulk work | 100 µs, sizes above | Same |
| Blocking worker waits with bulk work pending | Not applicable | 0 | 0 |
| One ShuffleNetV2 catalogue load | Not supported after startup | 964,224 bytes SHA-256 once and about 920,000 CRC32C; 0 translations, 0 allocations; estimated 30–50 ms, or 4–7 ms with the SHA-2 instructions | 964,224 bytes SHA-256 once |
| Start order with S busy slots | Period S | Period S, also while another slot loads | Same |

H is estimated at 0.35–0.7 ms and P at 0.4–0.9 ms, so W is 0.75–1.6 ms. D for
ShuffleNetV2 is unmeasured; whether W ≤ D holds is a board item below. With
S = 1 the NPU is idle for H and P after every job, because the single client's
next SUBMIT waits for its reply.

[checks/npu-session.c](../../checks/npu-session.c) and
[checks/npu-session.py](../../checks/npu-session.py), with two QEMU cores and a
register model that reads descriptors through the NPU tables, move to the
runtime and keep their cases. Their counters go in first and run against
`npu_session.c` to record the baseline. They count translate and cache
callbacks; SHA-256, CRC32C, `memset`, `DC ZVA` and `memcpy` bytes through
`--wrap`, attributed to worker, client, G or H; table and END words; the
largest step in bytes and, with the rates above, microseconds; service sends
and collects; `vTaskDelay` calls; and blocking waits taken while a step
remains. The register model completes a job on a credit. New cases:

- load model A, run, load the shorter-stream model B into the same slot, run;
  a refused LOAD that wrote past A's stream, followed by B: the END tail
  reaches the high-water mark;
- three slots running while a fourth loads, and credits that make jobs shorter
  than W: the worst measured start stays within the bound;
- a catalogue byte changed between two section reads of a LOAD: refused, slot
  never READY;
- a command record outside the templates, and a relocated field outside its
  role's region: refused at LOAD;
- a descriptor byte changed while its slot is idle: refused in G, other slots
  continue;
- a coefficient byte changed while its job runs: post-job mismatch;
- CPU writes to the output and pool before SUBMIT: QEMU models no caches, so
  the case checks that preparation maintains both extents before the trigger;
- `DCZID_EL0` reporting `DC ZVA` prohibited: initialization refused.

## Failure classes (section 1.8)

| Failure | Class | Retained |
| --- | --- | --- |
| LOAD refused: format, section digest, template, role, size or, in upload builds, staging state | 1 | Nothing; slot EMPTY |
| SUBMIT refused: slot not READY, wrong input count or length | 1 | Nothing |
| *Interim:* next slot's address-bearing bytes or command tail changed, found in G | 1 | That slot is FAULT until reloaded; its last job passed H, so the device has not read the change |
| *Interim:* NPU table words changed | 3 | Every slot and the controller |
| *Interim:* post-job check mismatch | 3 | Every slot and the controller |
| Job deadline or controller fault | 3 | Every slot and the controller until the hang-reset package supplies a verified quiesce |

## Uploads: an optional build, off in every board image

The plan asks for arbitrary model uploads, but no board consumer needs them.
`A733_NPU_RUNTIME_UPLOAD` holds every upload-only part: one staging area sized
to the largest accepted file, its FREE, USB, SEALED states (EL1-only state
word, release stores and acquire loads), the USB0 chunk handler, the NBG
format-checksum path, the rule that uploaded models always clear the pool, and
the upload check set. Staging returns to FREE on CLOSE, on a refused LOAD, and
when no LOAD arrives within an image constant after sealing. Uploads skip the
identity SHA-256 and compute the interim reference CRC32C values. A relocated
upload covers only the fields the loader parses, so an uploaded model can read
every slot and write every write window. The image configuration therefore
refuses the option unless every slot belongs to one client. Deferring this
part until the NPU MMU board checks pass is proposed to the plan owner.

## Changes needed from other owners

- **Session code:** delete `npu_session.c`, move its catalogue and reservation
  to the runtime, and change `tools/npu-session-catalogue.py` to emit
  per-section SHA-256.
- **Single-core provider:** `apps/npu-convolution/native.c` constructs the
  runtime for `apps/npu-runtime-large`.
- **Memory policy:** per-buffer maintained extents, a preparation call outside
  publication, mapping checks once at initialization, and one allowed alias
  pair: the EL1 alias registered in the buffer registry, with write permission
  checked on the alias and read-only permission on the client view; details in
  that owner's issue.
- **Shared digest helper:** the SHA-2 instructions when `ID_AA64ISAR0_EL1`
  reports them.
- **Primary agent:** the fixed mappings, and A76 admission for client affinity.
- **Hang-reset package:** a verified quiesce and reset, making controller
  faults class 2.
- **Conformance package:** the pool-pattern comparison and the numerical
  conformance of new operators.

## Left out

- **Isolation between models in different slots.** Per-job page-directory
  switching needs an unqualified NPU MMU flush sequence.
- **Software (CPU) layers and compilation on the device.** Loads refuse them;
  they need CPU kernels and a buffer hand-off inside a graph.
- **Cancelling a queued job,** which needs a second outstanding request per
  client.
- **The crypto engine for digests,** a separate DMA master with its own
  ownership; no SHA-256 remains per inference.
- **Weaker MMIO barriers,** which need the board qualification in
  [docs/shared-helpers.md](../shared-helpers.md).
- **One shared END page behind every command tail,** which would shrink A and
  H but needs physical aliases that the memory policy forbids.
- **Mapping the image's model data read-only,** which would let the
  whole-model digest replace per-section digests; it is a further
  primary-agent request with no measured benefit.

## Board evidence that would confirm the design

- Two models with different shader programs, loaded alternately into one slot,
  each match their SDK outputs.
- Every job completes by interrupt, with zero fallback-poll completions.
- A descriptor that addresses a guard page raises an NPU MMU fault, and one
  that writes into a read-only page raises an NPU MMU permission fault.
- The start order matches the host model.
- Back-to-back runs without a pool clear match the reference outputs.
- D, G, H, P, W and the S = 1 idle time, measured with the generic timer at the
  clock core 0 runs, with `ID_AA64ISAR0_EL1` recorded, show W ≤ D for each
  image's models.
