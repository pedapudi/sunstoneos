# NPU

**Status: implemented.**

Native model decoding, mappings, shared-provider startup, IRQ97 service, runtime clients and thermal policy have software evidence. Finite scheduled startup and inference have scoped physical results. Repeated-transition reliability remains unqualified. Opt-in internal clock gating of the idle NPU, its power owner and a benchmark with a baseline, a control and a gating image are implemented with host evidence only; no gating image has run on a board. A quiesced finite image that fails now returns to the boot ROM with a failure record instead of ending at the watchdog. Hardware values these paths assert (GIC priority views, interrupt routes, CPU identity) are in [the A733 hardware facts](../hardware/a733-facts.md).

**Software evidence:** `checks/npu-idle.py`, `checks/npu-queue-handoff.py` (production owner, queue and idle code with modeled devices and nine regression mutations), `checks/npu-idle-producer.py`, `checks/npu-idle-report.py`, `checks/npu-failure-return.py`, `checks/npu-latency.py`.

**Board-qualified scope:** A finite interrupt diagnostic produces matching
ShuffleNetV2 outputs at 852 MHz/800 mV. Startup takes 4.0 ms and inference takes
2.91 ms doorbell-to-interrupt, about 3.3 ms end to end. Provider-free
entry, disabled-IRQ installation/restoration and real provider/task construction
before scheduler start return normally with saved RAM restored. These
checkpoints perform no inference. The watchdog-admitting finite startup,
one-inference and two-job/two-model runtime images also qualify on 3-3 at
852 MHz/800 mV, with matching copied outputs, CLOSE, local stop and normal
ROM return. The resident command queue runs 1,000 matched jobs at 10.25 µs
p50 and 26,925 jobs/s, and 1,000 fence-chained two-stage jobs, on 3-3. The combined CPU and NPU image runs four NPU jobs through the OS NPU service while eight CPU workers run (overlap mask 15, matching outputs and CPU digests), then CLOSE, local stop and normal return, on 3-3 at 852 MHz/800 mV. The baseline idle benchmark image (no power owner) returns normally with 61,231 matching jobs: five windows of 12,000 jobs at about 26,650 jobs/s, p99 11 µs, maximum 17 µs, and cold jobs after 9, 11 and 100 ms gaps at 10.6 µs p50. The finite idle benchmark qualifies internal clock gating on 3-3 at 852 MHz/800 mV: five rounds of baseline, control and gating pass every limit of the design (throughput, p99, maximum latency, CPU time), every resume of the 11 ms and 100 ms gaps is within 100 µs (largest 25 µs) and the 9 ms gaps never gate.

## Open work

1. Call the idle power owner from the OS NPU service (`npu_runtime_queue_power_start` is not called by the operating system image), then repeat the idle benchmark there. Measure energy and the break-even idle time: the PMIC provider exposes no DCDC2 current or power readback. Qualify shared SRAM access while the clocks are gated and recovery from a fault while suspended.
2. Qualify the combined image repeatedly and across both models: the active-cancellation image, and a 20-cycle start, inference, CLOSE and local-stop image.
3. Qualify the EL0 queue and prepared joined streams: review and finish the saved generic sequence registration and returning diagnostic (`npu-el0-next-1101`), add the public-ring predicate to the power owner, then run the control and the EL0 images.
4. Add exact per-job deadline interrupts through the scheduler's deadline facility (`work/scheduler-rt-1101`); do not add a device timer.
5. Measure energy: the PMIC provider exposes no DCDC2 current or power readback, so idle savings and break-even time have no evidence without an external meter.
6. Qualify repeated start and stop, shared SRAM access while the clocks are gated, and recovery from a fault while suspended.
7. Run the `restart-*` cases of `checks/npu-irq-route.py` on a restart image; the other cases pass.

## Evidence

Board results with a queue entry are records in `docs/evidence/npu.jsonl`, added with `tools/evidence.py add`. Results cited by path, and earlier prose evidence, are in [the historical log](../history/status-npu-20261101.md).

<!-- evidence-table:begin -->
32 records in `docs/evidence/npu.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793558381244643310-6a29a71d` | 2026-11-01 | 3-3 | `dd826f5ff757` | returned | passed | run_total 110.813 s, matching_jobs 2, matching_completions 4 |
| `1793558909256910454-ab3a76b4` | 2026-11-01 | 3-3 | `8ede7a8341fa` | returned | passed | run_total 110.785 s, matching_jobs 3, matching_completions 4 |
| `1793559233825594424-693cc430` | 2026-11-01 | 3-3 | `a0dd10679c00` | returned | passed | run_total 110.751 s, matching_jobs 2, matching_completions 3 |
| `1793559627833816772-2003f2e7` | 2026-11-01 | 3-3 | `f9cc623ab7de` | returned | passed | run_total 110.927 s, matching_jobs 200, matching_completions 201 |
| `1793563838443635411-92c1fdae` | 2026-11-01 | 3-3 | `862114046425` | returned | passed | run_total 111.246 s |
| `1793564135238449408-5dbdd6ad` | 2026-11-01 | 3-3 | `2a6411ba3fcc` | returned | passed | run_total 112.541 s |
| `1793565141195481474-2e135a8c` | 2026-11-01 | 3-3 | `ae8dc27e6c16` | returned | passed | run_total 111.135 s |
| `1793572859635159128-187e5901` | 2026-11-01 | 3-3 | `7f18c978f34c` | watchdog-reset | failed | run_total 51.235 s |
| `1793573593531771376-a5c808a4` | 2026-11-01 | 3-3 | `a2e66e80dd5c` | refused-before-launch | inconclusive | run_total 0.594 s |
| `1793576493129223154-47e61f61` | 2026-11-01 | 3-3 | `c8b67e0dd9e4` | returned | passed | run_total 133.524 s, workload 5.001 s, cores_verified 8 cores, npu_jobs 0 jobs |
| `1793576635217632637-5879361e` | 2026-11-01 | 3-3 | `e574d622a946` | watchdog-reset | failed | run_total 71.723 s |
| `1793581334418340064-a4b4d83e` | 2026-11-02 | 3-3 | `ea38709362f5` | result-mismatch | failed | run_total 77.417 s |
| `1793587317962103239-5bbfc48d` | 2026-11-02 | 3-3 | `5669db271593` | returned | passed | run_total 126.154 s, window_jobs_per_s 26653.6 jobs/s, window_latency_p50 10.125 µs, window_latency_p99 11.04 µs, window_latency_max 17.04 µs, charged_cpu_per_job 34.5 µs, cold_job_p50_9ms_gap 10.58 µs, cold_job_p50_100ms_gap 10.58 µs, matching_jobs 61231 jobs, deadline_margin_at_end 12.69 s |
| `1793592163786755757-7c8b2818` | 2026-11-02 | 3-3 | `c0fb7bbadc21` | watchdog-reset | failed | run_total 26.192 s, first_failure_line 257, open_reply_state 1 |
| `1793592717914134756-b95efca0` | 2026-11-02 | 3-3 | `24f8f0f6a0b5` | returned | passed | run_total 31.329 s, npu_jobs 4, npu_output_mismatches 0, overlap_mask 15, cpu_cores_acknowledged 8, npu_trigger_to_irq_p50 2904 us, npu_trigger_to_irq_max 2911.96 us |
| `1793595222816645677-be2320f0` | 2026-11-02 | 3-3 | `1417c8183b75` | returned | passed | run_total 24.292 s, jobs 61231, window_jobs_per_s 26964.4 jobs/s, window_latency_p99_ticks 251, window_latency_max_ticks 429, charged_cpu_per_job_ticks 816.693, gates 0, resumes 0, register_writes 0, resume_max_ticks 0, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595292307271613-0417379e` | 2026-11-02 | 3-3 | `b3e4795f525c` | returned | passed | run_total 23.857 s, jobs 61231, window_jobs_per_s 26964.9 jobs/s, window_latency_p99_ticks 259, window_latency_max_ticks 426, charged_cpu_per_job_ticks 816.792, gates 130, resumes 130, register_writes 780, resume_max_ticks 183, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595393793090144-178725b8` | 2026-11-02 | 3-3 | `4fd7815df3e0` | returned | passed | run_total 24.043 s, jobs 61231, window_jobs_per_s 27005.8 jobs/s, window_latency_p99_ticks 258, window_latency_max_ticks 403, charged_cpu_per_job_ticks 816.552, gates 0, resumes 0, register_writes 0, resume_max_ticks 0, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595461923184040-005689c1` | 2026-11-02 | 3-3 | `1417c8183b75` | returned | passed | run_total 24.102 s, jobs 61231, window_jobs_per_s 26962.4 jobs/s, window_latency_p99_ticks 256, window_latency_max_ticks 428, charged_cpu_per_job_ticks 816.586, gates 0, resumes 0, register_writes 0, resume_max_ticks 0, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595557085395502-ba052514` | 2026-11-02 | 3-3 | `b3e4795f525c` | returned | passed | run_total 24.02 s, jobs 61231, window_jobs_per_s 26965.4 jobs/s, window_latency_p99_ticks 252, window_latency_max_ticks 430, charged_cpu_per_job_ticks 816.488, gates 130, resumes 130, register_writes 780, resume_max_ticks 170, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595585257669811-c80ab9ea` | 2026-11-02 | 3-3 | `4fd7815df3e0` | returned | passed | run_total 24.095 s, jobs 61231, window_jobs_per_s 27011.6 jobs/s, window_latency_p99_ticks 251, window_latency_max_ticks 405, charged_cpu_per_job_ticks 816.401, gates 0, resumes 0, register_writes 0, resume_max_ticks 0, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595613409995497-5db5d0ff` | 2026-11-02 | 3-3 | `1417c8183b75` | returned | passed | run_total 25.411 s, jobs 61231, window_jobs_per_s 26963.9 jobs/s, window_latency_p99_ticks 255, window_latency_max_ticks 443, charged_cpu_per_job_ticks 816.599, gates 0, resumes 0, register_writes 0, resume_max_ticks 0, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595642848063892-cd5db0e8` | 2026-11-02 | 3-3 | `b3e4795f525c` | returned | passed | run_total 24.007 s, jobs 61231, window_jobs_per_s 26965.3 jobs/s, window_latency_p99_ticks 250, window_latency_max_ticks 428, charged_cpu_per_job_ticks 816.608, gates 130, resumes 130, register_writes 780, resume_max_ticks 175, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595670424088724-d888a853` | 2026-11-02 | 3-3 | `4fd7815df3e0` | returned | passed | run_total 24.022 s, jobs 61231, window_jobs_per_s 27003.2 jobs/s, window_latency_p99_ticks 309, window_latency_max_ticks 403, charged_cpu_per_job_ticks 816.336, gates 0, resumes 0, register_writes 0, resume_max_ticks 0, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595698599182458-0c1066a4` | 2026-11-02 | 3-3 | `1417c8183b75` | returned | passed | run_total 23.986 s, jobs 61231, window_jobs_per_s 26970.6 jobs/s, window_latency_p99_ticks 255, window_latency_max_ticks 428, charged_cpu_per_job_ticks 816.598, gates 0, resumes 0, register_writes 0, resume_max_ticks 0, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595726557173335-e009f1c3` | 2026-11-02 | 3-3 | `b3e4795f525c` | returned | passed | run_total 23.974 s, jobs 61231, window_jobs_per_s 26963.8 jobs/s, window_latency_p99_ticks 250, window_latency_max_ticks 428, charged_cpu_per_job_ticks 816.34, gates 130, resumes 130, register_writes 780, resume_max_ticks 165, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595754162008571-71c3a964` | 2026-11-02 | 3-3 | `4fd7815df3e0` | returned | passed | run_total 24.022 s, jobs 61231, window_jobs_per_s 27003.5 jobs/s, window_latency_p99_ticks 308, window_latency_max_ticks 407, charged_cpu_per_job_ticks 816.505, gates 0, resumes 0, register_writes 0, resume_max_ticks 0, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595781813556905-b8d31af9` | 2026-11-02 | 3-3 | `1417c8183b75` | returned | passed | run_total 24.072 s, jobs 61231, window_jobs_per_s 26961 jobs/s, window_latency_p99_ticks 259, window_latency_max_ticks 425, charged_cpu_per_job_ticks 817.111, gates 0, resumes 0, register_writes 0, resume_max_ticks 0, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595809517564687-116973a0` | 2026-11-02 | 3-3 | `b3e4795f525c` | returned | passed | run_total 24.111 s, jobs 61231, window_jobs_per_s 26966 jobs/s, window_latency_p99_ticks 255, window_latency_max_ticks 426, charged_cpu_per_job_ticks 816.489, gates 130, resumes 130, register_writes 780, resume_max_ticks 169, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595837683091653-202bf23e` | 2026-11-02 | 3-3 | `4fd7815df3e0` | returned | passed | run_total 23.97 s, jobs 61231, window_jobs_per_s 27004.3 jobs/s, window_latency_p99_ticks 323, window_latency_max_ticks 407, charged_cpu_per_job_ticks 816.311, gates 0, resumes 0, register_writes 0, resume_max_ticks 0, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595865267017264-80a35307` | 2026-11-02 | 3-3 | `1417c8183b75` | returned | passed | run_total 24.23 s, jobs 61231, window_jobs_per_s 26964.4 jobs/s, window_latency_p99_ticks 257, window_latency_max_ticks 432, charged_cpu_per_job_ticks 816.772, gates 0, resumes 0, register_writes 0, resume_max_ticks 0, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595893101543391-d845400a` | 2026-11-02 | 3-3 | `b3e4795f525c` | returned | passed | run_total 24.01 s, jobs 61231, window_jobs_per_s 26967 jobs/s, window_latency_p99_ticks 294, window_latency_max_ticks 429, charged_cpu_per_job_ticks 816.553, gates 130, resumes 130, register_writes 780, resume_max_ticks 169, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
<!-- evidence-table:end -->
