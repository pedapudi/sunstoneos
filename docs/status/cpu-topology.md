# CPU topology queries and capacity calibration

**Status: blocked.**

**Board-qualified scope:** No topology or capacity image has board evidence.
Port 3-3 is paused after queue entry `1793572859635159128-187e5901` from
another worker fails its return window. Coordinator recovery and queue resume
are required before capacity measurement. All per-core throughput, capacity
ratios, repetition spread and parallel split comparisons remain unmeasured.

## Application queries link with topology enabled and disabled

`SUNSTONE_A733_CPU_TOPOLOGY` defaults to OFF. Enabled images capture each
CPU's architectural registers before its first task. Queries validate the
immutable SoC description against those boot observations and copy bounded
records. A captured identity mismatch prevents topology publication. Missing
observations permit a later query after boot publication completes.

The metadata's admitted mask describes board policy. Application grants select
task placement separately. The A733 table describes six Cortex-A55 cores and
two Cortex-A76 cores in one cluster, with a shared level-three cache. Measured
cache geometry and instruction features remain separate boot observations.

The topology build links the clock owner's existing operating-point tables
without requiring its service. Unreferenced clock-transition code is removed
by section garbage collection. Capacity-image selection requires the CPU clock
service, which also requires A76 application placement.

## Host evidence covers queries and fixed-work arithmetic

Evidence paths are relative to
`~/.cache/sunstoneos/build/sched-topology-1101/`.

- `wrappers/`: sanitizer checks cover disabled queries, incomplete boot
  publication, identity refusals, output capacity, calibrated splitting and
  permitted MIDR revision differences.
- `topology/`: A733, symmetric, three-cluster and simultaneous-multithreading
  fixtures preserve their validation and work-conservation results.
- `observation/`: register models cover cache-selector completion, legacy and
  extended geometry, restored selectors and unknown feature encodings.
- `capacity-host/` and `capacity-kernels.elf`: the host producer and compiled
  AArch64 scalar/NEON instruction models agree on digest `f0ebbf03e42fda66`.
  Clock-epoch changes invalidate calibration while retaining raw samples.
- `link-on/` and `link-off/`: native hello images link with all four public
  query wrappers retained. The enabled image includes architectural capture
  and the OPP lookup without the clock service; the disabled image omits them.
- `review-evidence/review.md`: independent review approves the wiring and
  capacity configuration correction. Its separate sanitizer checks and native
  links pass. `refuse-capacity-no-clock.log` records the configuration refusal.

The fixed-work probe contains three warm-ups and seven measured repetitions
per kernel and core. These host checks establish arithmetic and record layout;
they supply no measured board capacity or energy cost.

## Physical calibration waits for coordinator recovery

The runner pause and blocking result are copied into `blocked-board/`.
No capacity image is frozen or submitted while the port is paused. The next
run uses the existing CPU-return path on port 3-3. The runner first normalizes
DCDC3 and DCDC5 to 800 mV. The approved workload points are CPU-L 1794 MHz at
1000 mV, CPU-B 2002 MHz at 1050 mV and DSU 1274 MHz.

The image must return with per-core identity, cache geometry, LSE instruction
support and integer/NEON timing samples. A stable clock epoch and complete
work digests are required for calibration. The resulting integer and NEON
ratios need separate repetition spreads. Scheduler capacity data and the
equal-versus-proportional eight-core comparison depend on that result.
Maximum capacity, effective capacity and energy costs remain explicitly unknown.
