# DRAM controller measurements

These measurements compare the tree before the DRAM controller work (commit
`041c09a`) with the tree that adds the DDR passive trip (commit `13037cf`),
against the budgets in the [DRAM controller design](dram-controller.md). The
second tree implements only the DDR trip. The memory-bus priority hook, the
display-port latency window and the copy load are designed but have no code
in either tree, so their rows record no measurement.

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
own comparison, which the design requires. The hook, window and copy-load
rows wait for their code, and the scan-out and real-time-factor rows wait for
a board session.
