# Power authority: measurements against the design budgets

Status: **host-modeled**. Every figure below comes from instruction
emulation or a host model; no figure comes from the board. The budgets are in
[power-authority.md](power-authority.md).

## Conditions

- Base tree: the merge base with the integration branch,
  `d40ca4b72b42348880d89f1ebc4eaec61704b8d0`. Measured tree: commit
  `1fbc911c9671e53b7d6dd381265444b1e5faf616`. Each tree was extracted with
  `git archive` and configured and built in its own directory.
- Images: `sunstone-a733-board`, `sunstone-a733-hdmi` and
  `sunstone-a733-storage-maintenance`, built with clang 21 and the project
  toolchain file. Both trees build without errors.
- Rail and clock costs: `checks/rail-leases-cost.py` from the measured tree,
  run once with `--root` set to each tree. It compiles the ledger, the clock
  registry and the GPU resource manager with the production code-generation
  flags (`-march=armv8-a -mtune=cortex-a55 -mstrict-align -O2`) and counts the
  instructions that Unicorn executes and the distinct 64-byte lines of those
  objects that each operation touches. The small configuration holds 42 clock
  and 16 rail leases; the full configuration fills the rail table.
- PMIC costs: `checks/a733-pmic-cost.c` from the measured tree, compiled with
  each tree's provider, ledger, clock registry, GPU resource manager, pin units
  and `checks/a733-pmic.c` model. The model clock advances 1 µs per time read
  and each bus event completes 90 µs after the write that starts it.
- Native-adapter costs per request (counter-clock lock acquisitions,
  mutex-holder queries and `DSB SY`) follow from the adapter source and the
  register-access counts above. In the base adapter every time read calls
  `a733_counter_clock_now`, every register access queries the I2C mutex
  holder, and every register access carries a `DSB SY` before and after it.
  In the measured adapter the time read is `ISB; MRS CNTPCT_EL0` with no lock,
  the holder is queried once at bus entry, and the barriers are unchanged.
- Validation run on the measured tree: `checks/rail-leases-cost.py --limits`,
  `checks/a733-pmic-cost.c --limits`, `checks/pmic-tree.py` and
  `checks/cpu-clock-check.py` all pass.

## Results

| Operation | Base | Measured | Budget | Within budget |
| --- | --- | --- | --- | --- |
| Rail status, one-rail token, small / full | 222 / 888 instructions; 34 / 256 lines | 37 / 37; 3 / 3 lines | ≤ 60 instructions, ≤ 3 lines | Yes |
| Clock token status, small / full | 235 / 592 instructions | 45 / 45 | ≤ 50, no scan | Yes |
| Acquire, poll and release, small / full | 1,643 / 3,565 instructions | 335 / 335, no bus access | ≤ 450 | Yes |
| Borrow; end borrow, small / full | 823 / 1,531; 271 / 923 instructions | 153 / 153; 86 / 86 | — | — |
| Authority check per PMIC transaction | 498 / 1,507 instructions; 68 / 353 lines | 6 / 6; 1 line | 1 acquire load, ≤ 6 instructions | Yes |
| Ledger storage | 20,096 B | 1,600 B | ≤ 1,632 B | Yes |
| Clock registry storage | 6,208 B | 6,208 B | — | — |
| Time reads per snapshot; per selector write | 590; 732 | 469; 599 | — | — |
| Counter-clock lock acquisitions per snapshot; per selector write | 590; 732 | 0; 0 | ≤ 2; ≤ 2 | Yes |
| Mutex-holder queries per snapshot; per selector write | 250; 309 | 1; 1 | — | — |
| Register accesses per snapshot; per selector write | 250; 309 | 211; 270 | — | — |
| Setup reads per snapshot; per selector write | 64; 70 | 25; 31 | 3, plus 3 after each STOP (21; 27) | No, reason stated |
| `DSB SY` per snapshot; per selector write | 500; 618 | 422; 540 | 7 per read transaction, 5 per write | No, reason stated |
| Polls per snapshot; per selector write (model) | 44; 56 | 44; 56 | 0 ticks, with the service task | No, reason stated |
| Image `.text`, board / HDMI / storage maintenance | 496,940 / 120,768 / 179,852 B | 494,828 / 118,712 / 177,804 B | — | — |
| Image `.bss`, board / HDMI / storage maintenance | 1,201,856 / 382,032 / 437,952 B | 1,183,424 / 363,600 / 419,520 B | — | — |
| Package object code, five power and clock objects | 25,369 B | 25,113 B | — | — |

The `.bss` figures are the `.bss` section reported by `llvm-size -A`; each
image shrinks by 18,432 B, which is the ledger reduction less alignment.

Package object code per object, identical in all three images:

| Object | Base | Measured |
| --- | --- | --- |
| `a733_pmic.c` | 10,948 B | 12,176 B |
| `a733_pmic_native.c` | 1,701 B | 1,965 B |
| `a733_rail_leases.c` | 5,832 B | 4,080 B |
| `a733_clock_leases.c` | 3,456 B | 3,468 B |
| `a733_pmic_consumer.c` | 3,432 B | 3,424 B |

## Conclusions

- Every implemented budget holds: rail status, clock status, the
  acquire-poll-release cycle, the per-transaction authority check, the ledger
  size and the counter-clock lock count. Rail and clock status cost the same
  with 16 and with 31 holders, so neither scans.
- Three rows remain outside their targets, each for a reason the design
  states. Setup reads exceed 21 by four because admission rereads the S_TWI0
  clock route and divisor, which `checks/a733-clock-reference-pmic.c`
  requires for drift detection. The adapter keeps two `DSB SY` per register
  access until a board run qualifies a lighter placement. Polls stay at 44
  per snapshot because the PMIC service task needs a second task-notification
  index and a scheduled-core constant from the primary agent.
- The provider object grows by 1,228 B. Of that, the per-phase transfer
  function grows by 716 B and the register helpers by 100 B each, from the
  inlined state-word check and fault-bit write that replace the per-phase
  borrow and clock-registry queries; the new `route` function adds 576 B
  and the startup-only baseline check shrinks by 420 B. The native adapter
  grows by 264 B for the raw counter read and the holding flag. The ledger
  shrinks by 1,752 B, so the package objects shrink by 256 B and each image's
  `.text` shrinks by about 2 KiB.
- No measured performance metric is worse than the base.

## Reproduction

```sh
M=~/.cache/sunstoneos/build/power-authority-measure
git archive d40ca4b72b42348880d89f1ebc4eaec61704b8d0 | tar -x -C $M/base-src
git archive 1fbc911c9671e53b7d6dd381265444b1e5faf616 | tar -x -C $M/head-src
PYTHONDONTWRITEBYTECODE=1 ~/.cache/sunstoneos/venv/bin/python \
    $M/head-src/checks/rail-leases-cost.py --root $M/base-src
# In each tree, with checks/a733-pmic-cost.c copied from the measured tree:
clang-21 -std=c11 -Wall -Wextra -Werror -Iinclude -Ichecks -O1 cost.c \
    drivers/power/a733_pmic.c drivers/power/a733_rail_leases.c \
    drivers/clock/a733_clock_leases.c drivers/gpu/a733_gpu_resources.c \
    drivers/pinctrl/a733_pins.c drivers/pinctrl/a733_pin_tables.c -o cost
```

## Board evidence still required

These measurements do not replace the board evidence listed in the design:
PMU cycle counts for rail and clock status on an A55 and an A76, S_TWI0 bus
timing and spin lengths, and a comparison of snapshots and selector writes
under the current and a lighter barrier placement.
