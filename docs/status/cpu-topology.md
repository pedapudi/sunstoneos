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

## Open work

1. Supply effective-capacity data so that `ss_work_split()` can use current clocks and thermal state; only the maximum-weight split `ss_work_split_max()` works today.
2. Measure capacity at lower operating points and under thermal throttling; the table holds the top point only.
3. Measure per-core energy costs; the optional energy fields are empty for the A733.
4. Connect the topology to task placement in the partitioned scheduler; `SUNSTONE_A733_CPU_TOPOLOGY` defaults to off and the control kernel does not read it.
5. Decide whether NEON-heavy work uses its own weight; the NEON ratio (4.093) differs from the integer ratio (3.589) that the maximum weights use.
6. Repeat the capacity comparison for sustained operation and fault recovery.
7. Publish the boot CPU observation in the CPU-return profile. `ss_cpu_observations` (`runtime/cpu_topology_app.c`, from `ss_cpu_observation_copy`, `platform/a733/cpu_observation.c`) returned 0 for every core in the storage-write application's runs on the A733 CPU-return image (`SUNSTONE_A733_OS_CPU_RETURN`, A76 cluster, `PERF_BOOT=high`), so an application cannot learn its core's `ID_AA64ISAR0_EL1` (the register is not readable at EL0). The storage-write test falls back on the measured table in [the hardware facts](../hardware/a733-facts.json) (`cpu.id_aa64isar0_el1` = 0x100010211120 on all eight cores, SHA2 = 1) and reports that it did. Backlog: the snapshot is published by `ss_cpu_observe_boot` from `platform/a733-smp-cached/continue.c`; find why the CPU-return profile's cores do not publish it. Not fixed in the storage-write branch.

## Evidence

Board results with a queue entry are records in `docs/evidence/cpu-topology.jsonl`, added with `tools/evidence.py add`. Results cited by path, and earlier prose evidence, are in [the historical log](../history/status-cpu-topology-20261101.md).

<!-- evidence-table:begin -->
2 records in `docs/evidence/cpu-topology.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793576407894634321-03a919d6` | 2026-11-01 | 3-1 | `8d341cfc3f36` | result-mismatch | failed | run_total 120.97 s |
| `1793577150613173047-15e57e89` | 2026-11-01 | 3-1 | `37d659bd2dc9` | returned | passed | run_total 121.342 s |
<!-- evidence-table:end -->
