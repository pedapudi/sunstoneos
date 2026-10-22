# Memory qualification measurements

These measurements compare the tree before the memory qualification image
(commit `d40ca4b`) with the tree that adds it, against the budgets in the
[memory qualification design](memory-qualification.md). The image is
described in [memory-qualification.md](../memory-qualification.md). No board
was used; every figure comes from a host model, and figures marked
*estimated* come from the design's cost model.

## Conditions

- **Builds.** Both trees were exported from git and built with the
  project's aarch64 Clang 21 toolchain under
  `~/.cache/sunstoneos/build/memory-qualification-measure/`: the tree before
  the change in `base/`, the new tree in `head/`. Each build links
  `a733-power-probe`, the central build of the returning platform that every
  returning image links. The new tree also links `a733-memtest`, added to the
  configuration through the deferred include that
  [memory-qualification.md](../memory-qualification.md) gives.
- **Translation tables.** `checks/memtest-image.py` runs the linked
  `platform_init` in Unicorn 2.1.4 and reads every table word it writes.
- **Counts.** `checks/memtest-emulate.py` runs the linked kernels and the
  image's task in Unicorn with a modeled counter and classifies instructions
  with Capstone 5.0.7. Each walk kernel runs over one 2 MiB block in each
  direction, the harness kernels over 1 MiB, and complete runs over a reduced
  64 MiB tested map.
- **Plan.** `checks/memtest-plan.py` drives the image's admission and
  planner, compiled for the host, over the board geometry.
- **Cost model.** Eight cycles per four-register `LD1` or `ST1`, one per
  other instruction, half a cycle per taken loop branch, no dual issue,
  494 MHz.

## Results

| Metric | Budget | Before | After | Within budget |
| --- | --- | --- | --- | --- |
| DRAM tested per board | 12,224 MiB less excluded ranges | 8 KiB (board record) | Scan and signature 12,224 MiB; march plan 12,224 MiB in 48 members and 23 groups; every fixed extent inside the tested map | Yes |
| DRAM mapped at EL1; table memory | 12,288 MiB; 8 KiB | 128 MiB (126 MiB write-back, 2 MiB non-cacheable); 8 KiB | 128 MiB; 8 KiB. 12,288 MiB once the platform writes the tested-map entries | No; the entries belong to `platform.c`, the reason the design's budget row states |
| Translations | 3 at admission; none afterwards | None | 3 in a complete run | Yes |
| March cycles per line | Write ≤ 12, read-verify ≤ 21, read-verify-write ≤ 33.5 | None | 12.19, 21.19, 33.19 | Read-and-write yes; write and read no, by 0.19 cycles of loop decrement and branch, the reason the design's budget row states (see below) |
| March work per block outside the loop | 1 `ISB` and counter read, 1 `UMAXV`, 1 progress store, ≤ 1 `SMC` per 200 ms; no call, maintenance, `DSB` or `DMB` | None | As budgeted, in every kernel and direction | Yes |
| `PRFM` and load-to-use | 1 `PRFM` per loaded line, 16 lines ahead; each `LD1` one line before its use | None | 32,767 `PRFM` for 32,768 loaded lines, all 16 lines ahead outside a member's last 16 lines; at least 17 (read-and-write) and 12 (read) instructions between a load and its use | Yes |
| March rate at 494 MHz, *estimated* | Write 2–3, read 1.5–1.9, read-and-write 0.94–1.6 GB/s; 0.91–1.45 s per 256 MiB | None | 2.59, 1.49, 0.95 GB/s; 1.41 s per 256 MiB | Write, read-and-write and time yes; read no, 1.49 against 1.5, by the same loop cost (see below) |
| Runs for March C-, *estimated* | 3–5 | None | 3 at the fast rate, 5 at the slow rate | Yes |
| Window scan | 6,258,688 loads; ≤ 9 instructions per page; ≥ 15 from load to use | None | 6,258,688 loads; 6.58 instructions per page; 15 | Yes |
| Address lines, bits 6–25 | 21 writes, 21 reads | None | 21 writes, 21 reads, 21 `DC CIVAC`, 1 `DSB SY` | Yes |
| Page signature | 3,129,344 each of `DC ZVA`, store, `DC CIVAC`, load; 6,113 `DSB SY`; no linefill read; ≤ 7 instructions per page per pass | None | 3,129,344 each; 6,112 `DSB SY`; no load in the write pass; 5.58 and 3.72 instructions per page | Yes; see below |
| Recovery digest | 192 `DC CIVAC`, 1 `DSB SY`, 12 KiB of loads | None | 192, 1, 12,288 bytes | Yes |
| Harness instructions | Read ≤ 2, write ≤ 3, copy ≤ 5 per line; chase ≤ 1.25 per load | None | 1.5 (both directions), 2.75, 4.5; 1.25 | Yes |
| Wall time and renewal | ≤ 20 s, with a fault in every march block too; gap ≤ 200 ms plus one block | None | Admission clamps to 20 s; with 11 ms of counter per block and a 1 s budget the march stopped at a group boundary, and renewals were at most 209 ms apart. With 0.1 s of counter per read, a 1 s budget and one bad word in every block, the run returned at 1.20 s with its mismatches, as a clean run does; an image without the deadline check after a rescan returned at 4.70 s | Yes |
| March eviction distance | ≥ 256 MiB (128 blocks) between a write and the next read | None | 128 blocks in every group of the board plan; reversing the down elements' member order gives 0 | Yes |
| DMA0 cases | ≤ 8 transfers of ≤ 1 MiB | None | Not built; admission refuses the bits | Not measurable |

### Per-line cost of the write and read elements

The design's budgets for the write and read elements, 12 and 21 cycles,
count only the per-line work; its read-and-write budget of 33.5 also counts
half a cycle of loop branch. The kernels unroll eight lines per iteration, so
the loop's decrement and branch add 0.19 cycles per line to each element. The
per-line work matches the design's sequence: 12, 21 and 33 cycles. The write
and read elements therefore exceed their budgets by 0.19 cycles, 1.6% and
0.9%, and the read rate of 1.49 GB/s falls below the 1.5 GB/s lower bound by
the same loop cost. A 2 MiB block holds 32,768 lines, so the kernels need a
loop, and the design's budget rows state this loop cost as the reason for the
excess: a 16-line unroll would leave 12.09 and 21.09 cycles, and a 32-line
unroll 12.05 and 21.05 cycles at 1.50 GB/s for 3.6 KiB more kernel text. A
March C- pass over 256 MiB takes 1.41 s, inside the 0.91–1.45 s budget.

### Signature barriers

The write pass issues one `DSB SY` after each of the 6,112 blocks of the
tested map. The barrier after the last block is also the barrier at the end
of the pass, so the design's separate end barrier is not issued.

### Translation entries

The image uses the platform's tables. Until `platform/a733/platform.c` writes
the tested-map entries under the memtest build, the image's mapping check
refuses each run, which `checks/memtest-emulate.py` confirms. With the entries
written as the design specifies, the tables map 12,286 MiB write-back and
2 MiB non-cacheable in the same 8 KiB.

## Image size and shared code

`a733-memtest.elf` holds 37,184 bytes of text, 8,416 of data and 79,840 of
BSS; the uploaded binary is 53,440 bytes. The text includes the entry page and
the four recovery pages, which the linker pads to whole pages. The image is a
new target; before the change no image tested memory.

The change adds files only inside its own directories and edits no shared
source. `a733-power-probe` links byte-identical ELF files before and after the
change (20,320 bytes of text, 168 of data, 62,352 of BSS), so no existing image
gains instructions, data, barriers or copies.
