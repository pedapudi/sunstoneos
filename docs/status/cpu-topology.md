# CPU topology queries and measured maximum capacity

**Status: implemented.**

**Board-qualified scope:** Port 3-1 supplies architectural identity, cache geometry,
LSE instruction support and seven fixed-work integer/NEON samples on every core.
The applied points are CPU-L 1794 MHz at 1000 mV, CPU-B 2002 MHz at 1050 mV
and DSU 1274 MHz, with speed-bin word `0x495`, bin 4.
Three paired eight-core trials preserve exact work and reduce median completion
time from 43.090 to 26.777 ms. Both images return normally with seven secondaries
off, native retirement validated and DRAM restored. These finite results do
not qualify sustained operation, other operating points, energy costs or thermal recovery.

## Immutable maximum weights remain separate from operating state

The A733 topology table records six Cortex-A55 cores and two Cortex-A76 cores
in one DynamIQ cluster. Each core has private level-one and level-two caches;
all eight share a 1 MiB level-three cache. Maximum integer capacity is 285 for
each A55 and 1024 for each A76. The A76 class is the normalization reference.
The weights use the measured class-median throughput ratio, without treating
small differences between identical cores as distinct capacities. The table
records the calibration entry, workload and epoch. Energy and live effective
capacity remain unknown.

`SUNSTONE_A733_CPU_TOPOLOGY` defaults to OFF. Enabled images capture architecture
registers before tasks, restore the cache selector, and publish immutable RAM
observations. Queries check identity and table validity once. The admitted mask
describes board policy; application grants control task placement.

`ss_work_split()` requires effective-capacity data. `ss_work_split_max()` explicitly
uses calibrated maximum weights and makes no assertion about current clocks.
The comparison application verifies the approved top points before work and an
unchanged clock epoch afterward. It calls the public maximum-weight helper once
when preparing ranges. The clock owner retains the existing operating-point tables.

## Every core supplies identity, features and fixed-work throughput

Each kernel performs 4096 element updates for 64 rounds after three warm-ups.
An element update applies a shift/XOR, multiplication and addition to one
32-bit value. Timing includes initialization and digest computation. Throughput
counts 262,144 element updates per sample. Seven samples provide each median;
the reported p99 is the nearest-rank sample maximum, with no tail guarantee.

All cores report LSE support and 64-byte cache lines. MPIDR includes its MT bit;
core affinity occupies Aff1 and Aff0 is zero. Cache sizes below are KiB, in
level-one instruction/data, level-two and shared level-three order.

| Core | MPIDR | Type | Cache KiB | Integer Mupdates/s | NEON Mupdates/s | Integer median/p99/max µs | NEON median/p99/max µs |
| --- | --- | --- | --- | ---: | ---: | --- | --- |
| 0 | `0x81000000` | A55 | 32/32/128/1024 | 195.727 | 431.661 | 1339.333/1344.417/1344.417 | 607.292/607.375/607.375 |
| 1 | `0x81000100` | A55 | 32/32/128/1024 | 195.965 | 432.849 | 1337.708/1337.708/1337.708 | 605.625/605.625/605.625 |
| 2 | `0x81000200` | A55 | 32/32/128/1024 | 195.965 | 432.849 | 1337.708/1337.708/1337.708 | 605.625/605.625/605.625 |
| 3 | `0x81000300` | A55 | 32/32/128/1024 | 196.008 | 432.849 | 1337.417/1337.458/1337.458 | 605.625/605.625/605.625 |
| 4 | `0x81000400` | A55 | 32/32/128/1024 | 195.965 | 432.849 | 1337.708/1337.708/1337.708 | 605.625/605.625/605.625 |
| 5 | `0x81000500` | A55 | 32/32/128/1024 | 196.008 | 432.849 | 1337.417/1337.708/1337.708 | 605.625/605.625/605.625 |
| 6 | `0x81000600` | A76 | 64/64/256/1024 | 704.136 | 1771.243 | 372.292/375.583/375.583 | 148.000/148.000/148.000 |
| 7 | `0x81000700` | A76 | 64/64/256/1024 | 702.642 | 1771.742 | 373.083/375.083/375.083 | 147.958/148.000/148.000 |

The integer A76/A55 ratio is 3.589357, with paired-repetition ratios from
3.573575 to 3.605571. The NEON ratio is 4.092637, with ratios from 4.092061
to 4.105932. The pairs compare class medians at each repetition index inside
the same image. Scheduler maximum weights use the integer profile; the NEON
ratio remains a separate workload measurement.

Median elapsed cost per integer update is about 5.10 ns on an A55 and 1.42 ns
on an A76. These counter-based costs include interruptions; active CPU cycles
and energy per operation are not measured.

## Capacity weighting reduces completion time and finish skew

One image runs the same integer kernel across all eight cores, with 256 tiles
and one private 16 KiB buffer per worker. Each tile performs one complete
fixed-work kernel. Equal ranges assign 32 tiles per core. Maximum-capacity
ranges assign `20,20,19,19,19,19,70,70` tiles to cores zero through seven.

One warm-up pair precedes three measured pairs. Each pair runs equal splitting
and then proportional splitting. Dispatch and completion use task notifications;
completion uses a private notification index. Measured jobs have no tick waits.
The initial service-startup wait is outside the measured intervals.

Time starts before dispatch and ends at the last worker finish, excluding later
controller acknowledgment processing. Every tile and digest is checked.
Identical before/after clock text includes the transition count; each phase
also records ready and unthrottled state.

| Split | Median ms | p99/max ms | Median finish skew ms | Throughput Mupdates/s |
| --- | ---: | ---: | ---: | ---: |
| equal | 43.090 | 43.094 | 32.234 | 1557.400 |
| proportional | 26.777 | 26.784 | 3.003 | 2506.178 |

Capacity weighting improves median completion by 1.609 times. Residual finish
skew is 3.003 ms. These weights are placement hints for the named workload,
with finite-sample scope.

Each lag below is the median finish time relative to the earliest finisher
in that trial. Medians are computed per core, so their differences need not
equal a difference between separately computed median finish times.

| Core | Equal finish lag ms | Proportional finish lag ms |
| --- | ---: | ---: |
| 0 | 31.982 | 3.003 |
| 1 | 31.907 | 2.958 |
| 2 | 31.912 | 1.628 |
| 3 | 31.912 | 1.629 |
| 4 | 31.914 | 1.633 |
| 5 | 32.234 | 1.819 |
| 6 | 0.008 | 0.016 |
| 7 | 0.000 | 0.000 |

## Frozen records identify the measurements and their limits

The capacity record is queue entry `1793576407894634321-03a919d6`, source
`50fea837`. Its queue outcome remains `result-mismatch`: the original expectation
omits MPIDR.MT on every core. Independent decoding validates the complete payload,
and an audit confirms that these eight MT bits are the only word mismatches.
The corrected expectation and its host model include MT. No unchanged image is
rerun, and the queue record is preserved.

- Capacity image SHA-256: `8d341cfc3f36e758cd0b126de46e16904d0febe35ac80542b0789d5829cd02a6`.
- Capacity output SHA-256: `3aadc67a97a08d178ab4ff0ec215eb189783e2c2ce2cccd2d05f9a91780c44f1`.
- Comparison entry: `1793577150613173047-15e57e89`, source `b741802a`, outcome `returned`.
- Comparison image SHA-256: `37d659bd2dc958b7bdcfad2de68669a60f67153b25c71e7cc4db3e23a54e57be`.
- Comparison output SHA-256: `1a625276c8c9e4979df1fa30f0a2db88cbc9b148ee7199ed340c95cd685c9f5d`.

Both runner records verify 800 mV on DCDC3 and DCDC5 before CPU execution.
Normal return and restoration establish this finite cleanup path. Fault recovery
and lower operating points remain outside these results.

Evidence paths below are relative to
`~/.cache/sunstoneos/build/sched-topology-1101/`.

- `capacity-evidence-audit.json`, `capacity-startup-fix-decoded.json` and
  `capacity-summary.json` retain the decoder result, mismatch explanation and
  per-core samples. The authoritative raw output and runner result remain under
  `~/.cache/sunstoneos/board-queue/entries/1793576407894634321-03a919d6/run/`.
- `split-decoded.json` and `split-summary.json` retain all paired times, ranges
  and per-core finish lag. Raw output and retirement evidence remain beneath
  comparison entry `1793577150613173047-15e57e89` in the same queue directory.
- `review-evidence/split/review.md` independently approves the calibration data,
  maximum/effective API distinction, synchronization and decoder. The controller
  has a 16 KiB stack; the compiler-reported call frames total 6768 bytes before
  runtime/context overhead. Worker stacks remain 8 KiB.
- `maximum-wrappers-complete/`, `measured-topology/` and `split-host-final/` pass
  focused sanitizer, symmetric, three-cluster, unknown-capacity, work-conservation
  and record-rejection cases. Scalar/NEON arithmetic retains the independent
  digest `f0ebbf03e42fda66`.
- `link-on-measured-build.log` and `link-off-measured-build.log` pass native
  hello links with all five topology query/split entry points retained.
- `gate-sched-topology-measured-full-1101/summary.txt` passes the full integration
  gate on `b741802a`: all OS variants, the pinned mimus baseline, kernel and
  eight-core soak checks, the check index and indexed host checks.
  External-input checks listed as pending are not physical qualification.
