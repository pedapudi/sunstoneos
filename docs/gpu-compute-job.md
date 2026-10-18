# Fixed numerical GPU compute job

The native job producer builds a complete numerical compute submission for the
A733 GPU. Source-byte comparisons and host completion models pass. Execution on
physical hardware remains unqualified. The interface is
[pvr_compute_job.h](../include/pvr_compute_job.h).

The fixed shader reads two unsigned 32-bit input words, `seed` and `factor`.
For each lane `i` from 0 through 31, it writes:

```text
output[i] = (seed + (i + 1) * factor) XOR (0x9e3779b9 + (i << 16))
```

All arithmetic wraps modulo 2^32. One workgroup contains 32 lanes. The shader
source is [pvr_compute_probe.cl](../shaders/pvr_compute_probe.cl). The pinned
Mesa compiler produces 280 instruction bytes and reports six temporary registers.
The native implementation embeds these executable bytes and preserves the
compiler metadata; it does not require a runtime compiler for this workload.

The Unified Shading Cluster (USC) executes the arithmetic. The Programmable Data
Sequencer (PDS) supplies input/output addresses and starts the USC program.
The Compute Data Master (CDM) control stream dispatches the workgroup, executes a
PDS fence kernel, and terminates. Its 60-byte encoding occupies the start of a
private 4096-byte page, keeping termination away from the page's final 64 bytes.

| Private allocation | Bytes | Purpose |
| --- | ---: | --- |
| USC program | 320 | Instructions, required prefetch space, and padding |
| Work PDS program | 128 | Address constants and dispatch code |
| Fence PDS program | 64 | Completion barrier and shared-partition release |
| CDM page | 4096 | Control stream and fetch guard |
| Input | 64 | Seed, factor, and padding |
| Output | 256 | 128 result bytes between two 64-byte guards |
| Firmware timeline | 64 | A 32-bit completion sequence and private padding |
| Border-color table | 262144 | Dedicated zeroed table for the command register |

These eight allocations total 267,136 bytes. The shader does not sample textures;
the zeroed border table does not implement general sampler border-color semantics.
All GPU addresses require 64-byte alignment, and the CDM page requires 4096-byte
alignment. CPU byte spans may be unaligned. Typed configuration, image, and state
records require their natural C alignment.

The offline builder validates the selected hardware revision, proven single-core
lease, heap-relative address encodings, firmware addresses, and all supplied
CPU/GPU range conflicts before writing. Refusal preserves every output byte.
The existing [context programs](gpu-compute-programs.md),
[compute context](gpu-compute-context.md), and VM memory record remain separate
owned allocations. Their known spans must appear in the program configuration.
The client ring must have the same size as its supplied backing and be at least
512 bytes for the compute command, timeline update, and transport reserve.

A required privileged admission callback proves the remaining conditions:
physical mappings and aliases, cache ownership, permissions, valid immutable
context/program records, matching heap bases, and exclusive ownership of a fresh
queue and timeline at initialization. The first job advances the timeline from
zero to one. After acquired completion, `pvr_compute_job_repeat` advances its
sequence without reformatting the live client ring. Sequence wrap is refused. The fixed command requests the published
single-core execution flag and prevents overlap with other work. A production
single-core admission proof and multicore context addressing remain required;
this workload does not reduce the final hardware-support scope.

Submission uses one serialized privileged worker and the firmware queues described
in [gpu-cccb.md](gpu-cccb.md). The job runner performs one bounded step per poll:

1. Publish the context, translations, programs, job memory, and initialized timeline.
2. Read the coherent timeline and require the previous sequence.
3. Submit the 96-byte compute command followed by an eight-byte timeline update.
4. Wait for firmware acceptance of the KICK command.
5. Wait for the current timeline sequence, then acquire and check the results and guards.

The publication callback must clean or otherwise publish dirty initialization
before any timeline invalidation. Fence reads use one aligned 32-bit transaction.
Output reads must acquire all 256 guarded bytes. The common monotonic clock uses
microseconds and covers publication through result verification. Clock reversal,
expired deadlines, callback failures, unexpected timeline values, and incorrect
results fault the operation and retain its resources. Callbacks must finish within
their own bounds; clock checks after return reject late success but cannot preempt
a callback. Firmware control and event processing must continue between polls.

The pinned board source initializes context records in `pvr_context_create` and
queue storage in `pvr_queue_create`. Its first `pvr_queue_run_job` carries the
firmware context address in `pvr_cccb_send_kccb_kick`. There is no separate context
attachment command between creation and that first KICK. Device-visible publication
must finish before that submission can expose the context to firmware.

The board's `pvr_queue_submit_job_to_cccb` appends a timeline UPDATE after the
compute command. Its `pvr_queue_signal_done_fences` observes that timeline to
signal job completion. The native runner additionally requires numerical and
guard agreement. KICK acceptance alone leaves the runner waiting for completion.
All faults preserve the context, queues, mappings, programs, and output storage.
The native service can reuse the same immutable workload after acquired
completion. It verifies the previous timeline, poisons the exclusively owned
output, and cleans and invalidates its page before another submission. Acquired
results are copied into private job state with the current sequence. Separate
context cleanup remains required before disposal or a new session. Power OFF does not establish that memory transactions have drained.

The source comparison uses Mesa revision
`86c7176d2dd0065555521bf8c61f208bf53c75d6` and board driver revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`. The existing
[compiler build instructions](gpu-compute-programs.md) describe the pinned Mesa
compiler and private dependency installation. The shader compilation uses
`mesa_clc` with the source include paths and OpenCL 2.0, followed by `pco_clc`.
Exact commands, tool hashes, SPIR-V, and generated USC artifacts reside in
`~/.cache/sunstoneos/evidence/gpu-compute-job-20261016/`.

The focused check consumes those retained shader artifacts and the previously
validated Mesa PDS reference object. It verifies source hashes before compiling
unchanged upstream PDS/control-stream encoders and the board firmware-stream
decoder into a host reference:

```sh
python3 checks/pvr-compute-job.py \
  --mesa-source ~/.cache/sunstoneos/gpu-producer-build/mesa \
  --mesa-build ~/.cache/sunstoneos/gpu-producer-build/build \
  --board-source ~/.cache/sunstoneos/sources/gpu/board-2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/imagination \
  --output ~/.cache/sunstoneos/evidence/gpu-compute-job-20261016
```

Each of two address/input layouts matches all 267,368 emitted bytes: the eight
allocations and 232-byte command/result image. The reference clears allocation
padding for deterministic comparison; those zeros are the native allocation
policy. The host model exercises actual native VM/context construction and client
and kernel command queues. It supplies simulated device writes for the timeline
and numerical output, so its result does not establish shader execution.

The sanitizer checks cover 67 atomic refusals, completion failures, retained
faults, and deadlines. Focused follow-ups cover ring-size/queue-alias refusal and
publication before initial timeline sampling. Their evidence reuses the full
byte comparison and unchanged component checks. The final freestanding AArch64
object has no libc dependency. Its largest individual static stack frame is
352 bytes in initialization; callback and nested call frames are additional.

Link `drivers/gpu/pvr_compute_job.c` with `pvr_compute_programs.c`, `pvr_cccb.c`,
and `pvr_ccb.c`. The caller provides publication, coherent reads, admission, and
clock callbacks, plus the existing native context/program/VM builders. The job
runner is independent of the service API and does not register itself with it.

The first physical qualification requires a committed bootable image with a
qualified independent watchdog and ROM return, powered and identified GPU,
running firmware service, proven single-core admission, native mappings, and
working publication/read callbacks. The coordinator should submit one bounded
job, record KICK status and timeline progression, and compare all results and
guards. A failure retains every lease until independently proven isolation and
cleanup. No physical-test request is queued until that image and its input hashes
exist. Board execution, multicore execution, shutdown integration, and performance
qualification remain outstanding.
