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

Status: **implemented** and host-modeled on the OS image side. The image with
the option does not link yet: it needs four functions from the GPU runtime
and one translation function from the page-table owner (listed under
[What other owners provide](#what-other-owners-provide)), and CMake refuses
the option until the two build constants below exist. No board has run any
part of it. The option is off by default, and with it off every OS image is
byte-identical to its build without this code apart from the 32-byte build
identity.

## Parts in this package

| Part | Source | Role |
| --- | --- | --- |
| Placement table | [placement.py](../apps/os/placement.py), generated `placement.h` | One row per model tensor: GPU channels and the largest job |
| Dispatcher | [dispatch.c](../apps/os/dispatch.c), [dispatch.h](../apps/os/dispatch.h) | Session state, the per-call lookup, the dispatch record and the failure path |
| Recognizer change | [recognition-dispatch.patch](../apps/os/recognition-dispatch.patch) | OPEN after the model hashes, failure path, recognition core |
| Mapping check | [application.patch](../apps/os/application.patch) | The two sum slots read-only at EL0 |
| Start entry and interrupt | [start.c](../apps/os/start.c), [irq.c](../apps/os/irq.c) | `gpu-linear` and INTID 95 on the recognition core |
| Build gate | [a733-os.cmake](../cmake/a733-os.cmake) | `SUNSTONE_A733_OS_GPU_LINEAR` and its prerequisites |
| Host checks | [os-image-dispatch.py](../checks/os-image-dispatch.py), [os-image-dispatch.c](../checks/os-image-dispatch.c), [os-image-link.py](../checks/os-image-link.py) | Table, dispatcher, supervision, link layout |

The GPU kernel, the hook inside mimus that splits a call into GPU jobs, the
submit message, the worker and its cache maintenance belong to the GPU kernel
design (`docs/designs/gpu-mimus-compute.md` on branch `audit/gpu-mimus-compute`).

## Memory and data flow

The loader places the base and overlay models before the image starts. The
recognizer hashes them once per boot and passes the recognition arena at
`0x46000000` to mimus. Codes stay in the arena's first 12 MiB. The GPU writes
sums into two 2 MiB slots at `0x46C00000` and `0x46E00000`, which the CPU maps
read-only at EL0 and therefore also at EL1. No weight, code or sum byte is
copied. The image has no model upload path, so the session lasts until the
boot ends.

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
- `job_rows`: `min(floor(2^26 / (K × n_gpu)), floor(2 MiB / (n_gpu × width)))`
  rounded down to a multiple of 32, where `width` is 4 bytes for K ≤ 512 and
  8 otherwise. It keeps a job within the kernel's 64 Mi multiply-accumulate
  (MAC) cap and one slot. When the bound rounds to zero, `n_gpu` is zero too.

`n_gpu` is the candidate with the shortest modeled call among N and N minus
each multiple of 16, the channel width of mimus's NEON kernel. The time model
replays one call on three timelines: the recognizer core, the second
inference worker and the GPU. The recognizer core cleans each job's codes and
submits it; both cores then compute half of the CPU channels; the recognizer
core then invalidates and reads out each job's sums in order as each job
completes. A call of more than 32 rows becomes two jobs so that the first
job's read-out overlaps the second job's GPU work; a 224-row subsampling
block stays one job. The seeds, which board measurements replace, are:

| Seed | Value |
| --- | --- |
| A55 NEON rate | 4,167,910,560 MACs in 14.894 s at 494 MHz, 0.280 G MAC/s per core |
| A76 NEON rate | Twice the A55's per clock |
| GPU rate | 32 lanes at 400 MHz, 1.75 instructions per MAC, over a grid padded to 32 rows by 16 channels |
| Read-out | 12 instructions per GPU output; 1 per clock on an A55, 2 on an A76 |
| Cache maintenance | 20 cycles per 64-byte line |
| Per-job overhead | 25 µs submit, 25 µs firmware, 25 µs wake; 10 µs to hand work to the second worker |

The recognition core pair is a build constant. Until the A76 cores are
admitted it is cores 2 and 3 at 494 MHz; with `A733_A76_ADMITTED` it is cores
6 and 7 at 2,002 MHz. The emitted tables for the bench models are:

| Layer, K → N | A55 pair: n_gpu, job_rows | A76 pair: n_gpu, job_rows |
| --- | --- | --- |
| Subsampling convolution, 1,584 → 176 | 176, 224 | 112, 352 |
| Subsampling output, 3,520 → 176 | 176, 96 | 112, 160 |
| Feed-forward first linear, 176 → 704 | 688, 544 | 448, 832 |
| Feed-forward second linear, 704 → 176 | 176, 512 | 112, 832 |
| Attention projections and pointwise convolution 2, 176 → 176 | 176, 2,144 | 112, 3,392 |
| Pointwise convolution 1, 176 → 352 | 352, 1,056 | 224, 1,696 |
| Output head, 176 → 1,025 | 993, 352 | 641, 576 |

On the A55 pair the model places 99.5% of the MACs on the GPU and gives a
linear stage of 0.814 s; on the A76 pair it places 63.6% and gives 0.407 s.
The design expected 0.79 s and 0.39 s, and on the A55 pair 48 CPU channels
for the feed-forward first linear and 64 for the output head. The
differences have two causes. With jobs split on 32-row boundaries, the second
job of a 222-row call has 126 rows, so the exposed read-out is 0.183 s
instead of the 0.155 s that equal halves give. The recognizer core is also one
of the two CPU workers, so CPU channels delay its read-out of the first job;
at 48 CPU channels the feed-forward first linear takes 7.36 ms against
6.16 ms at 16.

## The dispatcher

`os_dispatch_place(ordinal)` returns the tensor's row while the session is
open and a zero row otherwise. It compiles for AArch64 to two loads (the
active row count and the row) and one compare. The active count is the
table's length after a successful OPEN and zero before OPEN, after a refused
OPEN and after a GPU fault. The recognizer task runs the hook, so the count
and the dispatch record need no atomic operations or locks.

The dispatch record, `os_dispatch_record`, counts calls placed on the GPU and
jobs refused before device access, and holds whether OPEN succeeded or failed
and the GPU fault, if any. The recognizer is its only writer.

## Session lifecycle and failures

| Event | Action | Retained |
| --- | --- | --- |
| Before every model hash matches | No OPEN; every row reads as CPU-only | Nothing |
| OPEN refused or failed | CPU for the boot; OPEN is not retried | Nothing |
| Job refused before device access | The hook computes those rows on the CPU and counts the refusal; the session stays open | Nothing |
| RESET or LOST completion | `os_dispatch_failed` records it, clears the active count and takes the recognizer's failure path (`board_fail`, then suspension) before any CPU row of the failed job; the watchdog resets the board | Slots, model regions and code span until the reset |
| No reply | The recognizer records no progress, its 3 s service window expires, renewal stops and the watchdog resets the board within 4 s of the last renewal | Everything |

RESET ends the boot like LOST because the models and the code span are
trustworthy after a GPU fault only if the GPU's MMU enforces its read-only
mappings, which no board has shown. The recognizer has no timer of its own:
the resident lifetime's service window bounds every GPU wait, so CMake
refuses the option in the supervised lifetime.

## Mapping check, interrupt and start entry

With the option, `mimus_board_pins` checks the first 12 MiB of the arena as
writable, as before, and calls `os_dispatch_el0_read_only` on the 4 MiB of
slots. That function requires, for every 4 KiB page, an EL0 read translation
that succeeds as inner-shareable write-back memory at the same address and an
EL0 write translation that faults. The check makes 3,072 + 2 × 1,024 = 5,120
translations at application start, against 4,096 without the option.

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
- `A733_GPU_SLOTS_EL0_READ_ONLY` is set by the page-table owner once
  `0x46C00000`–`0x47000000` is mapped read-only at EL0 as two 2 MiB blocks.
  Without it, a stray EL0 store leaves a dirty line whose eviction can
  overwrite GPU sums;
- `A733_GPU_SLOTS_CLEAN_INVALIDATE` is set by the GPU kernel owner once the
  slots' completion uses `DC CIVAC`. `DC IVAC` is checked as a write and
  faults on a read-only slot; `DC CIVAC` needs read permission only;
- `SUNSTONE_A733_OS_MODEL_DIR` names the directory with `base.mimus` and
  `overlay.mimus`, from which the build generates the table.

```sh
cmake -S . -B BUILD ... -DSUNSTONE_MIMUS_BOARD_APPLICATION=ON -DSUNSTONE_A733_OS=ON \
  -DSUNSTONE_A733_OS_RESIDENT=ON -DSUNSTONE_A733_OS_GPU_LINEAR=ON \
  -DSUNSTONE_A733_OS_MODEL_DIR=<models> [-DSUNSTONE_A733_OS_AUDIO=ON]
```

The recognizer patch applies after the microphone or headset capture patch,
so the option combines with either capture source.

## What other owners provide

- **GPU runtime:** `a733_gpu_mimus_open` (OPEN: lease the model regions, the
  code span and the slots, and clean the model regions and slots once),
  `a733_gpu_mimus_worker`, `a733_gpu_mimus_start(core)` and
  `a733_gpu_mimus_irq`, declared in [dispatch.h](../apps/os/dispatch.h).
- **GPU kernel design:** the hook in mimus, which calls `os_dispatch_place`,
  `os_dispatch_placed`, `os_dispatch_refused` and, on RESET or LOST before any
  CPU row, `os_dispatch_failed`; `rows = 0` refused; `DC CIVAC` completion on
  the slots with `A733_GPU_SLOTS_CLEAN_INVALIDATE`.
- **Page-table owner:** the slots mapped read-only at EL0 with
  `A733_GPU_SLOTS_EL0_READ_ONLY`, and `aarch64_device_translate_el0`
  (`AT S1E0R` and `AT S1E0W`) with the signature of
  `aarch64_device_translate`; A76 admission with `A733_A76_ADMITTED`.
- **mimus port:** the tensor ordinal in `TensorView`, defined as above; the
  224-row subsampling producer; the second inference worker on the second
  core of the pair.
- **Shared helpers:** `os_dispatch_el0_read_only` can move to
  `a733_mapping_check.h` as a general EL0 read-only rule.

## Host checks

`checks/os-image-dispatch.py` runs without hardware. It emits both tables
from the bench models and compares them and the modeled linear stage with
the recorded values. It builds `checks/os-image-dispatch.c` with
AddressSanitizer and UndefinedBehaviorSanitizer and runs the dispatcher over
the bench call list, comparing every output's exact sum with a CPU-only
reference, and over the longest accepted input, the 1 MiB WAV slot
(820 encoder frames). It covers the cases in the lifecycle table, the slot
mapping rule with one writable, unreadable or Device page among 1,024, the
order of OPEN after the hashes in both recognizer patch chains, and the
lookup's AArch64 instructions. With `--configure-from` and a configured OS
build, it also runs CMake for the gate's refusals and for configurations
with both constants, with and without the microphone.

```sh
PYTHONDONTWRITEBYTECODE=1 ~/.cache/sunstoneos/venv/bin/python checks/os-image-dispatch.py \
  --configure-from <configured OS build> --output <new directory>
```

## Board evidence that would confirm it

- The GPU build recognizes the bench recording with text and CRCs equal to
  the host reference, and the dispatch record shows 204 placed calls and no
  refusal.
- An EL0 store into the slots faults, and the check in `mimus_board_pins`
  passes.
- INTID 95 arrives on the recognition core for every job.
- An injected hang ends in RESET, `board_fail` and a watchdog reset, and an
  injected LOST does the same; after each, the loader's model hashes match. A
  one-hour resident run never stops watchdog renewal.
- CPU and GPU rates per shape at each admitted clock replace the table's
  seeds.
