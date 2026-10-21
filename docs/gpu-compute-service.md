# The GPU service repeats one fixed numerical compute workload

The optional native compute component connects the
[fixed numerical workload](gpu-compute-job.md) to the FreeRTOS GPU service. Its
implementation publishes workload memory, maps firmware records before startup,
submits the job from the service worker, and acquires results after the completion
timeline advances. A two-core emulator check passes with actual native memory
operations and modeled firmware/GPU writes. Physical execution remains unqualified.

The configuration and internal worker API are in
[pvr_compute_service.h](../include/pvr_compute_service.h). The application uses
the existing copied service requests and replies. `COMPUTE` accepts only the
fixed workload from the owner of a READY session. Acceptance starts an operation;
STATUS reports its phase, component error, and job error. Completion requires the
firmware timeline and all numerical results and guards to match.

A platform first reserves memory and uses the existing native VM adapter,
context builder, program producer, and job producer to construct private offline
records. It then binds the compute component before initializing the GPU service.
Binding validates native CPU translations and physical-page exclusions. A required
privileged admission callback supplies the remaining ownership, heap, table/map
provenance, and offline MC1 profile selection. The mandatory `compute_mode`
callback validates the native post-power core-count observation before firmware
startup and again before COMPUTE. Binding does not construct or install CPU
mappings, enable GPU cores, publish memory, or access GPU registers.

The allocation descriptors distinguish two device address spaces:

| Storage | Address space | CPU mapping | Publication |
| --- | --- | --- | --- |
| Context, register state, client control/ring, VM record, timeline | MIPS firmware heap | Shareable Normal Non-cacheable | Ordered before startup |
| Context programs/state and fixed-job programs/data | Workload GPU VM | Inner-shareable Normal write-back | Clean and invalidate one page per poll |
| Workload translation tables | GPU page-table physical addresses | Inner-shareable Normal write-back | Existing native VM adapter cleans one page per poll |

The first five program exclusions hold firmware virtual addresses. Those records
need no mapping in the workload VM. The timeline also belongs only to firmware
memory. The native adapter separately compares declared physical pages, so an
address-space distinction cannot hide a shared physical page.

Each logical allocation owns separate page-rounded backing. All native CPU and
GPU data bases are 4096-byte aligned; the offline producers retain their broader
alignment support. Firmware addresses are page aligned within the 16 MiB heap.
The job timeline descriptor must identify the same allocation as the sixth
firmware object. All page lists and descriptors remain immutable and pinned.

The workload mapping list covers every program/data backing page once, uses
4096-byte pages, and contains no extra mappings. Programs, control streams,
input, and border-color data are GPU read-only. Context state and numerical
output permit GPU writes. Physical aliases among these allocations, firmware
resources, and VM tables are refused. Metadata and complete backing spans must
also be disjoint on the CPU. The admission callback excludes hidden aliases and
other platform reservations, including resources behind opaque callback contexts.

The component accepts at most 128 workload mapping entries and 256 table pages.
Six additional firmware objects join the 19 service objects, within the existing
64-object MIPS preparation limit. The service builds all firmware translations
before publishing the firmware page tables. No live table replacement or
translation-cache invalidation is involved. A valid PREPARED native VM adapter
retains its initialized, naturally aligned physical-page list and exact table
build; the privileged admission callback proves that the supplied mapping list
belongs to those bytes.

OPEN covers publication and firmware startup under one absolute deadline. The
service validates admission again, then publishes one data page per advance and
passes table publication to the existing native VM adapter. Firmware power and
startup begin only after that publication completes. READY therefore requires
compute-memory publication, current core-mode admission, and the existing
firmware health check.

COMPUTE rechecks admission, formats the private client control record for the
first submission, and drives the fixed job through the same privileged worker. Its final publication callback
orders the already-published memory before initial timeline acquisition. Client
queue and timeline accesses use uncached memory; timeline reads are single aligned
32-bit transactions. Result acquisition invalidates the exclusively owned output
cache lines before reading all 256 guarded bytes.

The worker continues firmware control and free-list processing between job steps.
A common microsecond clock rejects backward or late observations across those
controllers. Unknown or malformed events fail existing control processing.
Every decoded firmware reset faults this single-context qualification session,
including a reset after numerical completion. Other events retain their original
handlers. Failure closes admission and preserves context, mappings, program data,
queues, output, diagnostics, and resource ownership.

The READY owner can submit the same immutable workload again after acquired
completion. Each submission advances a 32-bit firmware timeline and the existing
client-ring serial. The live client control record is never reformatted. The
service verifies the preceding timeline before poisoning the exclusive output
page and cleaning and invalidating it. It refuses sequence wrap. The command,
input words, programs, mappings and context retain their original identities.

`COMPUTE_OUTPUT` returns the 32 acquired words and their submission sequence in
a copied reply. The worker copies observed output, rather than the expected
reference. Starting another job makes output unavailable until its completion.
`COMPUTE_CANCEL` acknowledges closed admission by faulting the session and
retaining all backing and power ownership. It does not establish that GPU
execution stopped. Later requests cannot submit work or retrieve output. Reset
and other faults also revoke output access, including after completion.

CLOSE returns BUSY while a compute operation or copied input staging is pending.
For a compute-only service, CLOSE rechecks native admission and acquires the exact
completed timeline, or zero for an unused context. It initializes an unused
client ring through the existing formatter, then submits firmware common-context
cleanup before the existing power and isolation protocol. STATUS exposes the
cleanup and cleaned compute phases. The same CLOSE deadline covers every step.

The cleanup command follows pinned Linux `pvr_fw_structure_cleanup`
(`pvr_fw.c:1096–1166`): KCCB command 106, flags 0, type 0 (`FWCOMMONCONTEXT`),
and the admitted context firmware address. Its epoch, serial and return slot
must match the exact submitted receipt. Only status 1 authorizes shutdown;
firmware BUSY, poll failure, a missing receipt or an expired deadline retains
ownership and prevents the healthy power-off path. BUSY is a terminal refusal
for this bounded operation; no retry is implied.

Cleanup acknowledges firmware context disposal. Compute backing, VM mappings
and pool allocations remain retained after ordinary CLOSE. Explicit disposal
and reconstruction require the [standalone lifecycle profile](gpu-render.md#standalone-sessions-can-dispose-and-reconstruct-child-resources).
Native graphics owners use the [ordered render cleanup](gpu-render.md#healthy-close-cleans-firmware-objects-before-stopping)
for render contexts, render-target datasets and free lists. An unexpected multicore observation also requires per-core
context state and command support; the documented A733 GPU is MC1. Power OFF
alone does not prove that outstanding memory transactions have drained.


With `config.compute == NULL`, existing requests use their previous lifecycle.
The optional field appends no automatic compute operation. Service configuration,
state, and reply layouts have changed, so callers must rebuild against the header.
The component uses no additional task, allocator, filesystem, or interrupt owner.

Link the service with `pvr_compute_service.c`, the existing fixed-job and program
producers, CCCB transport, native VM-memory adapter, and AArch64 device-memory
helpers. The caller still constructs the context and workload tables with their
existing builders. These dependencies must be available even when the optional
configuration is disabled; the service references the component's entry points.

The focused check builds in an isolated directory and runs selected cases:

```sh
python3 checks/pvr-compute-service.py \
  --output ~/.cache/sunstoneos/evidence/gpu-compute-service-20261016 \
  --freertos ~/.cache/sunstoneos/sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2 \
  --tinyusb ~/.cache/sunstoneos/sources/tinyusb-d668b0abb7219b9ce324fa88d46604bb9e869973 \
  --firmware ~/.cache/sunstoneos/sources/gpu/imagination-firmware/powervr/rogue_36.56.104.183_v1.fw \
  --lld ~/.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld \
  --qemu ~/.cache/sunstoneos/host/usr/bin/qemu-system-aarch64
```

The successful-job case covers owner-only admission, pending CLOSE and duplicate
COMPUTE refusal, unrelated event forwarding, native result acquisition, shutdown,
and retained backing. The reset case faults after KICK and observes no subsequent
backend callbacks. The partial-publication case changes a retained physical-page
descriptor after OPEN; native translation validation faults before GPU startup.
Initialization checks also refuse incorrect physical correspondence, workload
aliases of firmware memory, misaligned service records, and a missing mode callback.
The focused mode cases cover refusal after power, refusal before client-ring
formatting, and successful fresh admission before startup and COMPUTE.
Run those cases with `--cases 4 5 6`; prior completion/reset evidence remains valid.

The compiler and emulator execute the native translation and cache instructions.
The fixture supplies simulated firmware acceptance, timeline writes, and numerical
output. It does not execute USC instructions or establish physical cache
visibility. Existing shader-byte, VM-format, queue, and unchanged service-lifecycle
evidence is reused. Success/reset evidence is archived under `baseline/`; the
focused alignment and partial-publication follow-up has its own executable and
manifest. Source snapshots, commands, firmware hash, and ELF hashes identify each
executed artifact.

The cleanup checks use `--cases 15 16 17 18 19 20 21`. They cover an unused
context, a completed job with copied output, firmware BUSY and poll-failure
returns, a completion written to the wrong KCCB slot, a future timeline and
revoked admission. Both successful cases require acknowledged cleanup before
both power commands, retained VM mappings and refused reopen. Failure cases
retain the owner and prevent power shutdown. The unused case also checks the
initialized client-ring wrap mask. All seven pass under
`~/.cache/sunstoneos/evidence/gpu-compute-cleanup-20261020/`.
Firmware returns and GPU output are modeled; this does not qualify physical
cleanup, cache visibility, memory draining or allocation reclamation.

The [native application](gpu-application.md) builds a compute mode with the
actual reserved-memory and power providers. Physical qualification requires qualified
independent recovery, native power/register/interrupt service binding, reserved
CPU/GPU mappings, physical confirmation of the observed MC1 mode, and matching
heap bases. The coordinator must record advancing timelines, numerical results, guards and
retained ownership. Arbitrary programs, general graphics, mimus acceleration,
unexpected multicore execution modes and physical qualification remain outstanding.

The repeated-workload checks use `--cases 7 8 9 10 11`. They cover two
submissions with fresh output poisoning, copied values and owner-only access;
a stale second timeline; a future timeline; cancellation after KICK; and reset
after copied completion. The successful case also refuses sequence wrap.
Evidence is under `~/.cache/sunstoneos/evidence/gpu-completion-20261018/`.
Case 7 passed before a timeout-fixture correction and remains preserved under
`composition-before-timeout-fixture-fix/`. Cases 8 through 11 pass under
`composition/`. The stale timeline faults at the component deadline before the
job-local timeout check. These checks model firmware and numerical GPU writes;
they do not execute the USC program on a physical GPU.

The [trusted program application](gpu-compiled-programs.md#runtime-input-in-one-session)
also exposes bounded same-session input replacement. Its scoped copied staging
slot carries a client/ticket-bound request to this worker; generic queue messages
do not contain input pages. Worker-side completion and native resource admission
precede input overwrite and EL1 cache publication. The program and geometry stay
immutable. Exact-sequence copied reads reject output from another submission.
Focused runtime-input case 13 covers the changed path and healthy shutdown;
unchanged compiler, dispatch and physical checks are reused.

## Compute-only disposal and reopening

The supervised runtime-input application supports explicit disposal with
`SUNSTONE_GPU_LIFECYCLE=ON`. Build the existing
`sunstone-a733-gpu-compute-single-core` target with compute, program, runtime-input,
single-core and supervised options enabled; runtime-draw and runtime-texture
remain disabled. The manifest workload is `trusted-compiled-program-lifecycle`.
The optional compute wire version is 6; retained version 2 remains compatible.

After acknowledged healthy CLOSE, the native worker removes six compute MIPS
objects and its GPU VM mappings, invalidates complete WB child extents, then
releases the payload, NC firmware/context and VM-table parent leases. OPEN
reconstructs all three with fresh generations and the next queue epoch. Global
firmware leases and shared providers stay pinned. The complete authenticated
8,288-byte package, including original input, remains outside these children.
The new context executes that sealed input and copies sequence-one output before
publishing READY.

During pending OPEN, the application reports the previous queue epoch and
sequence. Completed output, the new identity and mailbox availability publish
under one bounded single-core critical section; USB snapshots the corresponding
fields under the same exclusion. STATUS requests carry expected sequence zero
and still validate the returned sequence and queue identity. Mutations and reads
retain exact epoch binding.

Focused evidence is in
`~/.cache/sunstoneos/evidence/gpu-compute-lifecycle-20261020/`. The native owner
check executes three-parent release, old-generation refusal, reconstruction and
new VM/program publication. CPU admission/cache/translation, the accepted full
healthy shutdown and provider construction are modeled. A separate application
check executes the actual mailbox/client and pinned TinyUSB FIFO/callback path
with modeled GPU/service completion and previously retained result bytes.

The application check copies 65 words as pages of 32, 32 and 1 word before and
after CLOSE, DISPOSE and OPEN. It holds the reopened compute completion until
STATUS copies the previous pending identity, then checks an immediate READ after
READY. Reply sizes are 176, 176 and 52 bytes; control replies are 48 bytes. These
selected replies end with a short packet at either supported USB speed and need
no terminal ZLP. Host replay and four new partial-page refusal cases validate
page index, tail count, acquired extent and sequence without repeating arithmetic.
Physical GPU execution, healthy reclamation, SMP timing and mixed-device disposal
remain unqualified.
