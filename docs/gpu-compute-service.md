# The GPU service owns one fixed numerical compute job

The optional native compute component connects the
[fixed numerical job](gpu-compute-job.md) to the FreeRTOS GPU service. Its
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
provenance, and single-core proof. Binding does not construct or install CPU
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
both compute-memory publication and the existing firmware health check.

COMPUTE rechecks admission, formats the private client control record, and drives
the fixed job through the same privileged worker. Its final publication callback
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

CLOSE returns BUSY while a compute operation is pending. After numerical
completion it can use the existing shutdown protocol and independently proven
isolation callback. Compute backing remains retained afterward, and OPEN refuses
to reuse that session. Explicit context cleanup, resource disposal, repeated jobs,
and multicore context state remain required work. Power OFF alone does not prove
that outstanding memory transactions have drained.

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
aliases of firmware memory, and misaligned service records.

The compiler and emulator execute the native translation and cache instructions.
The fixture supplies simulated firmware acceptance, timeline writes, and numerical
output. It does not execute USC instructions or establish physical cache
visibility. Existing shader-byte, VM-format, queue, and unchanged service-lifecycle
evidence is reused. Success/reset evidence is archived under `baseline/`; the
focused alignment and partial-publication follow-up has its own executable and
manifest. Source snapshots, commands, firmware hash, and ELF hashes identify each
executed artifact.

Physical qualification still requires a bootable integration image, qualified
independent recovery, native power/register/interrupt service binding, reserved
CPU/GPU mappings, and a proven single-core mode with matching heap bases. The
coordinator can then submit one fixed job and record the timeline, numerical
results, guards, and retained shutdown state. No board request is queued until
that image and its inputs exist. General applications, repeated submissions,
multicore execution, and performance qualification remain outstanding.
