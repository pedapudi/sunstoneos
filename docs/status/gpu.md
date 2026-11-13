# GPU

**Status: implemented.**

The PowerVR firmware lifecycle, queues, compute jobs, local memory and recovery paths have software evidence. Recognition on the GPU is slower than on the CPU alone.

**Software evidence:** host, emulator, source.

**Board-qualified scope:** Firmware lifecycle, finite queue workloads and one
reference recognition input, including exact FP32 sums; four changed-input
generic local-memory GEMMs pass on port 3-1. A zero-element vector shader
completes 20,064 jobs per measured input on port 3-3 (submission median 3.833 us
on the admitted ring). General GPU coverage remains unqualified.

## Capabilities

| Capability | Status | Key figure | Evidence and design |
| --- | --- | --- | --- |
| Firmware lifecycle, finite queue workloads, recovery paths | qualified on the board | Normal retirement on ports 3-1 and 3-3 | [gpu.jsonl](../evidence/gpu.jsonl); [gpu-compute-queue.md](../designs/gpu-compute-queue.md) |
| Reference recognition input with exact FP32 sums; four changed-input local-memory GEMMs | qualified on the board | Port 3-1 | [gpu.jsonl](../evidence/gpu.jsonl); [gpu-compute-local-memory.md](../designs/gpu-compute-local-memory.md) |
| Caller-side submission and interrupt completion | qualified on the board | 20,064 jobs per input; submission median 3.833 us; round trip about 104 us; budgets unmet | [gpu-fast-path.md](../designs/gpu-fast-path.md) |
| OS owner notifications and precise deadlines | one native lifecycle session passes | Ten owner cycles and one peer job; modeled quiet owner: 1,000 to zero wakes/s; physical counters span the whole lifecycle | [Owner waits](../designs/gpu-owner-events.md), [owner evidence](../evidence/gpu-owner-events.jsonl) |
| Recognition on the GPU | qualified on the board | 327.476 ms against 135.474 ms CPU-only, 33.5 percent of the linear work on the GPU | `gpu-recognition-vs-cpu-only` |
| Recognition placement, hook and bounded replies | host-tested | Modeled 0.818 s against the 0.4435 s goal | [gpu-recognition.md](../designs/gpu-recognition.md); [gpu-recognition-wait.md](../designs/gpu-recognition-wait.md); [recognition-dispatch.md](../designs/recognition-dispatch.md) |
| Composition with CPU clocks, eight workers, NPU and resident card | host-tested | No hardware run | [gpu-cpu-clock.md](../designs/gpu-cpu-clock.md); [gpu-eight-workers.md](../designs/gpu-eight-workers.md); [os-gpu-npu.md](../designs/os-gpu-npu.md); [resident-gpu.md](../designs/resident-gpu.md) |
| Operating-point change (600 and 1008 MHz) | host-tested | Three firmware commands per transition | [gpu-performance.md](../designs/gpu-performance.md) |
| GPU-master traffic counters | host-tested | No board result | [gpu-nsi-traffic.md](../designs/gpu-nsi-traffic.md) |
| CPU and GPU split of each linear call | open | Predicted saving 3.0 percent (2.0 percent with dispatch cost) against a 5 percent threshold; CPU-only stays selected | `gpu-overlap-prediction` |
| Sustained concurrent workloads, stuck-device recovery, thermal throttling, critical-temperature protection, graphics | open | None | None |

## Open work

1. Correct the queue-add submission expectation to schema `sunstone-board-expect-u32-v1` (12 output words and bounds), check it without USB, and request one new queued entry. Keep the failed entry as it is.
2. Run the built recognition image with the same model, input and placement as the 154-job baseline, then compare recognition time against the pinned baseline.
3. Calibrate the GPU-master traffic counters (`SUNSTONE_GPU_NSI_PMU`; host-checked, no board result) before reporting measured bytes.
4. Revisit the CPU and GPU split only if a shader reaches about 0.3 times the CPU rate per multiply-accumulate or the dispatch interval falls below 20 us.
5. Select the fastest qualified shader; the shared byte-split screen is rejected (`gpu-shader-byte-split-screen`).
6. Explain the 104 us dispatch interval and the 118 us notification tail before changing timing budgets.
7. Qualify sustained concurrent workloads, arbitrary stuck-device recovery, thermal throttling, critical-temperature protection and graphics.
8. Qualify sustained notification-driven lifecycle, no-work idle rates and WFI residency. Preserve the registered GPU stop and returning failure proofs.

## Evidence

Board results are records in [gpu.jsonl](../evidence/gpu.jsonl); add one with `tools/evidence.py add`. Earlier prose evidence and the built images with their README paths are in [the historical log](../history/status-gpu-20261101.md#built-images-approvals-and-shared-files). Design records for the GPU designs are in `gpu-recognition.jsonl`, `gpu-recognition-wait.jsonl`, `gpu-compound-followup.jsonl`, `gpu-cpu-clock.jsonl`, `gpu-eight-workers.jsonl`, `gpu-exact-sum-caller.jsonl`, `gpu-nsi-traffic.jsonl`, `gpu-performance.jsonl`, `resident-gpu.jsonl`, `os-gpu-npu.jsonl` and `recognition-dispatch.jsonl` under `docs/evidence/`.

<!-- evidence-table:begin -->
40 records in `docs/evidence/gpu.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `gpu-overlap-prediction` |  |  | `` | not-run | failed | gpu_rate 2.61 GMAC/s, cpu_rate_eight_cores 30.4 GMAC/s, rate_ratio 0.086 ratio, saving_no_overhead 4.08 ms, saving_no_overhead_share 3 %, saving_with_dispatch 2.78 ms, saving_with_dispatch_share 2 %, dispatch_interval 153 µs, threshold 5 %, revisit_rate_ratio 0.3 ratio, revisit_dispatch_interval 20 µs |
| `gpu-recognition-vs-cpu-only` |  |  | `` | returned | failed | recognition_gpu 327.476 ms, recognition_cpu_only 135.474 ms, gpu_linear_work_share 33.5 %, gpu_device_total 216.716 ms |
| `gpu-shader-byte-split-screen` |  |  | `` | not-run | failed | smallest_program 8368 B, program_limit 8128 B |
| `1793556885965480682-30338083` | 2026-11-01 | 3-3 | `c5c0dfd02b54` | no-return | failed | run_total 424.006 s |
| `1793558145641638619-38d15f35` | 2026-11-01 | 3-3 | `e120e5bb3a75` | returned | passed | run_total 69.53 s |
| `1793558247336076757-d91d1dbf` | 2026-11-01 | 3-3 | `aa310a26b5d7` | watchdog-reset | failed | run_total 65.286 s |
| `1793558556213820434-b15a959e` | 2026-11-01 | 3-3 | `d1885fa0d707` | returned | passed | run_total 64.464 s |
| `1793558596353360282-28c39811` | 2026-11-01 | 3-1 | `c6bc5b41d24c` | returned | passed | run_total 69.992 s |
| `1793558639545392070-81ba2729` | 2026-11-01 | 3-3 | `002aa7b4e94f` | returned | passed | run_total 64.651 s |
| `1793558732769026225-99c2d8da` | 2026-11-01 | 3-3 | `3f8392981f3c` | returned | passed | run_total 65 s |
| `1793558878520344931-0ec4f97b` | 2026-11-01 | 3-3 | `85f15e792e21` | watchdog-reset | failed | run_total 70.379 s |
| `1793559451655627386-914e388f` | 2026-11-01 | 3-3 | `793dab0a88f4` | watchdog-reset | failed | run_total 68.848 s, stop_phase 8 phase, stop_step 15 step, timeout_error 3 code |
| `1793560094069288734-8923c447` | 2026-11-01 | 3-3 | `727ce4bbf6f6` | returned | passed | run_total 66.774 s |
| `1793560272415337471-7f3b4f28` | 2026-11-01 | 3-3 | `9bfc97a7a416` | returned | passed | run_total 72.002 s |
| `1793560443073044909-2795e0f8` | 2026-11-01 | 3-3 | `1352dfc74887` | returned | passed | run_total 69.458 s |
| `1793560622000131271-53954059` | 2026-11-01 | 3-3 | `3a69b7709998` | returned | passed | run_total 70.126 s, gpu_jobs_completed 186 jobs |
| `1793560651607761780-3cb221f5` | 2026-11-01 | 3-1 | `1d169eb93ab4` | returned | passed | run_total 69.506 s, compute 444.437 ms, aggregate_kick_us 339.188 ms, jobs 154 jobs |
| `1793561022387122671-06be5388` | 2026-11-01 | 3-1 | `935573354591` | returned | passed | run_total 69.612 s, compute 448.492 ms, aggregate_kick_us 342.515 ms |
| `1793562362886636744-4b4fa9c2` | 2026-11-01 | 3-1 | `5fffc2831d89` | returned | passed | run_total 69.666 s, compute 444.063 ms |
| `1793562523846617254-42844119` | 2026-11-01 | 3-1 | `571bbf31e597` | returned | passed | run_total 70.92 s, compute 453.758 ms |
| `1793562798861818897-0827fa55` | 2026-11-01 | 3-1 | `db694df7c5ce` | returned | passed | run_total 69.932 s, compute 708.073 ms |
| `1793563069525024856-fae93123` | 2026-11-01 | 3-1 | `167ed18e056b` | returned | passed | run_total 69.568 s, jobs 154 jobs, compute 339.535 ms |
| `1793563416684239128-a996b815` | 2026-11-01 | 3-1 | `777d2b4b55aa` | returned | passed | run_total 69.995 s, compute 702.618 ms |
| `1793563721079634350-e908cac3` | 2026-11-01 | 3-1 | `8fbac6ef3883` | returned | passed | run_total 69.678 s, jobs 148 jobs, compute 333.483 ms |
| `1793563824693939001-7c9def1d` | 2026-11-01 | 3-3 | `ffa80959528c` | returned | passed | run_total 64.769 s |
| `1793563967717509980-ac170b9a` | 2026-11-01 | 3-3 | `ffa80959528c` | returned | passed | run_total 68.247 s, rtt_p50 104.458 µs, rtt_p99 121.083 µs, rtt_maximum 175.958 µs, submit_wall_p50 13.458 µs, submit_wall_p99 24 µs, submit_wall_maximum 75.5 µs, pipeline 9506.38 jobs/s |
| `1793563985616621662-f052bac6` | 2026-11-01 | 3-1 | `58305265a414` | returned | passed | run_total 72.232 s, compute 333.315 ms |
| `1793564152591260158-7b442d20` | 2026-11-01 | 3-1 | `62bd94efdb4f` | returned | passed | run_total 71.022 s, compute 327.617 ms |
| `1793564171325300569-9135052d` | 2026-11-01 | 3-3 | `ffa80959528c` | returned | passed | run_total 68.05 s, rtt_p50 104.417 µs, rtt_p99 120.792 µs, rtt_maximum 335.583 µs, submit_wall_p50 13.25 µs, submit_wall_p99 22.708 µs, submit_wall_maximum 76.5 µs, pipeline 9516.75 jobs/s |
| `1793564244209939257-9848a75a` | 2026-11-01 | 3-1 | `62bd94efdb4f` | returned | passed | run_total 70.818 s, compute 327.476 ms |
| `1793564362816985826-5f5fd2b5` | 2026-11-01 | 3-1 | `62bd94efdb4f` | returned | passed | run_total 69.506 s, compute 327.169 ms |
| `1793565561444800193-61cd0ed0` | 2026-11-01 | 3-1 | `6883e3180638` | returned | passed | run_total 70.14 s |
| `1793565751525688109-fee8241b` | 2026-11-01 | 3-1 | `4cdba9f5c009` | returned | passed | run_total 69.715 s, compute 497.222 ms, device 380.47 ms |
| `1793566402339564949-f2aad6a6` | 2026-11-01 | 3-1 | `dac4426c9f15` | returned | passed | run_total 17.016 s |
| `1793566602125877874-e604614d` | 2026-11-01 | 3-1 | `a816d8e295b4` | returned | passed | run_total 17.083 s |
| `1793567014678511403-498b089f` | 2026-11-01 | 3-1 | `77703b3cff35` | returned | passed | run_total 70.28 s |
| `1793567279428783460-552e5684` | 2026-11-01 | 3-3 | `9cb215e5134a` | returned | passed | run_total 64.686 s |
| `1793567368102223375-02dbc4e4` | 2026-11-01 | 3-3 | `9cb215e5134a` | returned | passed | run_total 66.926 s, rtt_p50 103.792 µs, rtt_p99 115.833 µs, rtt_maximum 247.958 µs, submit_wall_p50 3.833 µs, submit_wall_p99 11.25 µs, submit_wall_maximum 51.875 µs, pipeline 9539.78 jobs/s, irq_to_notification_return_p50 2.292 µs, irq_to_notification_return_p99 8.167 µs, irq_to_notification_return_maximum 118 µs, irq_to_wait_return_p50 3.417 µs, irq_to_wait_return_p99 11.333 µs, irq_to_wait_return_maximum 119.208 µs, irq_samples 10000 samples |
| `1793567400575391633-770ecbf2` | 2026-11-01 | 3-1 | `8cc4af2ddbc0` | returned | passed | run_total 70.548 s, compute 501.761 ms, device 384.985 ms |
| `1793567482542195083-8703e806` | 2026-11-01 | 3-3 | `93bd14285f91` | result-mismatch | failed | run_total 64.689 s |
<!-- evidence-table:end -->
