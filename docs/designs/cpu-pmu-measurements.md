# CPU PMU regions: measurements against the design budgets

Status: **host-modeled and emulated**, with core-0 figures **measured on the
board** in section "Board measurement on core 0". Every other figure comes
from Unicorn instruction emulation or from QEMU. The budgets are in
[cpu-pmu.md](cpu-pmu.md).

## Conditions

- Base tree: the merge base with the integration branch,
  `14c3cd8cd27ca4da69a986c222ef691e319aff4e`. Measured tree: commit
  `1aa1493`, which adds the module and its checks. The base tree was extracted
  with `git archive`; the measured images were built from the worktree, whose
  header matched that commit. Each tree was configured and built in its own
  directory with clang 21 and the project toolchain file.
- The base tree has no region module. Its comparable mechanisms are the
  inline `ISB`, `MRS PMEVCNTR0_EL0` pair that QEMU fixtures use and the pair
  of `a733_cpu_cycles_sample` calls that times work today. A bare-metal EL1
  program built from the base tree measures both under QEMU 10.2.1
  (`virt`, `-icount shift=0`, `-cpu cortex-a55` and `-cpu cortex-a76`) with
  counter 0 counting `INST_RETIRED`. The access count of the sample pair comes
  from the base tree's `checks/cpu-clock-check.py` instruction model.
- Module figures: `checks/cpu-pmu.py` (Unicorn, board SMP flags with
  `-mgeneral-regs-only`, eight-core configuration) and `checks/cpu-pmu-smp.py`
  (QEMU 10.2.1, two cores, the FreeRTOS SMP port at EL1, `-icount shift=0`).
  QEMU counts instructions exactly; its cycle counter follows virtual time and
  says nothing about A733 cycles.
- Images: the new header joins the header globs that seven image identities
  hash. `sunstone-a733-storage-maintenance`, `-smp-cached-devices`, `-board`,
  `-camera`, `-gpu-render`, `-hdmi` and `-mimus-native` were built from both
  trees. In each, every differing byte lies in the 32-byte
  `a733_smp_build_id`; the code and data are otherwise identical. No image
  links the module.
- Validation on the measured tree: `checks/cpu-pmu-smp.py` passes on both
  CPU models. `checks/cpu-pmu.py` passes its 20 module cases and verifies three
  pinned sources, then fails on the continuation sources, as the design
  requires until both continuation paths copy `PMCR_EL0.N` into
  `MDCR_EL2.HPMN`. `checks/cpu-clock-check.py` passes unchanged.
- Repeat measurement: a second, independent run built the base tree and
  commit `bdce0fa` from `git archive` exports in separate directories. That
  commit changes only documentation after `1aa1493`. The run reproduced every
  figure in the table: the base harness under QEMU (4 and 64 instructions on
  both models), the base Unicorn count (22 accesses, 2 `ISB`, 56
  instructions), the 224-byte base object, the module's 14 + 14 accesses and
  70 / 95 instructions, the 1,488-byte module code, the 512-byte state, and
  20 instructions per empty region on both cores of both QEMU models. The
  `-board`, `-smp-cached-devices` and `-storage-maintenance` images again
  differ only inside `a733_smp_build_id`, and no image contains a module
  symbol. The base harness needs `-mstrict-align`, because its output buffer
  is written with unaligned stores while the MMU is off.

## Results

| Metric | Base | Measured | Budget | Within budget |
| --- | --- | --- | --- | --- |
| Instructions in an empty region, QEMU A55 / A76, cores 0 and 1 | Inline `ISB`, `MRS` pair: 4 / 4 | 20 / 20 on both cores | ≤ 24 | Yes |
| Loop of 2,000 instructions, counted above the empty loop region | — | 2,000 on both cores and both models | Exact | Yes |
| System-register accesses, begin + end, configured path (`MSR DAIFSet` included) | `a733_cpu_cycles_sample` pair: 22, cycle counter only | 14 + 14 = 28, six event counters and the cycle counter | ≤ 28 | Yes |
| `ISB` per begin + end | 2 | 1 + 1 | ≤ 2 | Yes |
| `DSB`, `DMB`, `SVC`, `CurrentEL` reads, configuration readbacks per region | `CurrentEL` read twice; three configuration reads per sample | 0 | 0 | Yes |
| Locks per region | 0 | One kernel critical section in `vTaskCoreAffinityGet`, before the counted window | As stated | Yes |
| Allocations, MMIO accesses, wake-ups per region | 0 | 0 | 0 | Yes |
| Memory touched per call | — | Stack, region, record, the core's own state line | As stated | Yes |
| Lines written by more than one core | — | 0 | 0 | Yes |
| Record / state / region size | — | 64 B / 8 × 64 B, 64-byte aligned / 24 B | 64 B / 512 B / ≤ 32 B | Yes |
| Instructions per call in Unicorn, configured path, begin / end | 56 for the sample pair | 70 (stub affinity call included) / 95 | — | — |
| Module code (`.text`, four functions) | 224 B (`a733_cpu_cycles_native.o`) | 1,488 B | — | — |
| Empty-region cycles on cores 0 and 6 | Not measured | Core 0 (A55) on the board: 64 at the minimum and median, 131 at most; core 6 not measured | ≤ 150 A55, ≤ 100 A76 (provisional) | Core 0 yes; core 6 needs a board session |

The base pair of `a733_cpu_cycles_sample` calls counts 64 instructions
between two inline counter reads on both QEMU models; one inline read is
included in that figure. It reads only the cycle counter and checks the cycle
configuration and `CurrentEL` on every call. The module's configured path
instead checks the enable bit once per region and reads six event counters as
well.

QEMU implements `INST_RETIRED` and none of the other five events, so the
supported mask is 0x1 on both models; the instruction model covers the other
events with modeled `PMCEID` bits.

## Board measurement on core 0

Port 3-3 ran the single-core form of the module on core 0 (Cortex-A55,
`MIDR_EL1` `0x412fd050`) on 2026-10-24, in bundle 4 of
[board-bundles-next.md](board-bundles-next.md); raw log
`~/.cache/sunstoneos/evidence/board-session-20261023/port-3-3/bb4-run/kernel.log`.
IRQs were masked around each region.

- **Configuration.** `PMCR_EL0` reads `0x41453041` (six event counters),
  `PMCEID0_EL0` `0x7fffffff` and `PMCEID1_EL0` `0x01f0ae7f`; all six events
  of the record are implemented (supported mask `0x3f`), and no exception
  occurred.
- **Empty region.** 20 instructions in each of 64 regions, as in both QEMU
  models; 64 cycles at the minimum and median, 131 at most.
- **Fixed region.** 2,000 ALU instructions count 2,020, exactly 2,000 above
  the empty median, in 7,238 cycles.
- **Clock.** A 1 ms counter loop counts 494,104 cycles over 24,005 ticks of
  the 24 MHz counter: 494.0 MHz, the CPU-L rate.
- **`L2D_CACHE_REFILL`.** Implemented on the A733's A55: one load per line
  of a cleaned and invalidated 64 KiB buffer, 1,024 lines, counts 1,031
  refills.

Cores 6 and 7 (Cortex-A76) have not run the module on the board.

## Evidence

`~/.cache/sunstoneos/evidence/cpu-pmu-20261022/`:

- `baseline.json`, `baseline-qemu.log`, `baseline-unicorn.json` and
  `baseline-harness/`: the base-tree measurements and the program that made
  them;
- `host/results.json` and `host.stdout`: `checks/cpu-pmu.py`, with the access
  counts, object hash, section sizes, pinned and unfetched sources and the
  continuation failures;
- `smp/result.json` and the per-model logs: `checks/cpu-pmu-smp.py`;
- `cpu-clock-check/`: the unchanged CPU clock check;
- `image-comparison.txt`: the byte comparison of the seven images.
