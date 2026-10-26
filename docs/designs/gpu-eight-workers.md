# Eight inference workers with GPU recognition

Status: implemented and software-tested; physical execution remains unqualified. The first supported composition is
supervised recorded recognition with `SUNSTONE_MIMUS_WORKERS=8`, GPU linear
placement and admitted A76 cores. Existing CPU clock grants remain available.
Two workers remain the default. No throughput improvement is claimed.

## The pinned engine already joins GPU readout across eight workers

The source baseline is SunstoneOS `6a945b30` and the clean local mimus revision
`967747f10dcf038aadc2961b5990d540f6b76455`, named by
[`apps/mimus/source.json`](../../apps/mimus/source.json). The relevant engine
files are `firmware/components/asr/src/kernels.cc`, `exact_sum_readout.cc` and
`include/mimus/asr/parallel.h` within that checkout.

`HookFor` admits the exact-sum hook only on the task that installed it.
`SetExactSumHook` records that task's `platform::TaskIdentity()`. The recognizer
therefore owns every GPU Begin and Finish call; additional inference workers
never share the mutable call record in `apps/os/gpu_hook.cc`.

After activation producers join, Begin lends immutable codes to the GPU.
`HookedSums` computes the remaining CPU channels through `HookedBlocks`, calls
Finish on the owner, then passes the returned sums to synchronous
`ForRowRanges`. Its callback creates private readout arguments and writes
separate output rows. The readout arithmetic uses local values and the row's
output, including residual accumulation and an elementwise sink. Every worker
joins before the next Finish can reuse the GPU sum region. The non-elementwise
sink completion remains on the owner after the entire call.

The pinned row splitter already admits up to eight aligned ranges. Packed CPU
rows in hooked calls can use the same path. Small jobs use fewer workers;
stateful producers, LogMel scratch and unaligned packed-row repair retain
their established two-context limits. Asynchronous engine dispatch remains
disabled for eight workers. No engine change, new scratch allocator, GPU
queue or scheduler is needed.

The GPU job's existing two-second deadline starts at submission and includes
time spent computing CPU channels before Finish. Eight-worker dispatch must
not refresh it. The existing terminal path retains uncertain GPU ownership;
a worker that stops before acknowledgement retains the owner's borrowed
context and prevents another Finish or recognition generation.

## Fixed placement preserves service progress

`apps/mimus/parallel_runtime.cc` already places the owner on A76 core 6 and
worker 1 on core 7, both at priority 3. Workers 2 through 7 use A55 cores
0 through 5 at priority 2. The PMIC service remains at priority 3 on core 0;
USB and thermal services use priority 4. The GPU service remains at priority
4 on core 6, sharing that core with the priority-3 recognizer, and IRQ 95
retains its core-6 route. Neither the IRQ nor GPU reply ownership moves to an
inference worker.

The GPU recognition patch calls the count-aware adapter's init and
bind functions. Its task priority uses the existing `kMimusOwnerPriority`
constant so task creation and linked metadata have one source. Its checkpoint,
CPU-ready wait and terminal failure logic need no behavioral change.

The generated placement table still uses the historical A76-pair cost model.
Its channel split, row bounds, tensor identities and arithmetic remain valid
with eight CPU workers; it is not an eight-core optimum. Preserve that table
and its hash until physical measurements justify placement changes. Increasing
the worker count makes no throughput or deadline-success claim.

## Remove composition refusals while preserving exact admission

Admission is implemented in the eight-worker CMake and
`parallel_runtime.h` guards, `prepare.py:image_inference`, the final link
check, and `gpu_configuration`'s binding to the actual inference record.

Those checks permit the supervised eight-worker GPU composition.
Continue requiring A76 admission, recorded input and the existing exclusions
for capture, radio and headset. Keep resident eight-worker GPU refused: cold
GPU authority is outside the supervised entry contract. The separate
concurrent GPU/NPU design owns optional-device composition; this change alone
does not admit NPU or GPU performance commands.

Use the existing linked inference record: version 1, eight workers, owner 6,
mask 255, owner priority 3, A55 priority 2 and A76 priority 3. GPU metadata
continues to bind the same owner, IRQ, firmware, clock, voltage and placement.
No new capability version or session bytes are needed. The host must compare
both records with the exact ELF and require the existing reviewed-entry
inference record for eight workers. Preparation and frozen verification must
reject a missing record, a relabeled pair, wrong priorities or owner mismatch.
The existing 808-byte GPU and 904-byte GPU-plus-CPU contracts remain unchanged.

## Focused evidence and budgets

Reuse the accepted engine arithmetic and recognition parity, bounded GPU
caller/lifecycle evidence, and GPU-plus-CPU startup/provider checks. Their
anchors are `docs/designs/mimus-eight-workers.md`, `mimus-os-eight-workers.md`
and `gpu-cpu-clock.md`; the last image is
`~/.cache/sunstoneos/build/resume-gpu-cpu-clock-1026/sunstone-a733-os.elf`.
No full recognition, benchmark or hardware run belongs to this increment.

The new checks should cover only the previously untested combination:

- A focused FreeRTOS fixture links the pinned engine, eight-worker adapter and
  generated GPU recognition checkpoint. Use a modeled exact-sum producer with
  small synthetic linear inputs to check bit-exact int32 partial-channel and
  int64 residual readout, multiple jobs and a short tail. Exercise at least
  64 aligned rows so all eight workers participate. Verify Begin/Finish run
  on the owner and every readout is joined before the producer reuses sums.
- In that fixture, priority-3 A55 services and a priority-4 core-6 GPU-service
  stand-in must progress while inference is outstanding; count progress only
  inside that interval. Close board liveness
  before one readout acknowledgement; execute the generated terminal path
  and prove that neither owner return nor the next Finish occurs. The
  producer and services are models; this does not establish GPU completion.
- Build one actual supervised GPU-plus-CPU image with eight workers. Verify
  effective engine/adapter capacity, task placement, linked metadata, IRQ,
  unchanged firmware/placement, session size and all memory bounds. Measure
  against the accepted two-worker artifact. Preserve the existing 12 MiB
  inference arena and 4 MiB GPU view.
- Run selected host preparation/frozen-entry cases for the actual eight-worker
  image and its combined records, plus the accepted pair artifact for
  compatibility. Check missing A76 and resident GPU-eight refusals. Reuse
  unchanged device-exclusion and CPU-grant checks.

Six additional existing worker stacks cost 393,216 bytes; six private
queue/task slots cost 5,760 bytes. Count actual linked engine metadata,
compiler padding and any composition code separately. A fully populated
parallel dispatch adds six job messages and six matching completion messages
relative to a pair, using the existing queue synchronization. It adds no GPU
request, output copy or device operation. Sparse ranges retain the splitter's
smaller active count. The
native link must keep heap, pools, stacks, model inputs and GPU regions within
their existing reservations. No heterogeneous CPU speed ratio is assumed.

## Agreed implementation ownership

The GPU eight-worker owner holds `parallel_runtime.h`, the GPU recognizer
patch, focused readout/worker fixtures and the worker-count conditions in
CMake, `checks/os-image-link.py` and `prepare.py:image_inference`. The concurrent
GPU/NPU owner holds optional-device conditions, combined session and asset
admission, device-owner collisions and shared-provider lifecycle changes.
Both workers agreed this section-level split; shared files will be integrated
sequentially. Neither design changes the other's unreviewed condition.
Independent design review approved this scope before implementation.

## Recorded software results

Evidence is preserved under
`~/.cache/sunstoneos/evidence/resume-gpu-eight-workers-20261026/`.
The native GPU/CPU eight-worker ELF has SHA-256
`a20a0a3d03e9d8b822157eab68d384308b4c3cb7a6d0647a0c260aecf0481322`.
It binds the 904-byte supervised contract and the expected eight-worker
configuration. GPU firmware, placement, rate and voltage match the accepted
pair image.

`workers.json` records one focused FreeRTOS run using eight emulated A76 CPUs.
The actual pinned engine's int32 partial-channel and int64 residual outputs
match its CPU path for three jobs, including a short tail. The adapter reports
56 completed worker jobs, core mask 255 and no asynchronous jobs. The fixture
counts service progress only while all eight readout callbacks remain active.
Closing board liveness before core 0 acknowledges invokes the generated GPU
recognizer checkpoint's terminal path. The owner retains its stack context
and sum storage with only one Finish call issued. The emulated homogeneous
CPUs establish software ordering, not A55/A76 timing or GPU hardware behavior.

`host/result.json` records thirteen focused outcomes, including actual
preparation and frozen admission of read-only and TOP CPU grants. Missing or
pair inference records, wrong service priority and wrong owner refuse in both
fresh and frozen admission. Count, mask and priority relabeling and removal
of linked metadata refuse. `guards/result.json` records four CMake/header
refusals for missing A76 admission and resident GPU-eight. The accepted
GPU/CPU pair ELF passes current host admission without rebuilding it.

`layout.json` verifies the native heap, pools, worker slots/stacks and fixed
model/arena/GPU extents. The fixture has eight distinct 32 KiB exception stacks
and retains its 4 MiB heap below the existing RAM limit. Native worker stacks
occupy 458,752 bytes and slots occupy 6,720 bytes.

`cost.json` compares actual linked sections with the accepted GPU/CPU pair.
Executable bytes rise by 6,336 and mutable sections by 401,440; read-only
sections shrink by 16. The existing eight-worker engine instantiations add
6,612 executable bytes; removal of the unavailable asynchronous path reduces
adapter code by 340 bytes. Added worker stacks and slots account for 398,976
bytes, offset by nine unused asynchronous-state bytes. The engine's larger
worker-scratch descriptor array adds 96 bytes. Output-section alignment
accounts for the remaining 2,377 mutable bytes and 64 executable bytes.
The GPU caller and recognizer input-section sizes do not increase. These
static costs do not establish a throughput improvement.
