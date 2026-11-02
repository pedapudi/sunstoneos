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
p50 and 26,925 jobs/s, and 1,000 fence-chained two-stage jobs, on 3-3.

## Open work

1. Run the idle benchmark on port 3-3: the baseline smoke image (it links the failure return), then five rounds of baseline, control and gating images. Compare them with `apps/npu-finite/idle-compare.py` against the limits in [the design](../designs/npu-fast-path.md): throughput 98%, p99 105%, maximum 110%, charged CPU time 105%, every resume within 100 µs, no gating in 9 ms gaps.
2. Explain the first baseline smoke failure (a refused return that ended at the watchdog, outcome `watchdog-reset`); the refusal and panic crumbs now name the cause.
3. Establish a cold NPU at service start and make the interrupt bind of the combined OS image pass; the bind records the clause that refuses it. The NPU qualification branch owns this.
4. Qualify the EL0 queue and prepared joined streams: review and finish the saved generic sequence registration and returning diagnostic (`npu-el0-next-1101`), add the public-ring predicate to the power owner, then run the control and the EL0 images.
5. Add exact per-job deadline interrupts through the scheduler's deadline facility (`work/scheduler-rt-1101`); do not add a device timer.
6. Measure energy: the PMIC provider exposes no DCDC2 current or power readback, so idle savings and break-even time have no evidence without an external meter.
7. Qualify repeated start and stop, shared SRAM access while the clocks are gated, and recovery from a fault while suspended.
8. Repair the `monitor-*` and `restart-*` cases of `checks/npu-irq-route.py`; the others pass.

## Evidence

Board results with a queue entry are records in `docs/evidence/npu.jsonl`, added with `tools/evidence.py add`. Results cited by path, and earlier prose evidence, are in [the historical log](../history/status-npu-20261101.md).

<!-- evidence-table:begin -->
7 records in `docs/evidence/npu.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793558381244643310-6a29a71d` | 2026-11-01 | 3-3 | `dd826f5ff757` | returned | passed | run_total 110.813 s, matching_jobs 2, matching_completions 4 |
| `1793558909256910454-ab3a76b4` | 2026-11-01 | 3-3 | `8ede7a8341fa` | returned | passed | run_total 110.785 s, matching_jobs 3, matching_completions 4 |
| `1793559233825594424-693cc430` | 2026-11-01 | 3-3 | `a0dd10679c00` | returned | passed | run_total 110.751 s, matching_jobs 2, matching_completions 3 |
| `1793559627833816772-2003f2e7` | 2026-11-01 | 3-3 | `f9cc623ab7de` | returned | passed | run_total 110.927 s, matching_jobs 200, matching_completions 201 |
| `1793563838443635411-92c1fdae` | 2026-11-01 | 3-3 | `862114046425` | returned | passed | run_total 111.246 s |
| `1793564135238449408-5dbdd6ad` | 2026-11-01 | 3-3 | `2a6411ba3fcc` | returned | passed | run_total 112.541 s |
| `1793565141195481474-2e135a8c` | 2026-11-01 | 3-3 | `ae8dc27e6c16` | returned | passed | run_total 111.135 s |
<!-- evidence-table:end -->
