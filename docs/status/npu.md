# NPU

**Status: implemented.**

Native model decoding, mappings, shared-provider startup, IRQ97 service, runtime clients and thermal policy have software evidence. Finite scheduled startup and inference have scoped physical results. Repeated-transition reliability remains unqualified. Opt-in internal clock gating of the idle NPU, its power owner and a benchmark with a baseline, a control and a gating image are implemented with host evidence only; no gating image has run on a board. A quiesced finite image that fails now returns to the boot ROM with a failure record instead of ending at the watchdog.

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
p50 and 26,925 jobs/s, and 1,000 fence-chained two-stage jobs, on 3-3. The baseline idle benchmark image (no power owner) returns normally with 61,231 matching jobs: five windows of 12,000 jobs at about 26,650 jobs/s, p99 11 µs, maximum 17 µs, and cold jobs after 9, 11 and 100 ms gaps at 10.6 µs p50.

## Open work

1. Run the control and gating idle images and five rounds of baseline, control and gating on port 3-3, then compare them with `apps/npu-finite/idle-compare.py` against the limits in [the design](../designs/npu-fast-path.md): throughput 98%, p99 105%, maximum 110%, charged CPU time 105%, every resume within 100 µs, no gating in 9 ms gaps. These runs wait for the comparison of the NPU paths with the vendor driver, TF-A and Linux GIC code.
2. Find which interrupt bind condition refuses the combined OS image at `os_npu_irq_bind`; the bind records the refusing condition (values 0xb01 to 0xb09) and the reference comparison of the NPU paths predicts it. A powered NPU at entry does not explain the refusal: every NPU image is admitted with the domain on. The NPU qualification branch owns the rerun.
3. Qualify the EL0 queue and prepared joined streams: review and finish the saved generic sequence registration and returning diagnostic (`npu-el0-next-1101`), add the public-ring predicate to the power owner, then run the control and the EL0 images.
4. Add exact per-job deadline interrupts through the scheduler's deadline facility (`work/scheduler-rt-1101`); do not add a device timer.
5. Measure energy: the PMIC provider exposes no DCDC2 current or power readback, so idle savings and break-even time have no evidence without an external meter.
6. Qualify repeated start and stop, shared SRAM access while the clocks are gated, and recovery from a fault while suspended.
7. Run the `restart-*` cases of `checks/npu-irq-route.py` on a restart image; the other cases pass.

## Evidence

Board results with a queue entry are records in `docs/evidence/npu.jsonl`, added with `tools/evidence.py add`. Results cited by path, and earlier prose evidence, are in [the historical log](../history/status-npu-20261101.md).

<!-- evidence-table:begin -->
8 records in `docs/evidence/npu.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793558381244643310-6a29a71d` | 2026-11-01 | 3-3 | `dd826f5ff757` | returned | passed | run_total 110.813 s, matching_jobs 2, matching_completions 4 |
| `1793558909256910454-ab3a76b4` | 2026-11-01 | 3-3 | `8ede7a8341fa` | returned | passed | run_total 110.785 s, matching_jobs 3, matching_completions 4 |
| `1793559233825594424-693cc430` | 2026-11-01 | 3-3 | `a0dd10679c00` | returned | passed | run_total 110.751 s, matching_jobs 2, matching_completions 3 |
| `1793559627833816772-2003f2e7` | 2026-11-01 | 3-3 | `f9cc623ab7de` | returned | passed | run_total 110.927 s, matching_jobs 200, matching_completions 201 |
| `1793563838443635411-92c1fdae` | 2026-11-01 | 3-3 | `862114046425` | returned | passed | run_total 111.246 s |
| `1793564135238449408-5dbdd6ad` | 2026-11-01 | 3-3 | `2a6411ba3fcc` | returned | passed | run_total 112.541 s |
| `1793565141195481474-2e135a8c` | 2026-11-01 | 3-3 | `ae8dc27e6c16` | returned | passed | run_total 111.135 s |
| `1793587317962103239-5bbfc48d` | 2026-11-02 | 3-3 | `5669db271593` | returned | passed | run_total 126.154 s, window_jobs_per_s 26653.6 jobs/s, window_latency_p50 10.125 µs, window_latency_p99 11.04 µs, window_latency_max 17.04 µs, charged_cpu_per_job 34.5 µs, cold_job_p50_9ms_gap 10.58 µs, cold_job_p50_100ms_gap 10.58 µs, matching_jobs 61231 jobs, deadline_margin_at_end 12.69 s |
<!-- evidence-table:end -->
