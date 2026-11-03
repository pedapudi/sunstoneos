# Eight inference workers with GPU recognition

Status: **host-tested**. Supervised recorded recognition with `SUNSTONE_MIMUS_WORKERS=8`, GPU linear placement and admitted A76 cores is implemented in the mimus checkout (`sunstoneos/apps/mimus/parallel_runtime.cc`, `sunstoneos/apps/os/prepare.py`) and the OS link check, and passes focused software checks. Physical execution is **open**. Two workers remain the default, CPU clock grants remain available, and no throughput improvement is claimed.

## The engine joins GPU readout across eight workers

The engine is the mimus revision named by [apps/mimus/source.json](../../apps/mimus/source.json); the relevant files are `firmware/components/asr/src/kernels.cc`, `exact_sum_readout.cc` and `include/mimus/asr/parallel.h`.

`HookFor` admits the exact-sum hook only on the task that installed it. `SetExactSumHook` records that task's `platform::TaskIdentity()`, so the recognizer owns every GPU Begin and Finish call and the other workers never share the mutable call record in `gpu_hook.cc`.

After the activation producers join, Begin lends immutable codes to the GPU. `HookedSums` computes the remaining CPU channels through `HookedBlocks`, calls Finish on the owner, then passes the returned sums to the synchronous `ForRowRanges`. Its callback creates private readout arguments and writes separate output rows, with residual accumulation and an elementwise sink on local values. Every worker joins before the next Finish can reuse the GPU sum region, and a non-elementwise sink completes on the owner after the whole call. The row splitter admits up to eight aligned ranges, small jobs use fewer workers, and stateful producers, LogMel scratch and unaligned packed-row repair keep their two-context limits. Asynchronous engine dispatch is disabled for eight workers.

The GPU job's two-second deadline ([gpu-recognition-wait.md](gpu-recognition-wait.md)) starts at submission and includes the time spent computing CPU channels before Finish; eight-worker dispatch never refreshes it. The terminal path retains uncertain GPU ownership, and a worker that stops before acknowledgement retains the owner's borrowed context and prevents another Finish or recognition generation.

## Fixed placement

`parallel_runtime.cc` places the owner on A76 core 6 and worker 1 on core 7, both at priority 3. Workers 2 through 7 run on A55 cores 0 through 5 at priority 2. The PMIC service runs at priority 3 on core 0, and USB and thermal at priority 4. The GPU service runs at priority 4 on core 6 with the priority-3 recognizer, and IRQ 95 keeps its core-6 route. Neither the IRQ nor GPU reply ownership moves to an inference worker.

The recognition patch calls the count-aware adapter's init and bind functions, and its task priority uses `kMimusOwnerPriority` so that task creation and linked metadata share one source. The generated placement table keeps the A76-pair cost model: its channel split, row bounds, tensor identities and arithmetic remain valid with eight CPU workers, but it is not an eight-core optimum, and its hash is preserved until physical measurements justify a change.

## Admission

Admission is implemented in the eight-worker CMake and `parallel_runtime.h` guards, `prepare.py:image_inference`, the final link check and `gpu_configuration`'s binding to the linked inference record. It permits the supervised eight-worker GPU composition with A76 admission and recorded input, and keeps the exclusions for capture, radio and headset. Resident eight-worker GPU is refused, because cold GPU authority is outside the supervised entry contract.

The linked inference record has version 1, eight workers, owner 6, mask 255, owner priority 3, A55 priority 2 and A76 priority 3. GPU metadata binds the same owner, IRQ, firmware, clock, voltage and placement. The host compares both records with the ELF and requires the reviewed-entry inference record for eight workers; preparation and frozen verification reject a missing record, a relabeled pair, wrong priorities or an owner mismatch. The 808-byte GPU and 904-byte GPU plus CPU contracts are unchanged.

## Cost

Six additional worker stacks cost 393,216 bytes and six private queue and task slots 5,760 bytes. A fully populated parallel dispatch adds six job messages and six completion messages relative to a pair, using the existing queue synchronization, and no GPU request, output copy or device operation. Sparse ranges keep the splitter's smaller active count. Heap, pools, stacks, model inputs and GPU regions stay within their reservations, and no heterogeneous CPU speed ratio is assumed.

## Qualified figures

| Figure | Value | Record |
| --- | --- | --- |
| Worker jobs completed in the focused fixture, core mask | 56, 255 | `gpu-eight-workers-software` |
| Host admission outcomes; CMake and header refusals | 13; 4 | `gpu-eight-workers-software` |
| Worker stacks; slots (native image) | 458,752 B; 6,720 B | `gpu-eight-workers-software` |
| Executable growth against the two-worker GPU plus CPU image | 6,336 B; mutable 401,440 B | `gpu-eight-workers-software` |

Records: [gpu-eight-workers.jsonl](../evidence/gpu-eight-workers.jsonl). The fixture runs eight emulated homogeneous CPUs, so it establishes software ordering, not A55 and A76 timing or GPU behavior.

## Limitations and open work

- The concurrent GPU and NPU composition owns optional-device conditions, combined session and asset admission and shared-provider lifecycle ([os-gpu-npu.md](os-gpu-npu.md)); this design admits neither NPU nor GPU performance commands.
- Open: measure eight-worker throughput, service latency and recovery on a board, and revise the placement table from those measurements.
