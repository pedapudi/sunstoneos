# Memory architecture measurements

These measurements compare stage 1 of the memory architecture with the
integration commit it branches from, against the budgets in
[memory-architecture.md](memory-architecture.md). Every figure comes from a
host check, a QEMU fixture or the object files of a target build. No board
was used, so no row in this document qualifies a cycle count.

## Inputs

- **Baseline:** commit `c45a76e`, the merge base of the change and the
  integration branch.
- **Change:** commit `a786170`.
- **Sources.** The baseline was checked out from a shared clone and the
  change was exported with `git archive`, each into its own directory under
  `~/.cache/sunstoneos/build/memory-architecture-measure/`. The checks that
  read Git history ran from the change's clean worktree.
- **Toolchain.** clang 21 (`/usr/lib/llvm-21/bin/clang`), the project's QEMU
  build (`qemu-system-aarch64`) and FreeRTOS kernel `97f50c0`. The
  cached-devices image links with the GNU `aarch64-none-elf-ld` 14.3, as the
  capacity check requires.
- **Checks run at both commits.** `checks/device-memory-capacity.py` and
  `checks/memory-reservations.py --mode all`, each commit running its own
  version of the check. Both pass at both commits.
- **Checks run at the change only.**
  `checks/memory-architecture-smp.py --compare c45a76e:base` builds the
  two-core QEMU fixture once against the change's pool and once against the
  baseline pool and headers. For the baseline it measures only the
  per-access calls (describe, one xHCI register read, the camera memory
  check), because the baseline ledger has no lock or getter to measure. The
  measure, trace and contention runs all pass.
- **Baseline pool check.** The baseline capacity check prints no
  instruction count. A trace script
  (`tools/prepare_trace.py` in the measurement directory) reruns each
  commit's capacity image in the publication case with `-icount shift=0`
  and `-d exec,in_asm,nochain`. It sums the instructions and `AT`
  instructions that core 0 executes from each entry to
  `a733_device_memory_prepare()` until the block at the call's return
  address. On the change it reports 22,544 instructions, three fewer than
  the fixture's PMU count of 22,547, so the two methods agree.
- **Code and data size.** Both commits were configured with the shared
  configuration script and built with `-j 6` for `sunstone-a733-board`, a
  local-layout image that links the pool, the reservations and the
  `uncached` pool. The capacity check also builds
  `sunstone-a733-smp-cached-devices`, the extended-layout image, at each
  commit. Sizes come from `aarch64-none-elf-size`.

Logs, ELF files and `result.json` files are in the measurement directory:
`smp/` for the QEMU fixture, `base-capacity/` and `head-capacity/` for the
capacity check, `base-reservations/` and `head-reservations/` for the
reservations check, `board-base/` and `board-head/` for the image builds,
and `base-prepare.json` and `head-prepare.json` for the traced pool check.

## Results

| Metric | Budget | Baseline | Change | Within budget |
| --- | --- | --- | --- | --- |
| Extended pool check | 392 `AT` in one call; at most 25,000 instructions | 401,410 `AT` in 3,136 calls; 18,466,209 instructions | 392 `AT` in one call; 22,547 instructions | Yes |
| Local pool check (`AT` per preparation): 2 MiB pools, large NPU graph | Unchanged: 1,536 and 2,048 | No count in the baseline check | 1,536 and 2,048 | Yes |
| DRAM mapped at EL1; translation-table memory | 910 MiB; 20 KiB | 20,480-byte table region | 910 MiB by golden comparison; 20,480-byte table region | Yes |
| Slot comparisons per acquire | At most 1,056 | Not replayed | 160 worst, in the high-profile H.264 replay | Yes |
| Critical-section hold: acquire, release, retain (instructions) | Acquire at most 15,000; release and retain at most 100 | No serialization | 14,206 (31 occupied slots); 31; 24 | Yes |
| Critical-section pair per acquire, release or retain | One pair | None | 485 instructions; 5 `SVC`, 27 `DSB`, 7 `ISB`, 8 or 9 `DMB` | Yes; explained regression |
| Local getter call after preparation | No `SVC`, `DSB`, `ISB` or `DMB` | One plain load | 17 instructions, no barrier | Yes; explained regression |
| Critical-section hold of a local pool's first preparation | At most 15,000 instructions | None | 344 instructions | Yes |
| Describe | No `SVC`, `DSB` or `ISB` | 71 instructions, no barrier | 92 instructions, 1 `DMB` | Yes; explained regression |
| xHCI register read (`rd()`) | No `SVC`, `DSB` or `ISB` | 371 instructions, no barrier | 413 instructions, 2 `DMB` | Yes; explained regression |
| Camera memory check (`memory_owned()`) | No `SVC`, `DSB` or `ISB` | 102 instructions, no barrier | 123 instructions, 1 `DMB` | Yes; explained regression |
| GPU MMU entries per replayed image | 2 MiB and 64 KiB pages where aligned | 354 to 4,568 (4 KiB pages) | 24 to 137 | Yes |
| Largest free extent before each later allocation | No request newly refused | 4 KiB placement | None refused; up to 1,056 pages smaller | Yes; explained regression |
| Bytes cleaned on first use of a page | 0 | Callers clean whole extents | 0, except 77,824 (GPU images) and 1,048,576 (high-profile H.264) at admission | Yes; explained regression |
| Code and data size, extended-layout image | No budget | `.text` 57,300; `.bss` 570,000 | `.text` 56,124; `.bss` 569,968 | Yes: 1,176 and 32 bytes smaller |
| Code and data size, local-layout board image | No budget | `.text` 496,012; `.bss` 1,201,856 | `.text` 496,940; `.bss` 1,201,856 | No budget; 928 bytes more code |
| Task wake-ups, interrupts, copies per ledger call | Unchanged | None | None added | Yes |
| Plain clean cost; GPU session open; NPU model load | Board cycles | Not measured | Not measured | Provisional |

Instruction counts in the table are PMU instructions retired under
`-icount shift=0`, except the baseline pool check, which comes from the
execution trace. The traced counts for the per-access calls and the getter
are six to eight instructions higher than the PMU counts at both commits,
because the trace also counts instructions between the semihosting markers
and the PMU reads. The traced barrier counts for describe, the
xHCI register read, the camera check and the getter are zero for `SVC`,
`DSB` and `ISB` at both commits.

## Findings

- **Extended pool check.** The change replaces 3,136 calls of 128 page
  translations each with one call that translates one address per 2 MiB
  block. Translations fall from 401,410 to 392, and instructions from
  18,466,209 to 22,547, a factor of about 819. The baseline instruction
  count is lower than the 27,474,026 that the design records for the state
  before stage 1, because this trace counts only instructions inside the
  preparation calls; both figures are far above the budget, and the change
  meets it. The baseline `AT` count is two higher than the design's 401,408.
- **Explained regressions.** Describe gains 21 instructions and one `DMB`,
  the xHCI register read 42 instructions and two `DMB`, and the camera check
  21 instructions and one `DMB`. Acquire, release and retain each gain one
  critical-section pair. The prepared getter costs 17 instructions with one
  load-acquire. The design lists each of these, with its reason, under its
  performance budgets.
- **Code size.** The extended-layout image is 1,176 bytes smaller in
  `.text`, because the one-call pool check replaces the bounded-step
  preparation in `device_memory_layout.c` (3,700 to 1,892 bytes of object
  text). The local-layout board image is 928 bytes larger in `.text`. The
  added code is the ledger's critical-section pair, the describe generation
  check, aligned acquire with the clean watermark, and the one-call
  preparation of the local pools; the pool object grows from 1,416 to 2,000
  bytes and each local preparation object from 356 to 456 or 524 bytes in
  the reservations check's build. The design gives a correctness reason for
  each of these mechanisms but sets no code-size budget.
- **Contention.** Under multithreaded TCG the fixture ran 273,956 acquire
  rounds on core 0, with 10,864 releases, 10,200 retains and 10,293 acquires
  entered while the other core held the critical section, and no overlapping
  slots. In 100,000 slot-reuse rounds, 1,304,111 describes of a released
  lease returned the published extent and state and 18,234,937 were refused.
- **Unmeasured rows.** Cycle counts, the plain clean cost, stage 2 session
  open and NPU model load need the board runs listed in the design.
  Per-transition `DC` for the GPU and NPU is unchanged by stage 1.
