# Timing the full ShuffleNetV2 graph on the NPU, and the fixed cost of a job

Status: **implemented in the interrupt-driven finite image; not run on a
board.** The finite image runs the published ShuffleNetV2 graph at 492 and
852 MHz, completes each job on interrupt 97 and times it with the NPU's cycle
counters and the system counter. The second part analyses the fixed 23 µs
from publication to interrupt that the one-operation convolution measured
(`board-session-20261025-9/port-3-3`) and proposes reductions; none is
implemented here.

## The model and the image

The largest model whose reference output the repository pins is the
published ShuffleNetV2 graph: catalogue entry `shufflenet-v2-published` of
`tools/npu-models.json`, a 964,224-byte compiled graph of 125 NN operations
with a 150,528-byte UINT8 input and the SDK's 2-byte output `[0,255]`
([complete graph](../npu-complete-nn.md)). The repository holds the three
files' sizes and SHA-256 hashes; the bytes are in
`~/.cache/sunstoneos/evidence/npu-complete-graph-proposal-20261019/sdk-reference`.
The next largest, the relative-shift graph, has 3 operations.

The finite returning image runs it; the OS NPU profile is not needed. The
retained single-core image already executes this graph, but it takes its input
over USB. The finite image embeds the input and the reference instead, and
`apps/npu-finite/build.py --interrupt --shufflenet` selects the graph:

- `cmake/a733-npu-single-core.cmake` and `include/a733_device_memory_layout.h`
  admit the large NN-pool profile with the finite return as well as with the
  retained runtime, never both.
- `apps/npu-finite/link.ld` places the fixed 4 MiB device pool at
  `0x41600000..0x41a00000`, the pool the large graph's buffers require
  (`include/npu_convolution_layout.h`). The returned clean range grows to
  `0x41200000..0x41a00000`, 131,072 cache lines, inside the 18 MiB that the
  host saves and restores.
- The retained image's USB staging keeps its fixed addresses under its own
  linker symbol, `__npu_graph_staging`
  (`apps/npu-convolution-single-core/link.ld`).

The finite client, the service, the interrupt path, the startup and the host
are unchanged. The host files are byte-identical to those of the reviewed
interrupt host (`board-bundles-20261025-13`); the host fixtures now also cover
a reference shorter than 100 bytes.

## What each job records

Besides `NPU_TIME_US` and `NPU_JOB_COUNTERS`
([interrupt completion](npu-irq-completion.md)), the image logs
`NPU_JOB_PHASES`: per job, the system counter at the client's SUBMIT send, at
the entry of the job's publication and just after the doorbell write
(`+0x3A4`) that starts the job (`drivers/npu/npu_convolution.c`). With the
existing records each job divides into:

| Phase | From | To | Work |
| --- | --- | --- | --- |
| Submit | SUBMIT send | publication entry | Clear the 1,093,632-byte pool, copy the 150,528-byte input, queue hand-off |
| Publish | publication entry | counter start | Hash the coefficients, descriptors and job command; clean and invalidate 19,442 cache lines |
| Pre-trigger | counter start | doorbell | Seven NPU register accesses (below) |
| NPU | doorbell | interrupt delivery | The graph, then interrupt delivery |
| Completion | delivery | completion record | Worker wake and six NPU register reads |

## Predictions for ShuffleNetV2

The NPU's feature record gives 8 NN cores, 512 KiB of VIP SRAM and a 16-byte
AXI bus (`vip2/inc/vip_feature_database.h:161-183`). Decoding the 125 NN
descriptors with the field layout of the Mesa etnaviv driver
(`etnaviv_ml_nn.c:31-140`) gives about 0.59 G multiply-accumulates. That is
four times ShuffleNetV2's nominal 146 M, because the compiled graph runs the
first convolution at 224 × 224 before subsampling and the last 1 × 1
convolution at 14 × 14. It is an estimate of the engine's work, not a
measurement.

A job's time at NPU clock f is modelled as T(f) = C / f + M: C cycles of
clock-bound work and M of clock-independent memory and latency time. The
one-operation convolution measured C ≈ 0 and M ≈ 4.3 µs. For ShuffleNetV2:

| Quantity | Central prediction | Range |
| --- | --- | --- |
| C, at about 590 effective MACs per cycle | 1.0 M cycles | 0.4–2.5 M |
| M, DRAM traffic of 0.9 MB of coefficients and the pool, and per-operation latency | 0.8 ms | 0.3–2 ms |
| Doorbell to interrupt, 492 MHz | 2.8 ms | 1.5–6 ms |
| Doorbell to interrupt, 852 MHz | 2.0 ms | 1.0–5 ms |
| Time ratio 852/492 | 0.72 | 0.6–0.9; 0.58 if wholly clock bound |
| Active-cycle ratio 852/492, (C + M·852 MHz)/(C + M·492 MHz) | 1.21 | 1.0–1.5 |
| Idle share of the job's cycles | under 10% | the convolution: 90% |

The two starts determine C and M: C = (T₄₉₂ − T₈₅₂)/(1/492 MHz − 1/852 MHz)
and M = T₄₉₂ − C/492 MHz, which the set's `timing.py --compare` computes.
Both outputs match the reference `[0,255]` at both rates, and startup stays
at 4.0 ms. The submit and publish phases run on the CPU before the doorbell;
they are predicted at 1–5 ms and 5–30 ms per job, dominated by clearing the
pool and hashing about 0.92 MB with the software SHA-256.

Per-layer times need a completion per operation; the compiled command raises
one event after all 125 operations, so this run times the whole graph.

## The fixed 23 µs of a one-operation job

Set 13 timed four one-operation jobs. Two derived quantities explain the
23 µs from publication to interrupt:

| Job | Total-cycle window | System-counter window | Difference | Active | Publication to interrupt | Interrupt to completion |
| --- | --- | --- | --- | --- | --- | --- |
| 492 MHz, 1 | 45.35 µs | 40.08 µs | 5.27 µs | 4.37 µs | 23.54 µs | 16.54 µs |
| 492 MHz, 2 | 43.30 µs | 38.54 µs | 4.76 µs | 4.10 µs | 22.71 µs | 15.83 µs |
| 852 MHz, 1 | 44.88 µs | 39.50 µs | 5.38 µs | 4.38 µs | 23.88 µs | 15.62 µs |
| 852 MHz, 2 | 43.56 µs | 38.83 µs | 4.73 µs | 4.00 µs | 23.04 µs | 15.79 µs |

- **Cost of one register access.** The cycle counters are sampled one
  register read before the start timestamp and, after the completion
  timestamp, after one adapter-clock read and one more register read. The
  window difference, 5.0 µs on average, is therefore two register reads and
  one clock read: about 2.4 µs per NPU register access.
- **Publication to doorbell, about 17 µs.** After the start timestamp the
  controller reads three configuration registers, the idle register and the
  event register, writes `+0x654` and `+0x3A4`, and issues two `DSB SY`
  barriers (`a733_npu_controller.c:78-93`): seven accesses, 7 × 2.4 µs.
- **NPU work, 4.0–4.4 µs.** The active cycles are 2,000–2,200 at 492 MHz and
  3,400–3,700 at 852 MHz, the same 4.3 µs at both clocks, so memory latency
  bounds it: command fetch, descriptor, kernel and input reads and the output
  write, all from DRAM.
- **Completion to interrupt timestamp, about 2 µs**, the remainder:
  23.3 − 17 − 4.3 µs, covering interrupt delivery and exception entry.
- **Interrupt to completion record, 16 µs.** The worker reads three
  configuration registers, the event register, the idle register and the
  event register again (`a733_npu_controller.c:211-228`): 6 × 2.4 µs plus the
  wake.

Each access costs this much because the adapter translates the register
address with `AT` and decodes `PAR_EL1` on every access
(`platform/a733/npu_native.c:94-113`, `include/a733_mapping_check.h:62-77`),
and surrounds it with `DSB SY` (`include/a733_mmio.h:54-64`). The new
doorbell timestamp tests the 17 µs on the board.

## Proposed reductions

None is implemented. Each follows the vendor driver, whose default task mode
is the synchronous one (`vpmdENABLE_MULTIPLE_TASK` 3,
`vip2/inc/vip_lite_config.h:418-419`, `:860-863`).

1. **Plain register access.** The vendor reads and writes with `readl` and
   `writel` (`vip2/os/linux/vip_drv_os.c:383-430`). The adapter already
   checks the register window's mapping once at initialization
   (`npu_native.c:82-83`); keeping that check and the window guard, and
   dropping the per-access translation and the `DSB SY` pair, is predicted to
   bring an access to 0.1–0.3 µs, the cost the
   [performance architecture](../performance-architecture.md) gives for the
   access itself. Publication to doorbell: 17 → about 2 µs; publication to
   interrupt: 23 → about 8 µs; interrupt to completion: 16 → about 3.5 µs.
2. **Two-write trigger.** The vendor starts a job with the `+0x654` write, a
   memory barrier and the `+0x3A4` write, and reads nothing
   (`vip2/vip_drv_hardware.c:882-893`). Checking the configuration, idle and
   event registers at open, at completion and at close instead of before
   every trigger removes five reads: with item 1, publication to doorbell
   about 1 µs and publication to interrupt about 7 µs.
3. **One event read at completion.** The vendor's handler reads `+0x010` once
   (`vip_drv_device_driver.c:382`). Reading the event and idle registers once
   and the configuration at close: with item 1, interrupt to completion about
   2 µs.
4. **Immutable buffers prepared once.** The vendor keeps its wait-link
   command memory uncached (`vip_drv_hardware.c:666-670`) and flushes video
   memory through `vipdrv_mem_flush_cache` when it is written
   (`vip_lite_config.h:228-240`). The coefficients, descriptors and commands
   do not change after OPEN; hashing and cleaning them at OPEN only, and
   cleaning the input and invalidating the output and pool per job, removes
   the publish phase's hash and most of its 19,442 lines. The ShuffleNetV2
   run measures the phase this would shorten.

The memory-latency part of the one-operation job, 4.3 µs, does not change
with the NPU clock and none of these items addresses it.

## Checks

`~/.cache/sunstoneos/evidence/npu-full-model-1025/full-model-checks.sh`
builds the ShuffleNetV2 image at 492 and 852 MHz and runs the finite
return, SRAM routing, adapter, quiescence, interrupt client, interrupt
routing, task profile, reference-clock and interrupt host checks on it. The
convolution model checks the order of the new timestamps, and the interrupt
client check checks the logged `NPU_JOB_PHASES` block.

## Limits

- The predictions of C and M rest on an estimated MAC count and an assumed
  engine throughput; the two starts measure both.
- The 2.4 µs per access is inferred from the counter windows; the doorbell
  timestamp measures the pre-trigger phase directly.
- The ShuffleNetV2 input saturates the output; the comparison shows equal
  bytes, not classification accuracy.
