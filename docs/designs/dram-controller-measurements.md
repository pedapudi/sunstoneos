# DRAM controller measurements

The first comparison sets the tree before the DRAM controller work (commit
`041c09a`) against the tree that adds the DDR passive trip (commit `13037cf`),
using the budgets in the [DRAM controller design](dram-controller.md). Neither
tree has code for the memory-bus priority hook, the display-port latency
window or the copy load, so their rows in that comparison record no
measurement. The
[second comparison](#memory-bus-priority-window-and-decode-check) measures
that code against the budgets of the
[memory-bus priority design](dram-bus-priority.md).

## Conditions

- **Builds.** Both trees were built with the project's aarch64 Clang toolchain
  in separate directories under
  `~/.cache/sunstoneos/build/dram-controller-measure/`: the thermal image
  (`SUNSTONE_A733_THERMAL=ON`), the monitor image (`SUNSTONE_A733_MONITOR=ON`)
  and the integrated OS image (`SUNSTONE_A733_OS=ON`). These are the three
  images that link `drivers/thermal/a733_thermal.c`.
- **Sizes.** `llvm-size` and `llvm-nm` on the driver object and on each linked
  image.
- **Instruction counts.** `policy_emu.py` in the same directory loads each
  linked thermal and OS image into Unicorn 2.1.4, initializes a policy, and
  calls the linked `a733_thermal_policy_update` with one five-channel sample
  every 250 ms. Each count covers one call, including the two instructions of
  the stub that stands in for the notification callback. The script also
  counts `DSB`, `DMB` and `ISB` instructions and acquire or release accesses.
- **Host check.** `checks/thermal-policy.c` was compiled with
  AddressSanitizer and UndefinedBehaviorSanitizer against each tree's driver.
- No board was used.

## Results

| Metric | Budget | Before | After | Within budget |
| --- | --- | --- | --- | --- |
| Hook accesses | 2 CCU reads; at most 27 NSI accesses; 1 lock pair | 0 | 0; no hook code exists | Not measurable |
| Added accesses, tasks, timers and interrupts in HDMI, DisplayPort and steady-state images | 0 | 0 | 0 | Yes |
| Window accesses and barriers per phase | Start 6 NSI accesses, 2 ISB; collect 6 NSI reads, 1 ISB; no added DSB | None | None; no window code exists | Not measurable |
| Counter headroom over a 50 ms period | No 32-bit wrap | Not applicable | Arithmetic holds | Yes |
| Copy-load stop | No pass after the end phase | Not applicable | No copy-load code exists | Not measurable |
| Scan-out latency | Board result | Not measured | Not measured | Not measurable on the host |
| DDR trip, behavior | THROTTLE at 85.019 °C, held to 83.013 °C, released at 82.954 °C | No passive trip | As budgeted | Yes |
| DDR trip, instructions per policy update | Not budgeted | 196 | 199 to 217 | See below |
| DDR trip, notifications | 1 per level change | 0 | 1 at the trip, 1 at release, 0 otherwise | Yes |
| DDR trip, barriers per policy update | No added barrier | 0 | 0 | Yes |
| DDR trip, recognition real-time factor | At most 1.05 times the value without the trip | Not measured | Not measured | Board measurement |
| Code size outside the load mode | At most 2 KiB text, 128 B data | 0 | 68 B text, 0 B data | Yes |

### DDR trip

The policy update in the linked images takes these instruction counts per
call. The thermal image and the OS image give identical counts.

| Sample | Before | After | Level after the call |
| --- | --- | --- | --- |
| All channels at 30.008 °C | 196 | 207 | NORMAL |
| DDR at 84.960 °C | 196 | 203 | NORMAL |
| DDR at 85.019 °C | 196 | 209, one notification | THROTTLE |
| DDR at 84.960 °C, then 83.013 °C | 196 | 199 | THROTTLE |
| DDR at 82.954 °C | 196 | 217, one notification | NORMAL |
| All channels at 30.008 °C after release | 196 | 207 | NORMAL |

Before the change, the compiler omitted the passive comparison for the DDR
channel because its table entry was zero. The steady-state update therefore
costs 11 more instructions, a 5.6% increase. That cost is the 85 °C
comparison and its hysteresis test, which the design requires because LPDDR5
standard refresh is specified only up to 85 °C. The update runs once per THS
sample, has no barrier and touches no register, so the added cost is a few
nanoseconds per sample. Each call makes one release store in steady state and
two when the level changes, in both trees.

The policy check passes against both trees. The new check fails against the
old driver at its first DDR case, which shows that the check covers the trip.

### Code size

- `a733_thermal.c` text grows from 10,408 to 10,476 bytes. All 68 bytes are
  in `a733_thermal_policy_update` (1,216 to 1,284 bytes). The trip table is
  folded into the code, so no data or read-only data is added.
- Thermal image text grows from 512,732 to 512,800 bytes.
- OS image text grows from 843,680 to 843,744 bytes. The only symbol whose
  size changes is `a733_thermal_policy_update`.
- Monitor image text, read-only data and data sizes are unchanged. Its
  loadable bytes differ only in the 32-byte `a733_smp_build_id`, a digest of
  the build inputs, which include the changed `a733_thermal.h`. The monitor
  links no policy code.

### Counter headroom

At the 76.8 GB/s peak, a 50 ms period moves 3.84 GB, which is 2.4 × 10⁸
port 2 data units of 16 bytes. The period register holds 3 × 10⁷ cycles at a
600 MHz MBUS clock. Both are below 2³² (4.29 × 10⁹). By Little's law, the
read latency sum over one period equals the mean number of outstanding reads
times 3 × 10⁷ cycles, so it stays below 2³² while DE averages fewer than 143
outstanding reads. These figures match the design.

## Conclusion

Every measurable metric is within its budget. The one metric worse than
before, 11 instructions per steady-state policy update, is the DDR trip's
own comparison, which the design requires. The scan-out and real-time-factor
rows wait for a board session.

## Memory-bus priority, window and decode check

### Conditions

- **Trees.** Before: the integrated tree at commit `dfa5174`, which has no
  hook, window, copy or decode code. After: that tree with
  `drivers/dram/`, `include/a733_dram.h` and the three `checks/dram-*` files
  added. Both were copied to paths of equal length under
  `~/.cache/sunstoneos/build/dram-bus-priority/` (`mb-src`, `hd-src`), so
  embedded source paths do not change image sizes.
- **Images.** The gpu-display, HDMI, DisplayPort and integrated OS
  (`SUNSTONE_A733_OS=ON`) images were built from each tree with the
  project's aarch64 Clang toolchain. `llvm-size`, `llvm-nm` and a SHA-256 of
  each image's `.text` were compared. The current revision changes only
  `drivers/dram/`, `include/a733_dram.h`, `checks/dram-*` and documentation.
  No image compiles those files: `memory.c` declares
  `a733_dram_decode_check()` itself, in a layout 2 branch that no image
  compiles. The image comparison therefore holds for the current revision.
- **Host check.** `checks/dram-controller.py` ran on the after tree. The
  before tree has no code to run it against, so every figure from it is a
  new value against a baseline of no code. Its source comparison now also
  checks the port numbering: the sun60iw2 port enum, the `pmu_name[]` match
  that sets the port of each field write, `sub_node_id_mapping`, the
  register index it selects for the counter reads, the counter clock, and
  the U-Boot base and stride read from the pinned U-Boot archive. It also
  compares the `MBUS_CLK_REG` bit whose setting refuses the first start with
  `MBUS_DFS_EN` in the manual. Three mutations, a 0x100 port stride, port 13
  in place of port 14 and bit 27 in place of bit 28, each fail the check.
- **Earlier code.** The "Unmerged code" column repeats the figures the design
  records for commit `02154a1`.
- No board was used.

### Results

| Metric | Budget | Before | Unmerged code | After | Within budget |
| --- | --- | --- | --- | --- | --- |
| Hook accesses | 2 CCU reads; at most 27 NSI; 9 at the table; 0 with the NSI off; 1 lock pair | 0 | 21 from reset fields, 9 at the table, 27 worst | 2 CCU reads; 21 NSI from reset fields, 9 at the table, 27 worst; 9 reads-only; 0 NSI with the NSI off. The lock pair belongs to the framebuffer caller and is measured only when that caller merges | Yes for the accesses; the lock pair is not yet measurable |
| Window accesses per phase | Start 6, plus one `+0x588` read in the first start; collect 6, or 7 after the fallback; pending 0; at most 4,100 per session | 0 | 4,008 per session | Start 6, 7 in the first start; collect 6, 7 after the fallback; pending 0 and 1 counter read; 4,009 per session (40 phase A and 294 phase B windows) | Yes |
| `MBUS_DFS_EN` refusal | 1 CCU read, no port access, phases ended early | Not applicable | No check | Of the 32 bits of `+0x588`, only bit 28 refuses; the refusing start makes 1 read and 1 lock pair, and later steps and abandon make 0 accesses, `enter` calls and counter reads | Yes |
| Accesses, `enter` calls and counter reads after a failure | 0 | Not modeled | Not modeled | 0 after a failure at the first start's `+0x588` read, each of the 6 start accesses, each of the 6 collect accesses, the fallback write and a failed `enter` | Yes |
| Instructions outside the callbacks | Hook 600; start 200; collect 300; pending 50 | None | Not counted | Hook 489 from reset fields, 363 at the table, 552 worst, 71 with the NSI off; reads-only 307; start 161 for the first start and 148 later; collect 158; pending 39 | Yes |
| `DSB` and `DMB` in the component's objects | 0 | 0 | 0 | 0 | Yes |
| `DSB SY` per sequence, register path included | Hook up to 58; start 12; collect 12–14 | 0 | Same | Derived from the raw path's source, two per access, times the modeled access counts: hook 58 worst; start 12, 14 for the first start; collect 12, 14 after the fallback | Yes, as derived; no linked image counts them |
| `phase_ticks_max` | Longest span, saturating at 65,535 | Not carried | Offset 38 always 0 | Spans of 100 ticks, then 300, then 70,000 give 100, 300, then 65,535 at offset 38 | Yes |
| Copy kernel | 6 instructions per 64 bytes; 2 acquire loads per pass | None | Same | 6; 2 | Yes |
| Code size | Outside the load mode at most 2 KiB text, 128 B data; decode check at most 16 B | 0 | Hook 863 B; reads-only 723 B; window 2,452 B; copy 32 B; 0 B data | Hook 704 B; reads-only 588 B; window 3,000 B; copy 32 B; decode 8 B; 0 B data | Yes |
| Added `a733_dram_` symbols, tasks, timers, interrupts and tick wake-ups in the gpu-display, HDMI, DisplayPort and OS images | 0 | 0 | 0 | 0; see the image table | Yes |
| CPU time per window phase | At most 20 µs (480 ticks) | None | Not measured | Not measured: `phase_ticks_max` from both load images | Board measurement |
| Scan-out latency | Scan-out row of the DRAM controller design | Not measured | Not measured | Not measured | Board measurement |

The window's text grows by 548 bytes over the unmerged code. That growth is
the failure latch, the monotonic counter check, the phase span and the
`MBUS_DFS_EN` read, which the design adds. The hook's text shrinks by 159
bytes. Its table holds each field's address, mask, target and evidence
position, where the unmerged code computed the address and target on every
access, and the hook writes the evidence block directly instead of through a
separate structure and copy function. The hook zeroes the 136-byte block
field by field, because a structure assignment of that size compiles to a
`memset` call; each hook count above includes that zeroing. Each window phase counts 50 ms of
about 51 ms, because a collect and the next start run in consecutive 1 ms
steps; the design records that loss of about 2% as accepted.

Each image has the same text, data and bss sizes and the same symbol names
and sizes before and after, and none has an `a733_dram_` symbol:

| Image | Text (B) | Data (B) | Symbols | `a733_dram_` symbols |
| --- | --- | --- | --- | --- |
| gpu-display | 532,124 | 33 | 1,131 | 0 |
| HDMI | 159,248 | 0 | 434 | 0 |
| DisplayPort | 610,292 | 145 | 1,818 | 0 |
| Integrated OS | 889,492 | 3,216 | 2,482 | 0 |

The `.text` bytes of the OS image are identical. Those of the gpu-display,
HDMI and DisplayPort images differ, but two builds of the before tree's HDMI
image also differ from each other at the same section layout and symbol
addresses. These builds are not byte-reproducible, so the byte comparison
cannot attribute the difference to this change; the matching sizes and
symbol tables show that no new code is linked.

### Conclusion

Every host-measurable budget is met, and no image changes, because no image
links the component until the framebuffer and gpu-display edits merge. The
CPU time per phase and the scan-out latency need the two load images in one
board session.
