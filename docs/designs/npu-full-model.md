# Timing the full ShuffleNetV2 graph on the NPU, and the fixed cost of a job

Status: **qualified on the board** for the finite image running the published ShuffleNetV2 graph at 492 and 852 MHz (doorbell to interrupt 3.69 ms and 2.91 ms; [records](../evidence/npu-full-model.jsonl)). The output check is weak evidence of arithmetic, because the output saturates (see Limits). The job-cost reductions that this analysis motivated are in [the job-overhead design](npu-job-overhead.md).

## The model and the image

The largest model whose reference output the repository pins is the published ShuffleNetV2 graph: catalogue entry `shufflenet-v2-published` of `tools/npu-models.json`, a 964,224-byte compiled graph of 125 NN operations with a 150,528-byte UINT8 input and the SDK's 2-byte output `[0,255]` ([complete graph](../npu-complete-nn.md)). The repository holds the sizes and SHA-256 hashes of its three files; the bytes are in `~/.cache/sunstoneos/evidence/npu-complete-graph-proposal-20261019/sdk-reference`. The next largest graph, the relative-shift graph, has 3 operations.

The finite returning image runs it; the OS NPU profile is not needed. The retained single-core image also executes this graph but takes its input over USB. The finite image embeds the input and the reference, and `apps/npu-finite/build.py --interrupt --shufflenet` selects the graph:

- `cmake/a733-npu-single-core.cmake` and `include/a733_device_memory_layout.h` admit the large NN-pool profile with the finite return as well as with the retained runtime, never both.
- `apps/npu-finite/link.ld` places the fixed 4 MiB device pool at `0x41600000..0x41a00000`, the pool the large graph's buffers require (`include/npu_convolution_layout.h`). The returned clean range is `0x41200000..0x41a00000`, 131,072 cache lines, inside the 18 MiB that the host saves and restores.
- The retained image's USB staging keeps its fixed addresses under its own linker symbol, `__npu_graph_staging` (`apps/npu-convolution-single-core/link.ld`).

The finite client, service, interrupt path, startup and host are those of the reviewed interrupt host (`board-bundles-20261025-13`); the host fixtures also cover a reference shorter than 100 bytes. The run is described in [the runbook](../board-runbooks/npu-full-model.md).

## What each job records

Besides `NPU_TIME_US` and `NPU_JOB_COUNTERS` ([interrupt completion](npu-irq-completion.md)), the image logs `NPU_JOB_PHASES`: per job, the system counter at the client's SUBMIT send, at the entry of the job's publication and just after the doorbell write (`+0x3A4`) that starts the job (`drivers/npu/npu_convolution.c`). With the existing records each job divides into:

| Phase | From | To | Work |
| --- | --- | --- | --- |
| Submit | SUBMIT send | publication entry | Clear the 1,093,632-byte pool, copy the 150,528-byte input, queue hand-off |
| Publish | publication entry | counter start | Hash the coefficients, descriptors and job command; clean and invalidate 19,442 cache lines (the changes in the job-overhead design remove the hash and most of the lines) |
| Pre-trigger | counter start | doorbell | NPU register accesses |
| NPU | doorbell | interrupt delivery | The graph, then interrupt delivery |
| Completion | delivery | completion record | Worker wake and NPU register reads |

The compiled command raises one event after all 125 operations, so the run times the whole graph. Per-layer times would need a completion per operation.

## Job time model

A job's time at NPU clock f is T(f) = C / f + M: C cycles of clock-bound work and M of clock-independent memory and latency time. The two starts (492 and 852 MHz) determine them:

- C = (T492 - T852) / (1/492 MHz - 1/852 MHz)
- M = T492 - C / 492 MHz

`timing.py --compare` of the run set computes both. The one-operation convolution has C about 0 and M about 4.3 µs. For ShuffleNetV2 the board gives C about 902,000 cycles and M about 1.85 ms (`npu-full-model-shufflenet-prediction`), which exceeds the 0.8 ms predicted from 0.9 MB of DRAM traffic and per-operation latency. Decoding the 125 NN descriptors gives about 0.59 G multiply-accumulates, four times ShuffleNetV2's nominal 146 M, because the compiled graph runs the first convolution at 224 x 224 before subsampling and the last 1 x 1 convolution at 14 x 14.

## The fixed 23 µs of a one-operation job

Four one-operation jobs show a publication-to-interrupt time of 22.7-23.9 µs at both clocks (`npu-full-model-fixed-job-cost`):

- **Register access, about 1.6 µs each.** The counter windows span three register reads and one clock read (5.0 µs on average). Each access includes the bus access, a translation of the register address with `AT` with a `PAR_EL1` decode, and `DSB SY` around it.
- **Publication to doorbell, about 11 µs.** Seven accesses: three configuration reads, the idle and event reads, and the `+0x654` and `+0x3A4` writes, with two `DSB SY` barriers.
- **NPU work, 4.0-4.4 µs.** The same 4.3 µs at both clocks, so memory latency bounds it: command fetch, descriptor, kernel and input reads and the output write, all from DRAM.
- **About 8 µs not attributed**: interrupt delivery, exception entry and any NPU time the counters record as idle.
- **Interrupt to completion record, about 16 µs.** Six register accesses (about 10 µs) and about 6 µs of wake, GIC mask and clock reads.

The memory-latency part does not change with the NPU clock. The records do not separate bus latency from translation and barrier cost; the job-overhead design removes the translation and barriers.

## Limits

- The ShuffleNetV2 output `[0,255]` is saturated: both bytes sit at the ends of the UINT8 range, so many wrong computations would produce it, and a match shows equal bytes, not classification accuracy. No unsaturated intermediate or larger reference exists for this graph. Open work: compare an unsaturated layer output, taken from the DRAM pool before the host restores DRAM, with the SDK's `vpm_run --layer_dump` on the host simulator.
- The 1.6 µs per access is inferred from the counter windows.
- The checks (`~/.cache/sunstoneos/evidence/npu-full-model-1025/full-model-checks.sh`) build the image at both rates and run the finite return, SRAM routing, adapter, quiescence, interrupt client, interrupt routing, task profile, reference-clock and interrupt host checks on it.
