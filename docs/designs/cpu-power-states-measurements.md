# Measurements: idle cores wait in WFI

These measurements compare the integration base with this package against the
budgets in [cpu-power-states.md](cpu-power-states.md). All results come from
host builds and the instruction model of
[a733-smp-cached.py](../../checks/a733-smp-cached.py); no board executed
either image.

## Conditions

- **Base:** commit `14c3cd8`, the merge base of this package with the
  integration branch.
- **Package:** commit `8072abf`. The package adds the idle function in
  `include/a733_cpu_power.h`, its check and its documentation. No image
  includes the header until the scheduler owner applies the image change that
  the design lists under "Changes outside this package". The model
  measurement of the package therefore applies that change as a patch to the
  staged tree.
- **Builds:** both trees configured with the project's clang 21 toolchain and
  the pinned FreeRTOS kernel; the integrated OS image built with
  `SUNSTONE_A733_OS=ON` and `SUNSTONE_A733_OS_RESIDENT=ON`.
- **Model workload:** [cpu-power-states-idle.c](../../checks/cpu-power-states-idle.c)
  replaces the cached image's diagnostic. One task per secondary core blocks
  on a notification while the core 0 task sleeps for 64 ticks. The check
  counts SGIs, timer wake-ups and executed instructions per core over that
  window, then requires every secondary task to wake for a notification and
  for a two-tick delay.

## Results

| Metric | Budget | Base | Package with image change | Within budget |
| --- | --- | --- | --- | --- |
| SGIs to idle secondaries per tick, no ready task | 0 | 4.47 (295 in 66 ticks) | 0 (0 in 64 ticks) | yes |
| Instructions per idle secondary in the window | 0 | 328,857 to 597,773 | 0 | yes |
| Instructions on core 0 in the window | not budgeted | 463,508 | 70,551 | not budgeted |
| Core 0 timer wake-ups per second | 1,000 | 1,000 | 1,000.7 | yes |
| Instructions per idle-loop iteration, passive and active | at most 6 more | 3 and 4 | 5 and 6 | yes |
| Static instructions in `xTaskIncrementTick` | one list-length compare | 241 | 265 | no; reason below |
| Integrated OS image, allocated bytes | no growth | 10,850,228 | 10,850,228 | yes |

The base window spans 66 ticks because the core 0 task wrote its end marker
two ticks late. The core 0 wake-up rate of 1,000.7 per second comes from 64
timer interrupts over the model's clock count for the window; the model
advances its clock in fixed steps, so the rate is 1,000 within one step.

**Idle loop.** Each idle loop gains `DSB SY` and `WFI` per iteration and one
entry branch:

| Function | Base | Package with image change |
| --- | --- | --- |
| `prvPassiveIdleTask` | 7 instructions, loop `LDR`, `CMP`, `B.LO` | 10 instructions, loop adds `DSB SY`, `WFI` |
| `prvIdleTask` | 11 instructions, loop `BL`, `LDR`, `CMP`, `B.LO` | 14 instructions, loop adds `DSB SY`, `WFI` |

The added `DSB SY` is one barrier per idle wake-up, which section 1.2 of
`docs/performance-architecture.md` permits before `WFI`. In the base image the
same loop spins without a barrier and never stops.

**Tick handler.** `xTaskIncrementTick` grows by 24 instructions (96 bytes).
The compiler unrolls the time-slice loop over the eight cores, and each core's
iteration adds `CMP`, `MOV` and `CSINC` to select the idle-priority threshold
beside the existing list-length compare. No memory access is added.
[cpu-power.md](../cpu-power.md) records this excess over the budget and its
reason: in an idle system the 24 register instructions per tick replace seven
SGIs and seven context switches per tick.

**Image size.** At the package commit no image includes the new header, so
the integrated OS image is unchanged. Its `.text`, `.data`, `.bss` and
`.rodata` sizes match the base, and `.text` and `.data` are byte-identical.
The only differing allocated bytes are the 32-byte build identity in
`.rodata`, which hashes the source tree. With the image change applied, the
cached diagnostic image keeps its `.text` and `.bss` sizes; its function bytes
grow by 140, which section alignment absorbs. Its `.rodata` grows by 24 bytes
because assertion messages name the patched `tasks.c` by a longer build path.

## Reproducing

```sh
PATH=$HOME/.cache/sunstoneos/host/usr/lib/llvm-21/bin:$PATH \
PYTHONDONTWRITEBYTECODE=1 ~/.cache/sunstoneos/venv/bin/python checks/cpu-power-states.py \
  --evidence <new directory> --freertos <pinned FreeRTOS-Kernel> \
  --source <base tree> --record-only
PATH=$HOME/.cache/sunstoneos/host/usr/lib/llvm-21/bin:$PATH \
PYTHONDONTWRITEBYTECODE=1 ~/.cache/sunstoneos/venv/bin/python checks/cpu-power-states.py \
  --evidence <new directory> --freertos <pinned FreeRTOS-Kernel> \
  --patch <image change>
```

The first command records the base metrics without applying the budgets. The
second applies the budgets and reports a pass when idle secondaries receive no
SGI and execute no instruction over the window and still wake for work.

## Limits

The model has no A733 power, cache or interconnect timing, so these numbers
show interrupts sent and instructions executed, not energy or wake-up latency.
A board session must still show EL0 `WFI` with `SCTLR_EL1.nTWI` on all eight
cores, woken by SGI0 and by the timer, with no time-slice SGIs to idle cores.
