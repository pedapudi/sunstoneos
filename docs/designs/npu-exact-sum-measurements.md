# NPU exact-sum measurements

These measurements check the exact NPU sum study against the budgets in
[npu-exact-sum.md](npu-exact-sum.md). They compare the integration base
(`audit/integration`, commit `9b892b6`) with the current branch (commit
`e9b6300`). All values come from host builds, host checks and Unicorn
instruction emulation; no board or NPU was used.

## Method

- **Sources.** Each commit was exported with `git archive` into its own
  directory under `~/.cache/sunstoneos/build/npu-exact-sum-measure/`. The
  branch changes only host checks, host tools and documents:
  `checks/npu-mimus-cost.c`, `checks/npu-mimus-digits.cc`,
  `checks/npu-mimus-kernel.cc`, five scripts under `tools/` and four
  documents. No firmware, driver or session runtime source differs.
- **Code and data size.** Both commits were configured with the shared
  configuration script and built with `-j 6`. For each of the 13 default
  ELF targets, `llvm-size-21 -A` gave equal `.text`, `.rodata`, `.data` and
  `.bss` sizes, and `llvm-objcopy-21 -O binary` gave byte-identical loadable
  images at both commits.
- **Read-out exactness.** At each commit, `checks/npu-mimus-kernel.cc` was
  built with AddressSanitizer and UndefinedBehaviorSanitizer and run on the
  three catalogued INT16 studies. At the current commit it also ran on the
  digit studies of the query projection, the range fixture and the first
  feed-forward linear. The INT16 layer's mismatch count is the simulator
  comparison recorded by `tools/npu-mimus-linear.py`.
- **Per-job events.** `checks/npu-mimus-cost.c` from the current commit
  assembled the INT16 graph and the digit graphs and derived each job's
  events from the session runtime's code paths. The base commit has no
  event counter. The session runtime sources are identical at both
  commits, so the INT16 counts are the baseline for both.
- **Instructions.** `checks/npu-mimus-digits.cc` and the mimus
  `port/sunstoneos` kernel (revision `3e05158`) were compiled with the
  backend's AArch64 flags and linked into one ELF. `tools/npu-mimus-cost.py`
  ran the split, the recombination and the packed `MimusGemmS16FC` under
  Unicorn on the study's 8 rows. It checked each output against the
  simulator inputs and the reference bytes, then charged the events at the
  design's unit costs with CPU-B at 2,002 MHz and CPU-L at 494 MHz.
- **Barriers and locks.** `llvm-objdump-21` disassembled the three
  functions in the instruction-count ELF and found no `DMB`, `DSB`, `ISB`,
  load-acquire, store-release or exclusive instruction.

Every figure reproduced the values recorded in
[npu-mimus-linear.md](../npu-mimus-linear.md).

## Per-job results for the query projection, 8 rows

The baseline column is the catalogued INT16 layer on the unchanged session
runtime. The current column is the digit layer that the branch adds.

| Metric | Baseline (INT16 layer) | Current (digit layer) | Budget | Within budget |
| --- | --- | --- | --- | --- |
| Read-out floats differing from `MimusGemmS16FC` | 1,407 of 1,408 | 0 of 1,408; 0 of 1,408 for the range fixture; 0 of 5,632 for the first feed-forward linear | 0 on all three; K = 704 refused | Yes |
| NN multiply-accumulates | 247,808 | 991,232 | At most 991,232 | Yes |
| CPU bytes copied in by SUBMIT | 2,816 | 5,632 | 5,632 | Yes |
| CPU bytes copied out | 2,816 | 67,584 | 67,584 | Yes |
| Bytes cleaned / cleaned and invalidated / invalidated | 4,096 / 4,096 / 4,096 | 24,576 / 73,728 / 73,728 | Page-rounded tensor allocations: 6 × 4,096 inputs, 6 × 12,288 outputs | Yes |
| Cache lines maintained | 192 | 2,688 | Follows the row above | Yes |
| Bytes hashed with SHA-256 | 37,064 | 54,120 | INT16 plus allocation growth: 12,288 coefficient, 4,096 descriptor and 672 command bytes | Yes |
| Command words checked for END | 65,511 | 65,427 | INT16 plus growth | Yes |
| Page translations | 556 | 642 | INT16 plus growth: two per mapped page, 43 more pages | Yes |
| Page-table words read | 5,120 | 5,120 | INT16 plus growth | Yes |
| Queue round trips (wake-ups of the client) | 3 | 3 | 3 | Yes |
| CPU DDR bytes read / written | 587,264 / 5,632 | 671,232 / 73,216 | Sum of the rows above | Yes |
| Client instructions, digit split / recombination with read-out | None | 12,230 / 41,341 | Counted | Yes |
| Barriers and exclusive accesses in client code | None | 0 | No new barrier | Yes |
| Locks added | None | 0 | No lock | Yes |
| NPU cycles, compiler estimate | 11,193 | 51,104 | Not set; four times the MACs | — |

## End-to-end comparison with the A76 kernel

The packed NEON kernel executes 332,354 instructions for 8 rows, charged as
166,177 cycles, or 83.0 µs at 2,002 MHz.

| Path, 8 rows | CPU cycles, client + worker | Client time, SUBMIT to copied output | Against the kernel, cycles and time | Pass condition met |
| --- | ---: | ---: | --- | --- |
| INT16 layer, today's runtime | 1,619,380 | 3,281 µs | 9.7 and 39.5 times | No |
| Digit layer, today's runtime | 2,231,903 | 4,419 µs | 13.4 and 53.2 times | No |
| Digit layer with the runtime changes in the design | 105,571 | 230 µs | 0.64 and 2.8 times | No, time |

Per row, the exact path costs 21,348 cycles against the kernel's 20,772
at one client instruction per cycle, so no job size breaks even. At two
client instructions per cycle the exact path costs 18,000 cycles per row
and breaks even at 744 rows per job.

The design predicted this failure and states its cause: the per-job
SHA-256 of about 54 KB and the scan of the 512 KiB command allocation in
the session runtime. The failed comparison sends the work to the INT16
variant, which met the accuracy criterion in simulation. The digit layer
is not registered in the session catalogue, so no firmware path runs it.

## Size and firmware effect

| Item | Base | Current | Change |
| --- | --- | --- | --- |
| Firmware ELF targets with identical loadable bytes | 13 of 13 | 13 of 13 | None |
| `a733-mimus` text / data / bss | 417,904 / 3,680 / 4,490,624 B | 417,904 / 3,680 / 4,490,624 B | 0 |
| Digit split and recombination, AArch64 `.text`, not linked into firmware | Absent | 4,084 B (split 2,772 B, recombination 1,296 B) | Host study only |

## Conclusion

No metric is worse than its budget. The digit layer is larger than the
INT16 baseline in copies, maintenance, hashing and translations, and each
increase equals the budget the design sets from the layer's tensor and
allocation sizes. The CPU-cost comparison fails as the design predicted
and documents; the design records that failure as the study's result.
Firmware images are unchanged.
