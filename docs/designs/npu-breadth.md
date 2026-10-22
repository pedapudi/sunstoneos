# Design: loading, switching and sharing compiled NPU models at runtime

Status: **design**; no code exists. Section numbers refer to
[docs/performance-architecture.md](../performance-architecture.md).

The runtime, `drivers/npu/a733_npu_runtime.c` with the interface
`include/npu_runtime.h`, replaces the concurrent-session code in
[npu_session.c](../../drivers/npu/npu_session.c). Up to eight client tasks each
own one **slot**: fixed memory and a command-table entry that hold one model at
a time. A client can switch its slot to another model while other slots run
jobs, and jobs from every slot share the NPU with round-robin starts. Each slot
has three regions. The **read-only region** holds NN descriptors,
coefficients, shader instructions and a 524,288-byte command allocation. The
**read window** holds inputs. The **write window** holds the working pool and
the outputs. The image fixes the number of slots and every region's size.

The first consumer is `apps/npu-runtime-large` with one slot, whose client
switches among the catalogue models the image carries. Core 0 runs at Boot0's 494 MHz unless
the CPU clock driver raises CPU-L ([docs/cpu-clock.md](../cpu-clock.md)), so
every time below is an estimated worst case at 494 MHz.

## Data flow and ownership

1. **Initialization** fills each command allocation with END pairs, cleans it,
   and sets the slot's **END mark** to zero. The END mark is the offset beyond
   which every command byte is already an END pair.
2. **LOAD.** The client sends LOAD with a catalogue index. The worker accepts it
   only when the slot has no job, marks the slot LOADING and holds the reply
   until the slot is READY or the load is refused. Only the worker, at EL1,
   writes the read-only region.
3. **Single read.** The worker reads every source byte once. Parsed sections
   (header, directory, load-state records, NN descriptors) pass through a
   4 KiB worker-private buffer, and the worker parses only that buffer.
   Coefficients and shader instructions are copied into the slot, and the
   worker hashes the destination bytes, which the device reads. Each catalogue
   entry records one SHA-256 per section, and the slot becomes READY only when
   every section digest matches. The LOAD reply carries each tensor's
   address and length.
4. **Assembly.** The worker builds the command stream from the load-state
   records. Each record must match an accepted template, so the stream cannot
   program the NPU MMU or a register outside the template set. Each relocated
   field must lie in the region of its role: descriptor, instruction and command
   pointers in the read-only region, inputs in the read window, pool and
   outputs in the write window. This costs one comparison per relocated field
   (659 for ShuffleNetV2). The worker then writes END pairs from the stream end
   up to the END mark and sets the mark to the stream end; a refused LOAD sets
   it to the highest offset it wrote, if larger. The NPU prefetches the whole
   524,280-byte span on every trigger, so every byte after the stream must be
   an END pair.
5. **Job.** The client writes inputs in place and sends SUBMIT. The reply is
   held until the job is DONE or FAULT; the read and write windows are
   device-owned in between. The client then reads outputs in place, and USB0
   transmits from the output window. Outputs stay client-owned until the next
   SUBMIT, LOAD or CLOSE, which replaces WAIT and REAP. The per-job checks are
   those whose inputs change per job: slot state, input count and lengths.

## Interrupts, polling and the worker loop

The runtime binds the NPU interrupt (INTID 97) through `npu_service_bind_irq`;
it has no tick-polled mode. Clients block on their reply queues, and the
`vTaskDelay(1)` loops in `apps/npu-runtime-large` become notifications from
the USB0 request handler.

Bulk work (loads, job and pool preparation, the interim post-job check) runs in
steps of at most C = 100 µs. **The worker does not block while a step
remains.** Between steps it receives from its request queue with a zero
timeout. While INTID 97 delivery is unqualified, it also reads the controller
completion status between steps, one MMIO read per step, so a lost interrupt
delays completion by at most C. With no step left, it blocks on its queue and
the interrupt notification, with a one-tick timeout while a job runs
(section 1.3). Board evidence of interrupt delivery removes the
step-time read and the one-tick timeout. Steps run in this order:

1. preparation of the job dispatch starts next, while the NPU is idle;
2. post-job checks of finished jobs, which release their replies;
3. preparation of the job dispatch starts next, while the NPU runs, then pool
   preparation of slots with no queued job;
4. LOAD steps. After each trigger, one pending LOAD step runs first, so a load
   advances at least one step per job period.

Step sizes come from the worst-case rates under "Budgets and measurement": the
largest power of two whose worst case fits 80 µs, leaving 20 µs of C for the
loop, the queue poll and the status read. They are 1 KiB of portable SHA-256
(8 KiB with the SHA-2 instructions), 32 KiB of CRC32C or END compare, 16 KiB
of maintenance by address, 16 KiB of pool zeroing with its clean and
invalidate, and 1 KiB of records parsed, checked against the instruction count
the host emulation measures. Whether the cores have the SHA-2
instructions is a board constant. Once board evidence records
`ID_AA64ISAR0_EL1`, initialization refuses a different value; until then the
constant selects the portable code. The pre-trigger work G (below) runs as one
block, because the NPU is idle while it runs.

On core 0 the worker runs at priority 3, below the USB0 loop at 4 and above the
single-core client at 2. USB0 preempts the worker for at most one 200 µs burst
per 1 ms tick, so every worker term is multiplied by 1.25 where USB0 shares
core 0. Under steady load the worker occupies (G + W) / (G + max(D, W)) of
core 0 per job period, plus loads; that is all of it while W ≥ D. Core-0 tasks
below priority 3 then do not run. In the first consumer those are the client,
which waits on the worker's reply, and the idle task, which has no required
work (allocation is static and there is no idle hook). An image that puts a
task with a liveness requirement on core 0 must give it a priority above 3.

## Memory placement and cache policy

One parent allocation comes from the write-back cached DDR device pool, mapped
at EL1 with 2 MiB blocks; loads allocate nothing. Regions are grouped by CPU
permission: the read-only regions with the NPU table arena, the read windows,
and the write windows. Each group starts and ends on a 2 MiB boundary. The NPU
page tables are built once; unmapped device pages separate regions. The NPU
MMU entries of the read-only group carry no write permission, which the
hardware is not yet shown to enforce ([docs/npu-mmu.md](../npu-mmu.md)).
Device pages stay 4 KiB (section 2, item 8), because the memory policy verifies
only 4 KiB entries. The controller keeps its fixed fetch count of 65,535
eight-byte units ([docs/npu-controller.md](../npu-controller.md)). Each tensor is its own 4 KiB-aligned allocation padded to whole
64-byte lines, so no client data shares a line with an output.

The NPU is not coherent. Maintenance is by virtual address through
`aarch64_device_cache`. The permanent per-job maintenance is a clean of the
input bytes at job preparation and an invalidate of the output bytes at
completion. In addition:

- **LOAD** cleans the read-only bytes it wrote.
- **Pool preparation**, after the slot's previous job passed its post-job
  check, zeroes the pool with `DC ZVA` and cleans and invalidates it. The
  64-byte `DC ZVA` block is a constant; initialization refuses a `DCZID_EL0`
  that disagrees or prohibits `DC ZVA`. The clear stays for every model until
  the conformance package's SDK-simulator run shows that no catalogue model's
  output depends on the pool's initial contents.
- **Interim:** job preparation also cleans and invalidates the output bytes,
  so a dirty client line cannot be evicted over NPU output.

Each maintenance call ends with the `DSB SY` in `aarch64_device_cache`
(section 1.2). It completes preparation before the `a733_npu_submit` register
write and the completion invalidate before the reply. The controller keeps its
barrier between the command-address and trigger writes. No other barrier is
added.

**Fixed mappings,** requested from the primary agent, are set when the CPU
tables are built: the read-only and write groups read-only at EL0 and EL1, the
read group read-write, and an EL1-only read-write alias of the read-only and
write groups. The tables stay sealed, so there is no runtime TLB maintenance,
and a stray CPU write faults at the writing instruction. The worker loads
models, clears pools and invalidates outputs through the alias, because
`DC IVAC` needs write permission. Maintenance through either alias acts on
the same physical line. Until the fixed mappings exist, the alias and the
client view are one address.

## Interim integrity checks

Until the fixed mappings exist, the parent allocation has one set of CPU
permissions, and the interim CPU tables give no task isolation
([docs/cpu-coherence.md](../cpu-coherence.md)). No check can stop a deliberate
CPU writer, which can change bytes after a check runs, from another core or by
preempting the worker. The checks below therefore target accidental writes,
and CRC32C detects those; its instructions are mandatory on the Armv8.2 A55.
An accidental change matters most in the command payload: it could program
registers outside the template set, including the NPU MMU base, and reach
memory outside the parent allocation, such as the monitor at `0x42000000`.

All interim checks sit in `drivers/npu/a733_npu_runtime_interim.c`, which the
change that lands the fixed mappings deletes. Costs are worst case for
ShuffleNetV2.

| Check | Bytes read per job | Time | Covered afterwards by |
| --- | --- | --- | --- |
| T: every NPU table word compared after each completion, before any reply, and before a trigger that does not follow a completion | 20,480 per scan; one or two scans; up to about 36 KiB with eight slots | 0.04 ms per scan | Table arena in the read-only group |
| A, before each trigger: CRC32C of the next slot's address-bearing bytes (NN descriptors, shader instructions, command payload) and a word-by-word END compare of its command tail | 27,056 and 521,224 | 1.11 ms, of which the tail compare is 1.06 ms | Read-only group |
| H, while the next job runs: CRC32C of the slot's read-only bytes and its END tail compare; the service holds the SUBMIT reply until H passes | About 920,000 and 521,224 | 2.92 ms | Read-only group; the service's hold until DONE or FAULT stays |
| Output clean and invalidate at job preparation | 64 | Under 1 µs | Write group read-only |

The reference CRC32C values come from the LOAD pass that writes the bytes, so
no byte is hashed twice at LOAD. H reads through the CPU cache without
invalidating, so it detects CPU writes only. A CPU write to the pool or outputs
between preparation and trigger goes undetected until the fixed mappings.

The tail compare is 92 % of G = T + A, and G is NPU idle time before every
trigger for any slot count. With one slot the tail is compared twice per job,
in H and then in A, which is 2.1 ms or 25 % of that case's 8.4 ms NPU idle
time. The interim checks also stream about 2.0 MB per job through the shared
cache levels and can evict client working sets on other cores. Board evidence
measures a client kernel with and without the runtime running; if the kernel
slows, the read-only streaming checks switch to non-temporal `LDNP` loads.

## Synchronization across cores

Clients run at EL0 on cores 1 to 5 in multicore images, and on core 0 in the
single-core image. Cores 6 and 7 take clients only after the primary agent's
A76 admission shows the A76 cluster in the same inner-shareable domain as
core 0, so that maintenance by address on core 0 reaches lines dirty on an A76
core. Each client is the only sender for its slot and has one request pending,
which serializes LOAD and SUBMIT for that slot without a lock. Only the worker
writes the slot state word; the client library reads it with acquire ordering.
Worker-written and client-written slot fields sit on separate 64-byte lines
(section 2, item 6). Queue operations order a client's input writes before the
worker cleans them.

## Start and reply bounds

D is the image's job deadline cap, G = T + A the work before a trigger, P the
job's pool and job preparation, H the post-job check, and W = P + H + C the
worker's bulk work per job, including the guaranteed LOAD step. While W ≤ D the
bulk work hides behind the NPU; otherwise the start period is G + W. With S
busy slots, the wait before a start is at most

(S − 1) × (G + max(D, W)) + P + G + C,

and a held reply arrives within that wait plus D + P + G + H + 2C. The P term
is the preparation of the next job when it was not ready at completion; the
step order runs a finished job's H before any later preparation. Worker terms
take the 1.25 factor where USB0 shares core 0. A host check computes both
bounds for each image configuration from its region sizes and the worst-case
rates, and refuses an image whose client collect timeout is shorter than the
reply bound. NPU jobs cannot be preempted, so time-weighted fairness is not
offered.

While W > D the NPU completes one job per G + W for any S ≥ 2, and each slot
gets one start per S × (G + W). The images accept that throughput and record
the measured W and D in their evidence. Each step below applies while the
measured W / D after the previous steps still exceeds 1:

1. Request CPU-L at 1196 MHz (2.4 times the clock) once the CPU clock driver
   qualifies that point. Instruction-bound terms scale by 2.4; DDR-bound terms
   scale by less.
2. Schedule the conformance pool comparison first, and drop the clear for each
   model whose outputs it shows independent of the pool contents, which removes
   3.87 ms (53 %) of W. A model that needs a clear
   gets a DMA zero-fill of its pool, which leaves no CPU-dirty pool lines to
   maintain.
3. When D is below about H plus the input clean (3.4 ms), split H across an
   idle A55 core until the fixed mappings remove it.
4. When D is below about 0.5 ms, move the input clean to the client, which
   needs `SCTLR_EL1.UCI` for every EL0 task.

Each step is a later change with its own design.

## Reuse and removal

Moved from `npu_session.c`: one-parent placement with guard pages, catalogue
admission, handoff, binding, and startup and stop polls. Reused: the service's
round-robin dispatch, held replies and interrupt binding; the memory policy;
the controller; `nbg_model_open`; the general assembler; the device-pool
reservation; and the USB0 loop.
`drivers/npu/npu_service.c` gains LOAD as a control operation with a held
reply, a runtime hook that runs bulk steps under the worker rule above, and a
callback before `a733_npu_submit` that runs G, so a refusal faults only that
slot.

Removed from the job path: per-job translations, the client's pool `memset`,
the input and output copies, WAIT and REAP, and the per-job SHA-256 of the
model. Removed from loading: the private copy of every model (964,224 bytes for
ShuffleNetV2) and its digests at startup, activation and stop. Also removed:
tick-polled completion and any per-model choice of pool clear.

The fixed assembler in [nbg_sh_nt.c](../../drivers/npu/nbg_sh_nt.c) folds into
the general assembler once the relative-shift fixture assembles to identical
bytes. The assembler gains templates for shader and tensor-processor (kind 3)
operations beside NN (kind 2) and neural transpose (kind 8). Each template
enters with a pinned-compiler fixture, its SDK capture, including the
instruction-cache invalidation the SDK issues when shader instructions change,
and a host check against the Python reference assembler.

## Budgets and measurement

Figures are for ShuffleNetV2 in one slot: 964,224-byte model, 150,528-byte
input, 2-byte output, 1,093,632-byte pool, 27,056 address-bearing bytes and
about 893,000 coefficient bytes. Baselines are counted from the code and are
the same in `npu_convolution`'s published NN graph build and in
`npu_session.c` with ShuffleNetV2 as its only model.

Times use these estimated worst-case rates at 494 MHz, which the board
replaces with measured ones:

- `DC ZVA`: 16 cycles per 64-byte line;
- `DC CVAC`, `DC CIVAC` and `DC IVAC`: 96 cycles per line, a dirty line written
  back to DDR;
- a DDR-resident streaming read, which bounds CRC32C, the END compare and the
  table scan: 64 cycles per line (1 byte per cycle, about 0.5 GB/s);
- SHA-256: 25 cycles per byte portable, 3 with the SHA-2 instructions.

| Metric | Baseline | Interim | With fixed mappings and the pool result |
| --- | --- | --- | --- |
| Requests and replies per inference | 3 and 3 (SUBMIT, WAIT, REAP) | 1 and 1 | 1 and 1 |
| Client wake-ups per inference | 3, plus one per tick in `apps/npu-runtime-large` while idle | 1 | 1 |
| `AT` translations per inference | 2,126 | 0 | 0 |
| SHA-256 bytes per inference | About 920,000 | 0 | 0 |
| Integrity-check bytes read per inference | About 540,000 (END words and table) | About 2,030,000 (T, A and H) | 0 |
| CPU bytes zeroed or copied per inference | 1,244,162 | 1,093,632 zeroed by `DC ZVA`; none copied | 0 |
| Cache lines maintained per inference | 36,672 | 19,442: pool 17,088, input 2,352, outputs 2 | 2,353: input 2,352, output 1 |
| P / H / G | No worker | 4.33 / 2.92 / 1.15 ms | 0.46 / 0 / 0 ms |
| W, and W with USB0 on core 0 | No worker | 7.35 and 9.19 ms | 0.56 and 0.70 ms |
| NPU idle per inference with S = 1 | About 57 ms, SHA-256 47 ms of it | 8.4 ms plus client time | 0.46 ms plus client time |
| Largest worker step | No worker | C = 100 µs | Same |
| Blocking worker waits with bulk work pending | Not applicable | 0 | 0 |
| One ShuffleNetV2 catalogue load | Not supported after startup | 52 ms portable, 9 ms with the SHA-2 instructions; 0 translations, 0 allocations | Same |
| Start order with S busy slots | Period S | Period S, also while another slot loads | Same |

D for ShuffleNetV2 is unmeasured. A small network may run in a few
milliseconds on the NPU, so W > D is likely in the interim, and the ordered
steps above apply. The derivation of each time from the rates sits in the
comments of the host bound check.

[checks/npu-session.c](../../checks/npu-session.c) and
[checks/npu-session.py](../../checks/npu-session.py), with two QEMU cores and a
register model that reads descriptors through the NPU tables, move to the
runtime and keep their cases. Their counters go in first and run against
`npu_session.c` to record the baseline. They count translate and cache
callbacks; cache lines maintained and lines zeroed, attributed to P, G and H;
SHA-256, CRC32C, `memset`, `DC ZVA` and `memcpy` bytes through `--wrap`;
table and END words; the largest step in bytes and, with the rates, in
microseconds; service sends and collects; `vTaskDelay` calls; blocking waits
taken while a step remains; completions seen by interrupt and by the
step-time status read; and the worker's share of core 0 per job period. The
register model completes a job on a credit. New cases:

- load model A, run, load the shorter-stream model B into the same slot, run;
  a refused LOAD that wrote past A's stream, then B: the END tail reaches the
  END mark;
- three slots running while a fourth loads, with credits that make jobs
  shorter than W: the worst measured start stays within the bound and the
  load completes;
- SUBMITs from two other slots arriving just as a job completes: the measured
  reply stays within the reply bound;
- a dropped interrupt while bulk work is pending: the step-time read observes
  completion within C;
- refusals: a catalogue byte changed between two section reads of a LOAD, a
  command record outside the templates, a relocated field outside its role's
  region, and a `DCZID_EL0` that disagrees with the constant;
- a descriptor byte changed while its slot is idle, and a coefficient byte
  changed while its job runs: mismatch in G or H, every slot retained;
- CPU writes to the output and pool before SUBMIT: QEMU models no caches, so
  the case checks that preparation maintains both extents before the trigger.

## Failure classes (section 1.8)

| Failure | Class | Retained |
| --- | --- | --- |
| LOAD refused: format, section digest, template, role or size | 1 | Nothing; slot EMPTY |
| SUBMIT refused: slot not READY, wrong input count or length | 1 | Nothing |
| *Interim:* T, A or H mismatch | 3 | Every slot and the controller |
| Job deadline or controller fault | 3 | Every slot and the controller until the hang-reset package supplies a verified quiesce |

An A mismatch is class 3 because the interim checks cannot tell a CPU write
from an NPU write: another slot's job could have written there, since NPU
write permission is unqualified and the role check bounds base pointers only.
It becomes class 1 for that slot once the board shows NPU MMU write permission
enforced.

## Left out

- **Model uploads.** No board consumer needs them, so no upload-only code is
  built until the NPU MMU write-permission and guard-page board checks pass and
  a consumer exists; the role check is the only shared part. Uploaded content
  reaches memory through the NPU, whose write permission is unqualified, so an
  uploaded program could rewrite its own read-only bytes. CRC32C is linear and
  forgeable, so an upload design needs SHA-256 or an exact compare against a
  worker-private copy for the address-bearing bytes.
- **Isolation between slots,** which needs an unqualified NPU MMU flush
  sequence for per-job page-directory switching.
- **Software (CPU) layers and compilation on the device,** which loads refuse.
- **Cancelling a queued job,** which needs a second outstanding request.
- **The crypto engine for digests:** no SHA-256 remains per inference.
- **Weaker MMIO barriers,** which need the board qualification in
  [docs/shared-helpers.md](../shared-helpers.md).
- **One shared END page behind every command tail,** which needs physical
  aliases that the memory policy forbids.

## Changes needed from other owners

- **Session code:** delete `npu_session.c`, move its catalogue and reservation
  to the runtime, and change `tools/npu-session-catalogue.py` to emit
  per-section SHA-256.
- **Single-core provider:** `apps/npu-convolution/native.c` constructs the
  runtime for `apps/npu-runtime-large`.
- **Memory policy:** per-buffer maintained extents on the EL1 alias, a
  preparation call outside publication, mapping checks once at initialization,
  and one alias pair: write permission checked on the alias, read-only on the
  client view.
- **Shared digest helper:** the SHA-2 instructions as a board constant.
- **Primary agent:** the fixed mappings with the EL1 alias, and A76 admission
  with inner-shareable coherence to core 0.
- **Hang-reset package:** a verified quiesce and reset, making controller
  faults class 2.
- **Conformance package:** the pool-pattern comparison and the numerical
  conformance of new operators.

## Board evidence that would confirm the design

- Two models with different shader programs, loaded alternately into one slot,
  each match their SDK outputs; back-to-back runs without a pool clear match
  too.
- Every job completes by interrupt, with no step-time or fallback-poll
  completion; the start order matches the host model.
- A descriptor that addresses a guard page raises an NPU MMU fault, and one
  that writes into a read-only page raises a permission fault.
- With the fixed mappings, a job completes and its output invalidate through
  the alias raises no fault.
- Inputs written by a client on an A76 core and cleaned by core 0 reach the NPU.
- A client kernel's time with and without the runtime running.
- `ID_AA64ISAR0_EL1`, `DCZID_EL0`, the per-line rates of `DC ZVA`, `DC CVAC`,
  `DC CIVAC` and `DC IVAC`, the streaming read rate, and D, G, H, P, W and the
  S = 1 idle time, timed with the generic timer at core 0's clock, giving W / D
  for each image's models.
