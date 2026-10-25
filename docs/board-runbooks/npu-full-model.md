# Two ShuffleNetV2 starts in one board lifetime

Status: **runbook only**; nothing here has run. A set frozen from it needs a
root approval of each start's identities and grants before its claim.

## What the run measures

The interrupt-driven finite image built with `--shufflenet`
([design](../designs/npu-full-model.md)) runs the published ShuffleNetV2
graph twice per start with its published input, compares both outputs with
the SDK's `[0,255]`, and logs each job's phases, interrupt time and NPU
cycles. One cold lifetime on port 3-3, whose speed bin admits 852 MHz:

- **Start 1, 492 MHz, cold.** `build.py --interrupt --shufflenet`.
- **Start 2, 852 MHz, rate change.**
  `build.py --interrupt --shufflenet --rate-852`. Its register pass retunes
  the PLL that start 1 left at 492 MHz, at the unchanged 800 mV.

The procedure, records, observations and stop conditions are those of
[the interrupt runbook](npu-irq-starts.md) with these images; the host is
unchanged.

## Predictions

- Every start: status 0, normal ROM return, verified DRAM restoration, both
  outputs `[0,255]`, three notified completions and the stopped receipt;
  startup 4.0 ms.
- Doorbell to interrupt about 2.8 ms at 492 MHz and 2.0 ms at 852 MHz
  (ranges 1.5–6 ms and 1.0–5 ms); time ratio 0.6–0.9, central 0.72;
  active-cycle ratio 1.0–1.5, central 1.21; idle under 10% of the job's
  cycles.
- Publication to doorbell about 17 µs, as for the one-operation job.
- Submit phase 1–5 ms and publish phase 5–30 ms per job, on the CPU before
  the doorbell.
