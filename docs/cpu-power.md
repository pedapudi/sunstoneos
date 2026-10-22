# Idle cores wait in WFI

Status: **implemented in the instruction model; not yet in the merged
images.** The idle function and its check are in this tree. The image change
that enables them touches the cached image configuration, its EL1 entry code,
the FreeRTOS kernel and four build files; the eight-core scheduler's owner
applies it. No board has executed it. The design and its budgets are in
[designs/cpu-power-states.md](designs/cpu-power-states.md).

## Mechanism

Every image built on the cached eight-core configuration
(`platform/a733-smp-cached/FreeRTOSConfig.h`) runs one FreeRTOS idle task per
core. When no other task is ready, each idle loop executes `DSB SY` then `WFI`,
from `a733_cpu_idle()` in [a733_cpu_power.h](../include/a733_cpu_power.h). The
core stops until an interrupt is pending for it:

- **SGI0** when another core makes a task ready for this core. The kernel
  treats a core that runs its idle task as preemptible by any task, so the
  ready task's core always receives the SGI.
- **Timer PPI30** on core 0, every 1 ms tick.

The image change has four parts:

- **Idle hook.** The configuration sets `configUSE_PASSIVE_IDLE_HOOK` to 1,
  includes the header and defines `vApplicationPassiveIdleHook` as
  `a733_cpu_idle`. In a multicore build the kernel calls the passive hook from
  both idle loops, the active `prvIdleTask` and every `prvPassiveIdleTask`, so
  one hook covers all eight cores. The kernel's own `extern` declaration of the
  hook then names the header's `static inline` definition, which C gives
  internal linkage. The two instructions are inlined into each idle loop, and
  no image needs another source file.
- **Idle affinity.** `configIDLE_AFFINITY` is 1, so idle task *n* runs only on
  core *n*. The active idle task, which reclaims tasks that deleted
  themselves, therefore runs on core 0. The tick wakes core 0 every
  millisecond, so a self-deleted task is reclaimed within one tick of core 0
  becoming idle. Without the affinity, the active idle task could sleep on a
  secondary core that receives no interrupt, and reclamation would wait for
  one. No current image deletes a task.
- **Time slice.** The pinned kernel's tick sets a yield request for every core
  whose current priority has more than one ready task. The idle-priority list
  always holds all eight idle tasks, so each tick sent SGI0 to every secondary
  core that ran its idle task. The patch `patches/freertos-smp-tasks.patch`
  changes the threshold for idle priority to `configNUMBER_OF_CORES`, the test
  that `prvIdleTask` applies before it yields. A core at idle priority still
  shares its time slice whenever an application task of idle priority is
  ready, because the list then exceeds eight entries. Round-robin among tasks
  of any higher priority is unchanged.
- **EL0 WFI.** Idle tasks run at EL0. `platform/a733-smp-cached/continue.S`
  sets `SCTLR_EL1.nTWI` (bit 16) beside `nTWE`; otherwise `WFI` at EL0 traps
  to EL1 with exception class `0x01` (Arm DDI0616A.a, section E3.2).
  `SCR_EL3.TWI` and `HCR_EL2.TWI` are already 0.

The image keeps every safety property of the cached image. No EL3 code
changes; the EL3 vectors still treat every SMC as a terminal report. `WFI`
does not change interrupt masking, GIC state or redistributor sleep state, and
the core keeps its caches powered.

## Measurements

The check [cpu-power-states.py](../checks/cpu-power-states.py) builds the
cached diagnostic image with a workload in which one task per secondary core
blocks on a notification and the core 0 task sleeps for 64 ticks. It executes
the image in the instruction model of
[a733-smp-cached.py](../checks/a733-smp-cached.py), where `WFI` stops a core
until an interrupt is pending for it. After the 64-tick window, a notification
from core 0 and then a two-tick delay must each wake every secondary task on
its own core.

| Metric | Before | After | Budget |
| --- | --- | --- | --- |
| SGIs to idle secondaries per tick, no ready task | 4.5 (295 in 66 ticks) | 0 (0 in 64 ticks) | 0 |
| Instructions per idle secondary in the window | 329,000 to 598,000 (spin) | 0 | 0 |
| Instructions on core 0 in the window | 463,508 | 70,551 (about 1,100 per tick) | not budgeted |
| Core 0 timer wake-ups per second | 1,000 | 1,000 | 1,000 |
| Instructions per idle-loop iteration | 3 passive, 4 active | 5 passive, 6 active (`DSB SY`, `WFI`) | at most 6 more |
| Instructions in `xTaskIncrementTick` | 241 | 265 | one list-length compare |

Before is the integration base without the image change; after is this tree
with the image change applied. In the before image the core 0 task wrote the
end marker two ticks late, so the window spans 66 ticks there. The before
count is below the seven SGIs per tick that the kernel code requests. The
kernel sends no SGI to a core whose previous yield request is still pending,
and in the model some spinning secondaries handle that request only after the
next tick. The active idle loop also calls the kernel's reclamation
function, which the counts exclude.

The `xTaskIncrementTick` budget is exceeded in instruction count. The compiler
unrolls the time-slice loop over the eight cores. Each core still has one
list-length compare and gains three register instructions, `CMP`, `MOV` and
`CSINC`, that select the threshold; no memory access is added. These 24
instructions per tick replace, in an idle system, seven SGIs and seven context
switches per tick. The integrated OS image and the mimus board image show the
same function sizes as the cached diagnostic.

The check fails when the image omits `nTWI`: the first EL0 `WFI`
stops the run. An image with the idle hook but without the time-slice patch
still sends time-slice SGIs to every idle core. In the model, cores woken
together on every tick then contend for the kernel lock, and one secondary
task missed its eight-tick completion bound. Both parts must therefore be
applied together.

## Running the check

```sh
PATH=$HOME/.cache/sunstoneos/host/usr/lib/llvm-21/bin:$PATH \
PYTHONDONTWRITEBYTECODE=1 ~/.cache/sunstoneos/venv/bin/python checks/cpu-power-states.py \
  --evidence <new directory> --freertos <pinned FreeRTOS-Kernel> [--patch <image change>]
```

`--source` builds another tree, such as the integration base, and
`--record-only` reports the metrics without applying the budgets. The result
is written to `result.json` in the evidence directory.

## Limits and board evidence

The instruction model has no A733 power, cache or interconnect timing. It
shows which interrupts the scheduler sends and which instructions each core
executes; it does not show that a physical core wakes from `WFI`. A board
session must show EL0 `WFI` with `nTWI` on all eight cores under the
scheduler, woken by SGI0 and by PPI30, with no time-slice SGIs to idle cores.

Core power-off and power-on through EL3, core retention, cluster or DSU
power-down and tickless idle are not implemented. The design records why and
the conditions under which core power-off returns.
