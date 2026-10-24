# Placing mimus linear layers on the CPU or the GPU

The [integrated OS image](os-image.md) runs mimus speech recognition at EL0 on
the CPU. Its linear layers multiply int16 activation codes by int8 weight
codes and sum the products exactly; on the bench recording they take
14.894 s of 19.476 s on one Cortex-A55 at 494 MHz ([mimus.md](mimus.md)).
The GPU option of the image moves those exact sums, for some or all output
channels of each layer, to the GPU. mimus's own CPU code still converts and
scales every sum, so recognized text stays bit-identical to the host
reference. The [design](designs/recognition-dispatch.md) states the budgets;
[the measurements](designs/recognition-dispatch-measurements.md) compare
the result with them.

Status: **implemented** and host-modeled. The image with the option
configures, links and passes its link check; the GPU
runtime and its session component are described in
[gpu-mimus-service.md](gpu-mimus-service.md) and measured in
[the GPU recognition measurements](designs/gpu-recognition-measurements.md).
No call reaches the GPU until the image build's pinned mimus engine
declares the exact-sum hook; with an engine that lacks it the recognizer
never opens the session. The design's
["Changes needed in the mimus engine"](designs/gpu-recognition.md#changes-needed-in-the-mimus-engine)
gives the engine commit that provides the hook. No board has run any part
of it. The
option is off by default. With it off, the default OS images differ from
their earlier builds only by the inference worker on core 3 (about 1.6 KiB
of code and a 64 KiB stack) and the build identity.

## Parts in this package

| Part | Source | Role |
| --- | --- | --- |
| Placement table | [placement.py](../apps/os/placement.py), generated `placement.h` | One row per model tensor: GPU channels and the largest job |
| Dispatcher | [dispatch.c](../apps/os/dispatch.c), [dispatch.h](../apps/os/dispatch.h) | Session state, the per-call lookup, the dispatch record and the failure path |
| Recognizer change | [recognition-dispatch.patch](../apps/os/recognition-dispatch.patch) | OPEN after the model hashes, failure path, recognition core, 12 MiB arena assertion |
| GPU runtime | [gpu.c](../apps/os/gpu.c), [gpu_hook.cc](../apps/os/gpu_hook.cc) | Start, OPEN, jobs and INTID 95 ([gpu-mimus-service.md](gpu-mimus-service.md)); the engine's exact-sum hook |
| Mapping check | [application.patch](../apps/os/application.patch) | The 4 MiB above the arena, which holds the GPU output region, read-only at EL0 |
| Start entry and interrupt | [start.c](../apps/os/start.c), [irq.c](../apps/os/irq.c) | `gpu-linear` and INTID 95 on the recognition core |
| Build gate | [a733-os.cmake](../cmake/a733-os.cmake) | `SUNSTONE_A733_OS_GPU_LINEAR` and its prerequisites |
| Host checks | [os-image-dispatch.py](../checks/os-image-dispatch.py), [os-image-dispatch.c](../checks/os-image-dispatch.c), [os-image-link.py](../checks/os-image-link.py) | Table, dispatcher, supervision, link layout |

The GPU kernel and its cache maintenance belong to the GPU kernel design
([gpu-mimus-compute.md](designs/gpu-mimus-compute.md)); the session in the GPU
service, the worker and the hook belong to the GPU recognition design
([gpu-recognition.md](designs/gpu-recognition.md)).

## Memory and data flow

The loader places the base and overlay models before the image starts. The
recognizer hashes them once per boot and passes the recognition arena at
`0x46000000` to mimus. mimus's arena, which holds the codes, is the first
12 MiB. The GPU writes into one 2 MiB output region at `0x46C00000`: a 64 KiB
tag area, one word per GPU lane, then the sums. The CPU maps
`0x46C00000`–`0x47000000` read-only at EL0 and therefore also at EL1; the
upper 2 MiB of that span is not leased, so a CPU store past the arena faults.
The GPU has one job in flight. No weight, code or sum byte is copied. The
image has no model upload path, so the session lasts until the boot ends.

## The placement table

`placement.py` reads the two model files and refuses them unless their
SHA-256 equals `apps/bluetooth/recognition-models.json`, the hashes that the
image embeds in `mimus_asset_sha256`. It lists the tensors in the order that
mimus's `Model::Load` builds them: base tensors in file order with overlay
replacements in place, then the overlay's new tensors. A tensor's index in
that list is its **ordinal**; the bench models have 586 tensors. The table
has one 4-byte row per ordinal:

- `n_gpu`: output channels `[0, n_gpu)` run on the GPU; the CPU computes
  `[n_gpu, N)`. Zero keeps the tensor on the CPU. A tensor gets zero when no
  bench call reaches it, when its weights are not int8 or its activations not
  int16, or when K is not a multiple of 4.
- `job_rows`: the GPU worker's own bound for the tensor, rounded down to a
  multiple of 32. The worker computes it at OPEN as
  `min(floor(2^26 / (K × n_gpu)), floor((2 MiB − 64 KiB) / (n_gpu × width)),
  32 × floor(512 / ceil(n_gpu / 16)))`, where `width` is 4 bytes for K ≤ 512
  and 8 otherwise: the kernel's 64 Mi multiply-accumulate (MAC) cap, the sum
  area, and the 512 workgroups of 32 rows by 16 channels whose tags fill the
  tag area. Every job the hook issues therefore passes the worker's
  `rows ≤ job_rows` check. When the bound is below 32, `n_gpu` is zero too.

`n_gpu` is the candidate with the shortest modeled call among N and N minus
each multiple of 16, the channel width of mimus's NEON kernel. The time model
replays one call on three timelines: the recognizer core, the second
inference worker and the GPU. A call is one job, or consecutive jobs of
`job_rows` rows and a last job with the rest; the next job is submitted only
after the previous one is read out, because it reuses the output region. Per
job, the recognizer core cleans the job's codes and its 128-byte job words and
submits it. In a channel split both cores compute half of the CPU channels
while the first job runs. After each completion the recognizer core
invalidates the job's tags and sums, and the two workers read the job out,
half of its rows each, as the engine splits them. The seeds, which board
measurements replace, are:

| Seed | Value |
| --- | --- |
| A55 NEON rate | 4,167,910,560 MACs in 14.894 s at 494 MHz, 0.280 G MAC/s per core |
| A76 NEON rate | Twice the A55's per clock |
| GPU rate | 32 lanes at 400 MHz, 3.39 instructions per MAC (217 instruction groups per 64 MACs, the built program), over a grid padded to 32 rows by 16 channels; `--load-cycles` adds a load-latency bound |
| Read-out | 3.0 instructions per GPU output, the engine's compiled read-out counted on the placed shapes ([measurements](designs/gpu-recognition-measurements.md#with-the-engines-hook)); 1 per clock on an A55, 2 on an A76 |
| Cache maintenance | 20 cycles per 64-byte line |
| Per-job overhead | 25 µs submit, 25 µs firmware, 25 µs wake; 10 µs to hand work to the second worker |

The recognition core pair is a build constant. Until the A76 cores are
admitted it is cores 2 and 3 at 494 MHz; with `A733_A76_ADMITTED` it is cores
6 and 7 at 2,002 MHz. The recognizer runs on the first core and the engine's
second inference worker (`apps/mimus/parallel_runtime.cc`) on the second,
in the GPU build and in every other OS build. The emitted tables for the
bench models are:

| Layer, K → N | A55 pair: n_gpu, job_rows | A76 pair: n_gpu, job_rows |
| --- | --- | --- |
| Subsampling convolution, 1,584 → 176 | 160, 256 | 80, 512 |
| Subsampling output, 3,520 → 176 | 160, 96 | 80, 224 |
| Feed-forward first linear, 176 → 704 | 624, 416 | 320, 800 |
| Feed-forward second linear, 704 → 176 | 160, 576 | 80, 1,184 |
| Attention projections and pointwise convolution 2, 176 → 176 | 160, 1,632 | 80, 3,264 |
| Pointwise convolution 1, 176 → 352 | 304, 832 | 160, 1,632 |
| Output head, 176 → 1,025 | 897, 256 | 449, 544 |

On the A55 pair the model places 90.2% of the MACs on the GPU and gives a
linear stage of 1.117 s, against the regression bound of 1.12 s. On the A76
pair it places 45.4% on the GPU and gives 0.535 s, against 0.54 s. These are
issue-bound figures; with 200-clock GPU loads the A76 pair models at 0.665 s.
Every bench call is one job, 204 in all. The generated header also lists the
179 placed tensors' model file, weight offset, stride and K for the
runtime's admission, and a bitmap of the placed ordinals. The recognizer
passes the bitmap to the engine's `SetExactSumOrdinals`, so the engine
offers the hook only those tensors' calls; every other call keeps the
engine's streamed row ring and its internal-arena codes.

## The dispatcher

`os_dispatch_place(ordinal)` returns the tensor's row while the session is
open and thermal throttling is clear, and a zero row otherwise. Its table
lookup retains two table loads and adds one thermal getter call, an ordinal
stack save/restore and one conditional test. The getter performs one acquire
load; it never sleeps. The active count is the
table's length after a successful OPEN and zero before OPEN, after a refused
OPEN and after a GPU fault. The recognizer task runs the hook, so the count
and the dispatch record need no atomic operations or locks.

The thermal gate applies at each layer's `Begin`. A layer already admitted
can finish its remaining batches through `Finish`. Returning to NORMAL
restores placement only while the session remains open; it cannot reopen
a faulted or incomplete session. Moving work to the CPU alone does not
establish lower temperature. The OS also delays recognition checkpoints
during THROTTLE and closes admission on CRITICAL, as described in
[the thermal service](os-image.md#thermal-service).

The dispatch record, `os_dispatch_record`, counts calls placed on the GPU and
jobs refused before device access, and holds whether OPEN succeeded or failed,
whether a job completed with a missing tag, and the GPU fault, if any. The
recognizer is its only writer.

## Session lifecycle and failures

Before every model hash matches there is no OPEN and every row reads as
CPU-only. A refused or failed OPEN keeps the CPU for the boot and is not
retried. A refused job runs on the CPU and the session stays open. A missing
tag or a sum that differs from the CPU's makes `os_dispatch_incomplete`
clear the active count at once, and the hook removes itself from the engine
(`SetExactSumHook(nullptr)`), so every later call runs on the engine's CPU
paths as it does without a hook. A lost
job makes `os_dispatch_failed` record it, clear the active count and take the
recognizer's failure path (`board_fail`, then suspension) before any CPU row
of the failed job; the watchdog resets the board. A reply that never comes
stops the recognizer's progress, and the watchdog resets the board within 4 s
of the last renewal. [gpu-mimus-service.md](gpu-mimus-service.md#failure-classes)
gives each failure's class and what stays retained.

RESET ends the boot like LOST because the models and the code span are
trustworthy after a GPU fault only if the GPU's MMU enforces its read-only
mappings, which no board has shown. The recognizer has no timer of its own:
the resident lifetime's service window bounds every GPU wait, so CMake
refuses the option in the supervised lifetime. After a GPU fault, or when the
recognizer's window expires while the session is being opened or places
calls, the supervisor's stop detail is `OS_STOP_DETAIL_GPU_LOSS`, and the next
boot reads it from the retained fault slot and keeps OPEN refused.

## Mapping check, interrupt and start entry

With the option, `mimus_board_pins` checks the first 12 MiB of the arena as
writable with `os_dispatch_writable` and calls `os_dispatch_el0_read_only` on
the 4 MiB from `0x46C00000`. The second requires an EL0 read translation that
succeeds as inner-shareable write-back memory at the same address and EL0 and
EL1 write translations that fault. Both check at the stride of the image's
tables, `a733_local_pool_stride`: 2 MiB in the OS image, whose tables map
image RAM with level-2 blocks only. The arena check
(`a733_mapping_check_stride`) translates one page per block, 6 translations;
the view check translates the first and the last 4 KiB page of each block
three times, 12 translations; 18 at application start, against 4,096 without
the option. The QEMU golden
comparison of `checks/device-memory-capacity.py` fails if any of these
entries becomes a page table.

The interrupt table gains INTID 95, the GPU's SPI 63, at priority `0xd8` on
the recognition core. The recognizer task is created with affinity to the
same core, and the GPU runtime creates its worker there, so a completion
needs no inter-processor interrupt. The image records the core in
`os_recognition_core`; the link check refuses an image whose INTID 95 route
names another core. The start list gains `gpu-linear`, which grants the GPU
worker EL1 and creates it before the scheduler starts.

## Build gate

`SUNSTONE_A733_OS_GPU_LINEAR` is off by default. With it on, configuration
stops unless:

- `SUNSTONE_A733_OS_RESIDENT` is on;
- `A733_GPU_OUTPUT_EL0_READ_ONLY` is not given: the branch sets it itself,
  which maps `0x46C00000`–`0x47000000` read-only at EL0 and EL1 as two 2 MiB
  blocks, so no CPU store can leave a dirty line whose eviction overwrites
  GPU sums or tags;
- `SUNSTONE_GPU_FIRMWARE` names the GPU firmware with the pinned SHA-256 of
  the accelerator image;
- `SUNSTONE_A733_OS_MODEL_DIR` names the directory with `base.mimus` and
  `overlay.mimus`, from which the build generates the table.

Until the board shows GPU cache invalidation at job start, GPU read-only
enforcement, cross-core code writes and fault-slot retention across a WDT0
reset, the hook compares every GPU sum with a CPU sum before the engine reads
it ([gpu-mimus-service.md](gpu-mimus-service.md#one-job)).

```sh
cmake -S . -B BUILD ... -DSUNSTONE_MIMUS_BOARD_APPLICATION=ON -DSUNSTONE_A733_OS=ON \
  -DSUNSTONE_A733_OS_RESIDENT=ON -DSUNSTONE_A733_OS_GPU_LINEAR=ON \
  -DSUNSTONE_GPU_FIRMWARE=<gpu.fw> -DSUNSTONE_A733_OS_MODEL_DIR=<models> [-DSUNSTONE_A733_OS_AUDIO=ON]
```

The recognizer patch applies after the microphone or headset capture patch,
so the option combines with either capture source.

## What other owners provide

- **mimus engine:** the tensor ordinal in `TensorView`, defined as above; the
  exact-sum hook as `checks/gpu-recognition-hook.h` declares it, with its
  block-sink and job rules; `SetExactSumOrdinals`; codes of hooked calls in
  the arena; the 224-row subsampling producer. The design's "Changes needed
  in the mimus engine" lists each change and the engine commit that provides
  them.
- **Page-table owner:** A76 admission with `A733_A76_ADMITTED`.
- **Shared helpers:** `os_dispatch_writable` and `os_dispatch_el0_read_only`
  can move to `a733_mapping_check.h` as general stride-based rules.

## Host checks

`checks/os-image-dispatch.py` runs without hardware. It emits both tables
from the bench models, compares them with the design's values, checks every
row's `job_rows` against the worker's formula, and fails when the modeled
linear stage exceeds the design's regression bound. It checks the
generated admission list against the model files, and compiles the
recognizer patch's arena assertion with a 12 MiB arena, which passes, and
with a 16 MiB arena or another base, which fail. It builds `checks/os-image-dispatch.c` with
AddressSanitizer and UndefinedBehaviorSanitizer and runs the dispatcher over
the bench call list, comparing every output's exact sum with a CPU-only
reference, and over the longest accepted input, the 1 MiB WAV slot
(820 encoder frames). It covers the cases in the lifecycle table, the
supervisor's GPU-loss detail, the mapping rules at both strides with one
writable, unreadable or Device page at each checked position, a replay of the
pool admission order for the GPU's pages, the order of OPEN after the hashes
in both recognizer patch chains, and the lookup's AArch64 instructions. With
`--configure-from` and a configured OS build, it also runs CMake for the
gate's refusals and for the resident build, with and without the
microphone. `checks/gpu-recognition-engine.py` and
`checks/pvr-mimus-service.py` cover the hook and the GPU session
([gpu-mimus-service.md](gpu-mimus-service.md)).

```sh
PYTHONDONTWRITEBYTECODE=1 ~/.cache/sunstoneos/venv/bin/python checks/os-image-dispatch.py \
  --configure-from <configured OS build> --output <new directory>
```

## Board evidence that would confirm it

- The GPU build recognizes the bench recording with text and CRCs equal to
  the host reference, and the dispatch record shows 204 placed calls and no
  refusal.
- An EL0 store into `0x46C00000`–`0x47000000` faults, and the check in
  `mimus_board_pins` passes.
- INTID 95 arrives on the recognition core for every job.
- An injected hang ends in LOST, `board_fail`, a watchdog reset and a
  CPU-only next boot; after it, the loader's model hashes match. This is an
  intentional watchdog expiry, which waits for a qualified reset destination
  and recovery. A one-hour resident run never stops watchdog renewal.
- CPU and GPU rates per shape at each admitted clock replace the table's
  seeds.
