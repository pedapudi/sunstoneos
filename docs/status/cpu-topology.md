# CPU topology queries and measured maximum capacity

**Status: implemented.**

Core identity, cache geometry and counter frequency are in [the A733 hardware facts](../hardware/a733-facts.md).

**Board-qualified scope:** Port 3-1 supplies architectural identity, cache geometry,
LSE instruction support and seven fixed-work integer/NEON samples on every core.
The applied points are CPU-L 1794 MHz at 1000 mV, CPU-B 2002 MHz at 1050 mV
and DSU 1274 MHz, with speed-bin word `0x495`, bin 4.
Three paired eight-core trials preserve exact work and reduce median completion
time from 43.090 to 26.777 ms. Both images return normally with seven secondaries
off, native retirement validated and DRAM restored. These finite results do
not qualify sustained operation, other operating points, energy costs or thermal recovery.

## Capabilities

| Capability | Status | Key figure | Evidence and design |
| --- | --- | --- | --- |
| Architectural identity, cache geometry and LSE support per core | qualified on the board | Seven fixed-work samples on every core of port 3-1 | `1793576407894634321-03a919d6`; [design](../designs/scheduler-partitioned.md#cpu-topology-is-immutable-data-observed-operating-state-is-separate) |
| Measured maximum capacity (integer and NEON) and maximum-weight work split `ss_work_split_max()` | qualified on the board | Median completion 43.090 to 26.777 ms over three paired trials; integer ratio 3.589, NEON ratio 4.093 | `1793577150613173047-15e57e89` |
| Effective-capacity work split `ss_work_split()` | open | None | [design](../designs/scheduler-partitioned.md#applications-query-topology-and-split-parallel-work-proportionally) |
| Capacity at lower operating points, under throttling, per-core energy | open | The table holds the top point only | none |
| Topology as an input to task placement | open | `SUNSTONE_A733_CPU_TOPOLOGY` defaults to off | [design](../designs/scheduler-partitioned.md#capacity-hints-guide-placement-without-weakening-priorities-or-affinity) |

## Open work

1. Supply effective-capacity data so that `ss_work_split()` can use current clocks and thermal state; only the maximum-weight split `ss_work_split_max()` works today.
2. Measure capacity at lower operating points and under thermal throttling; the table holds the top point only.
3. Measure per-core energy costs; the optional energy fields are empty for the A733.
4. Connect the topology to task placement in the partitioned scheduler; `SUNSTONE_A733_CPU_TOPOLOGY` defaults to off and the control kernel does not read it.
5. Decide whether NEON-heavy work uses its own weight; the NEON ratio (4.093) differs from the integer ratio (3.589) that the maximum weights use.
6. Repeat the capacity comparison for sustained operation and fault recovery.
7. Publish the boot CPU observation in the CPU-return profile. `ss_cpu_observations` returned 0 for every core in the storage-write application's runs on the A733 CPU-return image, so an application cannot learn its core's `ID_AA64ISAR0_EL1` (the register is not readable at EL0); the storage-write test falls back on the measured table in [the hardware facts](../hardware/a733-facts.json) (record `cpu-topology-cpu-return-observation-missing`). Find why the profile's cores do not publish the snapshot that `ss_cpu_observe_boot` (`platform/a733-smp-cached/continue.c`) creates.

## Evidence

Board results with a queue entry are records in `docs/evidence/cpu-topology.jsonl`, added with `tools/evidence.py add`. Results cited by path, and earlier prose evidence, are in [the historical log](../history/status-cpu-topology-20261101.md).

<!-- evidence-table:begin -->
3 records in `docs/evidence/cpu-topology.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `cpu-topology-cpu-return-observation-missing` |  |  | `` | returned | failed |  |
| `1793576407894634321-03a919d6` | 2026-11-01 | 3-1 | `8d341cfc3f36` | result-mismatch | failed | run_total 120.97 s |
| `1793577150613173047-15e57e89` | 2026-11-01 | 3-1 | `37d659bd2dc9` | returned | passed | run_total 121.342 s |
<!-- evidence-table:end -->
