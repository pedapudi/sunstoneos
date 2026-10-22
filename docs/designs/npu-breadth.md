# Design: loading, switching and sharing compiled NPU models at runtime

Status: **design**; no code exists. Section numbers refer to
[docs/performance-architecture.md](../performance-architecture.md).

Up to eight client tasks each load any compiled model that passes admission,
replace it while other clients keep running jobs, and share the one NPU with
round-robin job starts. New code goes in `drivers/npu/a733_npu_runtime.c` and
`include/npu_runtime.h`. A **slot** is the fixed memory and command-table
entry of one bound client and holds one model at a time. Its **read-only
region** (command stream, NN descriptors, coefficients, shader instructions)
starts on a 2 MiB boundary. Its **read window** holds inputs; its **write
window** holds the working pool and the outputs.

## Data flow and ownership

1. **BEGIN.** The worker accepts it only when the slot has no job and marks the
   slot LOADING, so none of its jobs can start. The USB0 handler writes the
   uploaded file in chunks directly into the slot's staging area, which the
   client owns while loading.
2. **Load, in the client task.** Library code verifies the format checksum,
   records the file's SHA-256 identity, decodes the file and writes
   descriptors, coefficients and shader instructions into the read-only region.
   It zeroes the bytes the previous model used that the new one does not
   overwrite. This pass over the model runs on the client's own core.
3. **COMMIT, in the worker.** The command stream programs NPU registers, so only
   EL1 code writes it. The worker builds it from the file's load-state records,
   reading each record once, checking it against the accepted templates and
   checking that every emitted device address lies inside the slot's windows.
   It then cleans the used read-only bytes, cleans and invalidates the used
   write window, and marks the slot READY.
4. **Job.** The application writes inputs in place and sends SUBMIT, whose
   reply is held until the job is DONE, CANCELED or FAULT. The read and write
   windows are device-owned in between. Afterwards the client reads outputs in
   place and USB0 transmits from the output window. Outputs stay client-owned
   until the next SUBMIT, BEGIN or CLOSE, which replaces WAIT and REAP.

Admission runs once per load. The per-job checks are those whose inputs change
per job: slot state, input count and input lengths.

## Interrupts and polling

Jobs complete from the NPU interrupt (INTID 97) through the existing
`npu_service_bind_irq` binding, with the worker and the interrupt on core 0.
Until board evidence qualifies that delivery, the existing one-tick poll stays
as the timeout path while a job runs (section 1.3); afterwards the worker waits
for the notification with the job deadline as its timeout. Images without the
binding keep the tick poll. Clients block on their reply queue with the
remaining deadline. The `vTaskDelay(1)` loops in
[apps/npu-runtime/main.c](../../apps/npu-runtime/main.c) become notifications
from the USB0 request handler. Loads do not touch the device.

## Memory placement and cache policy

One parent allocation comes from the write-back cached DDR device pool through
`a733_npu_session_reserve`, which maps it at EL1 with 2 MiB blocks. The image
fixes the number of slots, each window's size and each slot's staging area,
which is sized to the largest model file the image accepts. Loads therefore
allocate nothing. The NPU page
tables are built once over every window and never rebuilt. Unmapped device
pages separate windows, and the NPU MMU gives no write permission to read-only
regions. Device pages stay 4 KiB (section 2, item 8): the memory policy verifies
only 4 KiB entries and large-page encodings are unqualified. Windows are 1 MiB
aligned, so 1 MiB pages later need no layout change.

The NPU is not coherent; maintenance is by virtual address to the point of
coherency. Commit cleans the used read-only bytes and cleans and invalidates
the used write window. Job start cleans the input bytes. Completion invalidates
the output bytes. The pool gets no per-job maintenance: after commit the CPU
neither reads nor writes it, so it can hold only clean speculative lines. The
per-job pool clear moves to commit. A model keeps a per-job clear, selected at
load, until the conformance package's SDK-simulator run shows that its outputs
do not change when the pool starts with a nonzero pattern. Such a model also
cleans the pool at job start.

## Synchronization across cores

Clients run at EL0 on cores 6 and 7 once the multicore work admits them, and
on cores 1 to 5 before that. Each client is the only sender for its slot and
has one request pending at a time, which serializes BEGIN, load, COMMIT and
SUBMIT for that slot without a lock. Only the worker writes the slot state
word; the client library reads it with acquire ordering. Worker-written and
client-written slot fields sit on separate 64-byte lines (section 2, item 6).

Queue operations order a client's input writes before the worker cleans them,
and cleaning by address is broadcast to the inner-shareable domain, so core 0
cleans lines dirty on core 6. The worker completes the output invalidation with
a `DSB` before it sends the reply. Loads in different slots run in parallel
over disjoint memory, and the worker never waits for a load.

Round-robin dispatch starts a queued job after at most one job from each other
slot. With every submit deadline capped by an image constant D and S busy
slots, the wait before a start is at most (S − 1) × D. NPU jobs cannot be
preempted, so time-weighted fairness is not offered.

## Reuse and removal

The runtime reuses the shared service mode with its round-robin dispatch, held
replies, interrupt binding and pre-device refusal path. It also reuses the
memory policy's session groups, the native adapter and controller,
`nbg_model_open`, the checksum verifier, the general layout assembler, the
device-pool reservation and the shared USB0 loop. The service gains three
things. A SUBMIT flag holds the reply until completion. One CONTROL operation
runs a runtime callback for a member with no job, for BEGIN and COMMIT. An
optional callback before `a733_npu_submit` checks slot state and, in the
interim, slot digests, so a refusal affects that slot instead of faulting the
controller.

Removed from the job path: the pool clear, the output copy, WAIT and REAP, and
the retained model copy with its startup and stop digest. The fixed assembler
in [nbg_sh_nt.c](../../drivers/npu/nbg_sh_nt.c) is folded into the general
assembler once the relative-shift fixture assembles to identical bytes. That
assembler gains a table of load-state templates per operation kind: NN (kind 2)
and neural transpose (kind 8) as today, plus shader and tensor-processor
(kind 3) operations. Each template enters with a pinned-compiler fixture, its
SDK capture and a host check against the Python reference assembler.
[npu_session.c](../../drivers/npu/npu_session.c) and
[npu_convolution.c](../../drivers/npu/npu_convolution.c), outside this package,
stay for their images; retiring `npu_session.c` is proposed once
`apps/npu-session/` uses the runtime.

## Budgets and measurement

Baselines are counted from the code for ShuffleNetV2 in the large-graph build
of `npu_convolution` (964,224-byte model, 150,528-byte input, 2-byte output,
1,093,632-byte pool allocation).

| Metric | Baseline | Budget |
| --- | --- | --- |
| Requests and replies per inference | 3 and 3 (SUBMIT, WAIT, REAP) | 1 and 1 |
| Client wake-ups per inference | 3, plus one per tick in `apps/npu-runtime` while idle | 1 |
| `AT` translations per inference | 2,126 (1,063 each in publish and finish) | 0 |
| NPU table words read per inference | 4,352 and 3,072 padding bytes | Same until the table arena is read-only to the CPU, then 0 |
| Bytes digested per inference | About 920,000, and an END check of about 65,000 command words | Same until the read-only region is read-only to the CPU, then 0 |
| CPU bytes cleared or copied per inference | 1,244,162 (pool, input, output) | 0 with in-place input, else 150,528; pool clear only where the load selects it |
| Cache maintenance bytes per inference | 2,347,008 | 150,592 |
| Worker wake-ups per job beyond the interrupt | One per tick of execution | Same until delivery is qualified, then 0 |
| Worker work per COMMIT | None exists | Command body and cache maintenance only; no model digest or copy |
| Translations and allocations per load | None exists | 0 and 0 |
| Start order with S busy slots | Period S | Period S, also while another slot loads |

*Estimated:* a ShuffleNetV2 load costs 20–30 ms of client time at 1.8 GHz
(portable SHA-256 at 15–25 cycles per byte); COMMIT maintains about 2 MB of
lines, an estimated 0.2–1 ms of worker time.

A new host model, `checks/npu-runtime-switch.py`, reuses the harness of
`checks/npu-session.py` (two QEMU cores, a register model that reads
descriptors through the NPU tables). It counts translate and cache callback
calls with bytes per operation, digest, memset and memcpy bytes through
`--wrap` (attributed to worker or client), service sends and collects, and
`vTaskDelay` calls. The counters go into `checks/npu-session.c` first, to
measure the baseline. Cases: load model A, run, load B into the same slot,
run; three slots running while a fourth loads; a changed coefficient refused
before device access while other slots continue; a command record outside the
templates refused at COMMIT.

## Failure classes (section 1.8)

| Failure | Class | Retained |
| --- | --- | --- |
| Load or COMMIT refused: format, checksum, template, size, address outside the windows | 1 | Nothing; slot EMPTY |
| SUBMIT refused: slot not READY, wrong input count or length | 1 | Nothing |
| Interim per-job digest mismatch, found before the trigger | 1 | Slot FAULT until reloaded; other slots continue |
| Job deadline or controller fault | 3 | Every slot and the controller until the hang-reset package supplies a verified quiesce |

## Changes needed from other owners

- **Memory policy** (`drivers/npu/a733_npu_memory.c`): a per-buffer
  maintained extent, settable while the buffer's group has no job; mapping
  verification once at initialization, because the CPU tables are sealed; and
  contract text allowing CONSTANT and COMMAND bytes of an idle group to be
  rewritten when they are cleaned before its next publication. Without the
  extent the cache budget fails, and the runtime does not merge.
- **Primary agent:** a call that toggles EL0 and EL1 write permission on 2 MiB
  blocks of the NPU reservation, with TLB invalidation and a mapping
  generation. COMMIT makes the read-only region read-only and BEGIN restores
  write access, which removes the per-job digest and table scan
  (section 1.4). Also A76 admission for client affinity.
- **Hang-reset package:** a verified quiesce and reset, making controller
  faults class 2.
- **Conformance package:** the pool-pattern comparison and the numerical
  conformance of new operators.

## Left out

- **Isolation between uploaded models.** Per-job page-directory switching needs
  an NPU MMU flush sequence that is unqualified. With one device address space,
  an uploaded model can read every slot and write every write window, but
  cannot reach memory outside the NPU reservation or write a read-only region.
  Upload is therefore a build option; other images accept catalogue identities
  only, through the same loader.
- **Software (CPU) layers and compilation on the device.** Loads refuse them;
  they need CPU kernels and a mid-graph buffer hand-off.
- **Weaker MMIO barriers,** which need the board qualification in
  [docs/shared-helpers.md](../shared-helpers.md).
- **A board image.** A board-derived image would construct a second resource
  manager (supervisor-apps-10 in [docs/audit/README.md](../audit/README.md)),
  so `apps/npu-session/` builds for emulation only.

Board evidence that would confirm the design:

- two models loaded alternately into one slot each match their SDK outputs, so
  the NPU keeps no stale state across a reload;
- every job completes by interrupt, with zero fallback-poll completions;
- a descriptor that addresses a guard page raises an NPU MMU fault;
- the start order on the board matches the host model;
- back-to-back runs without a pool clear match the reference outputs.
