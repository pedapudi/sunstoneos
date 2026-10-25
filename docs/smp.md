# Two-core FreeRTOS scheduling in QEMU

The separate SMP target runs FreeRTOS tasks on two emulated Cortex-A55 cores.
SMP means that both cores share one kernel and schedule tasks concurrently.
This target is **tested in emulation**. Physical A733 secondary-core startup,
cache coherence, interrupt routing, and recovery remain untested.

The target adapts the non-MPU path of the pinned
[FreeRTOS Cortex-R82 port](https://github.com/FreeRTOS/FreeRTOS-Kernel/tree/97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2/portable/GCC/ARM_CR82).
The dependency revision and archive hash are in
[sources.lock.json](../sources.lock.json). CMake applies the project patches
to cached copies; the source archive and its license notices remain intact.
The target has separate startup, interrupt setup, configuration, and linker
files under [platform/qemu-smp](../platform/qemu-smp).
It must be selected explicitly when building.

## Startup and exception handling

The primary core builds shared translation tables and initializes the GICv3
interrupt distributor. QEMU's PSCI CPU_ON service starts the second core through
HVC with `virtualization=off`. Each core installs its own exception stack,
enables its interrupt interface, and records a logical index in `TPIDR_EL1`.
Both cores complete a startup handshake before running tasks.

Tasks execute at EL0, the unprivileged processor level. Supervisor calls enter
EL1 for interrupt masks and context switches. The core index and the current
task come from `TPIDRRO_EL0`, which EL0 can read and only EL1 writes, as
described under [port costs](#port-operations-avoid-supervisor-calls-and-shared-lines).
The kernel has a 1 kHz tick on core zero. Software-generated interrupt zero requests a
context switch on the other core. The current target assumes affinity indices
zero and one in one group.

Code occupies a read-only, executable mapping at `0x41000000`; writable data
starts at `0x41200000` and cannot execute. Peripheral space uses device memory
attributes. RAM uses shared, cacheable mappings over 128 MiB.
Tasks can access all mapped data and peripherals, including kernel state.
These mappings provide no task isolation.
The separate [NPU service target](npu-service.md) grants EL1t execution to one
statically configured driver worker while application tasks remain at EL0.
Its focused check covers worker preemption and migration with full register
retention. The original SMP targets retain their port configuration.
Arm requires privileged code to avoid memory writable at EL0; the separate
code mapping satisfies that execution rule.
See [Arm's memory-protection guide](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/Providing%20protection%20for%20complex%20software.pdf).

The port patches make task interrupt controls use the same GIC priority mask.
This lets a pending remote yield interrupt run when the scheduler temporarily
enables interrupts while acquiring a lock. Supervisor-call handlers keep IRQs
masked until exception return, preventing a task switch from saving a partially
completed supervisor call as task context.
The patches also preserve the yield-flag address while checking critical-section
nesting, complete shared writes before wake events, and supply a hardware memory
barrier for scheduler operations. Core indexing uses the platform's logical index.

## Port operations avoid supervisor calls and shared lines

The port patches make four changes that remove work which protects no
shared data:

- **Current task in `TPIDRRO_EL0`.** The context-restore path writes the
  resumed task's TCB address to bits [47:0], the core index to bits [55:48]
  and, in bit 56, whether the task runs at EL1. Task selection writes the
  register when it changes the core's current task, so the register always
  matches `pxCurrentTCBs` for that core. `ucPortGetCoreID` and
  `xTaskGetCurrentTaskHandle` read it with one `MRS`. A task's own handle does
  not change when it migrates, so the read needs no interrupt mask.
- **Zero means not yet resumed.** The register is UNKNOWN at reset. The entry
  code of the cached images (`platform/a733-smp-cached/continue.S`) and the
  eight-core diagnostic (`a733_smp_pre_scheduler`) clears it before any kernel
  call; QEMU and the instruction models reset it to zero. While the TCB field
  is zero, both functions use the supervisor-call path, so code that runs
  before the first task, and emulators that start an image part-way, behave as
  before.
- **EL1 tasks mask in line.** `portDISABLE_INTERRUPTS`, `portENABLE_INTERRUPTS`,
  `uxPortSetInterruptMask` and `vPortClearInterruptMask` test bit 56. For an
  EL1 task they write `ICC_PMR_EL1` followed by `DSB SY` and `ISB`, the same
  sequence as the supervisor calls, and leave `DAIF` unchanged. EL0 tasks keep
  the supervisor calls; an EL0 task cannot reach the inline write because the
  restore path clears bit 56 for it.
- **Fewer masks and locks in interrupts, and one line per core.** The SGI
  handler only sets its own core's yield flag with `DAIF.I` still set from
  exception entry; the IRQ exit path takes both kernel locks when it switches
  context. The tick handler masks once, through its ISR critical section.
  Each core's FPU-context flag and lock-ownership bits share one 64-byte line
  (`xPortCoreStates`), and each lock word shares a line only with its own
  recursion count (`xPortLocks`). `ullCriticalNestings`,
  `ullPortInterruptNestings` and `ullPortYieldRequired` keep their names and
  8-byte stride because drivers and checks outside the port read them.

The lock gate keeps its `DMB SY` after a successful trylock and its `DMB SY`,
`DSB SY`, `SEV` and `ISB` around the release store. The recursion count and
ownership bits that only the holder or its core touches are read and written
without `DSB`, and the IRQ exit, wake placement and late ticks have lighter
paths. These four changes are implemented and checked in emulation only; they
merge after the board session in
[the multicore performance runbook](board-runbooks/multicore-performance-session.md),
and the [multicore performance design](designs/multicore-performance-1025.md)
gives their references and measurements. Weaker acquire and release ordering
of the gate, and `PMR` writes without `DSB`, wait for a supervised eight-core
board session, as [the design](designs/smp-heterogeneous.md) requires.

### Measured cost

[checks/smp-port-cost.py](../checks/smp-port-cost.py) builds a two-core QEMU
image with the port, task and privileged-task patches and counts
`INST_RETIRED` under `-icount shift=0`, where the count is exact. Idle tasks
wait in `WFI`, so an idle core retires almost no instructions. Each figure is
the least of 16 samples, minus an empty region; the round trip is the mean of
256. Cortex-A55 and Cortex-A76 models give identical counts. The base tree is
the integration branch before these changes.

| Operation | Base | Changed |
| --- | --- | --- |
| Critical-section pair, EL1 task | 487 | 296 |
| Critical-section pair, EL0 task | 487 | 368 |
| Current-task read, EL1 or EL0 task | 170 | 11 |
| Notification given to the caller and taken | 1,644 | 656 |
| Notification that wakes a higher-priority task on the same core, to its first instruction | 2,779 | 1,320 |
| Cross-core notification round trip, instructions on both cores | 29,236 | 12,191 |

The check fails if any operation retires more instructions than in the base
result, or if the round trip falls by less than 30%; it fell by 58%. The
design's targets of 100 instructions for the EL1 pair, 180 for the EL0 pair and
800 for the switch are not met. About 170 of the 296 instructions of the EL1
pair are the four calls to `vPortRecursiveLock`, which keep their `configASSERT`
checks and barriers.

The cached eight-core instruction model, run with `--count`, gives the median
per call over its scheduler run:

| Function | Base: instructions, SVC, DSB, ISB | Changed |
| --- | --- | --- |
| `ucPortGetCoreID` | 121, 1, 1, 1 | 5, 0, 0, 0 |
| `FreeRTOS_SGI_Handler` | 479, 2, 24, 12 | 15, 0, 0, 0 |
| `FreeRTOS_Tick_Handler` | 2,071, 12, 36, 23 | 1,667, 7, 32, 15 |
| `vTaskSwitchContext` | 1,747, 10, 47, 24 | 305, 0, 9, 0 |
| `vTaskExitCritical`, EL0 task | 373, 2, 19, 5 | 255, 1, 18, 4 |

The seven supervisor calls that this table still shows in the tick were
`SVC 117`, which sends the yield SGI to other cores; the next section removes
them. In this image every task runs at EL0, so the remaining
critical-section calls use the supervisor path for the mask.

```sh
cache="$HOME/.cache/sunstoneos"
python3 checks/smp-port-cost.py --output "$cache/evidence/smp-port-cost-base" \
  --root <base source tree>
python3 checks/smp-port-cost.py --output "$cache/evidence/smp-port-cost" \
  --baseline "$cache/evidence/smp-port-cost-base/result.json"
python3 checks/a733-smp-cached.py --evidence "$cache/evidence/smp-cached-count" \
  --image <a733-smp-cached-diagnostic.elf> --count
```

The counts are emulator evidence. QEMU models no cache-line transfer between
cores, so it shows neither the cost of a nesting word written by another core
nor the benefit of the per-core lines; only a board session can measure those.

### SGIs leave EL1 code without a supervisor call

`vInterruptCore` reads `TPIDRRO_EL0` once. When the register names a resumed
task and either bit 56 is set (an EL1 task, or a handler that interrupted
one) or the interrupt-nesting count of the core in bits [55:48] is nonzero (a
handler that interrupted an EL0 task), it writes `ICC_SGI1R_EL1` after
`DSB ISHST`. The EL1-task form ends with `ISB`, as Linux `gic_ipi_send_mask`
does; a handler's exception return synchronizes the context instead. EL0
tasks, and a core that has not resumed a task, keep `SVC 117`, because the
register is UNDEFINED at EL0 (IHI 0069G §12.2.21). The kernel calls the
function from task context inside its critical section, so the task cannot
migrate between the register read and the nesting load, and an EL0 task reads
a zero count because no handler is active on a core that runs a task. The
target value comes from `portSGI1R_VALUE`, which defaults to the upstream
Aff0 formula; the A733 configurations define it in line as `(core << 16) | 1`,
the Aff1 core number with target-list bit 0, after asserting SGI 0.

The tick no longer asks a core to give up its time slice merely because its
priority's ready list holds another task. Above the idle priority a core
yields only to a ready task that is not running (`taskTASK_NOT_RUNNING`) and
admits that core; the union of those tasks' affinity masks is computed once
per distinct running priority per tick. The idle priority keeps its length
test, so idle tasks without per-core affinity behave as before.

Measured with the cached eight-core instruction model on the diagnostic built
from the base tree and from this tree (2026-10-24). `checks/a733-smp-cached.py
--count` reports, in `tick_by_wakes`, the ticks that wake no task (no
`prvYieldForTask` entry), the ticks taken while the scheduler is suspended,
which only pend the tick, and the ticks that wake tasks, whose SGIs are
required preemptions:

| Metric | Base | Changed |
| --- | --- | --- |
| Ticks that wake no task: count, median and largest instructions, median `SVC`, `DSB`, SGI sends | 16, 1,345, 1,667, 5, 28, 5 | 9, 554, 590, 0, 18, 0 |
| Pended ticks: count, median instructions | 15, 229 | 8, 231 |
| Ticks that wake tasks: count, SGI sends over the run | 36, 246 | 47, 177 |
| `vInterruptCore` median: instructions, `SVC`, `DSB`, `ISB` | 161, 1, 2, 1 | 18, 0, 1, 0 |
| SGI handler calls; `vTaskSwitchContext` calls over the run | 407; 708 | 185; 480 |

On a tick that wakes nothing the base sent five time-slice SGIs to cores
whose same-priority peers are pinned elsewhere; the changed tick sends none.
Skipping the scan when a priority's list is no longer than the cores running
it was tried and left out: the zero-wake median rose to 618 instructions in
that run. [checks/smp-sgi-send.py](../checks/smp-sgi-send.py)
runs the linked `vInterruptCore` under Unicorn: an EL0 task while another
core is in a handler takes `SVC 117`, a handler that interrupted an EL0 task
writes the register without `ISB`, an EL1 task writes it with `ISB`, and a
core without a resumed task takes the supervisor call. The base image fails
the second case.

In QEMU (`checks/smp-port-cost.py`), the EL1 cross-core round trip falls from
12,191 to 11,994 instructions and every other operation is unchanged. The new
EL0 round trip rises from 14,127 to 14,169 instructions: the EL0 send path is
5 instructions longer, and QEMU's lock-wait interleaving amplifies a longer
send, as a control build with five no-ops in the base send path shows (EL0
round trip 14,147, EL1 12,240). The check allows 24 instructions per EL0 send.

### One cached base builds every cached eight-core image

The observe and supervised images take the cached diagnostic's source list and
its patched kernel, and replace only the boot, vector, diagnostic and
admission files (`a733_smp_observer` in
[cmake/a733-smp-observe.cmake](../cmake/a733-smp-observe.cmake)). The build
patches one kernel instead of three, and the three CMake files have 200 lines
instead of 325. Built from the same sources before the port changes, each
image's instructions matched the previous build. Outside the 32 bytes of
`a733_smp_build_id`, the cached image is byte-identical; the observe and
supervised images differ in 433 and 1,018 bytes, all in `configASSERT`
file-name strings, whose kernel copy path now names the cached build, and in
the addresses that point at them.

The port changes alter every cached-family build identity, so board evidence
stays bound to the identity it ran:

| Image | Identity before these changes | Identity with them |
| --- | --- | --- |
| Cached diagnostic | `e377251e3653dc4d48a9066d26ae552aa2c3e6abbc7e9ed3d57793a3630acf1d` | `a84a36befd4d61e29e911b683faeb1e7f322048e120f23ad392fe10282237027` |
| USB observer | `144491f5679836f651aabe0a84e91cee41e9057418266dfc89a96aa3dcd6b4f0` | `2c7773bfd399465a3def5f795bd45944f069913a4d7a9ab3f13e877cd4625458` |
| Supervised observer | `83ea52a2b4a287cf53fd9a6e1fea572fba6236ee999ebb2360dbbd42a8b63206` | `fe89501ef8801cbd9d69aab3b1c6902e3d4ab9f2db64cd0b5b38c6b2d050f2aa` |

## Concurrency evidence

The focused check uses four statically allocated tasks. A producer on core zero
and a consumer on core one exchange 256 messages of 256 bytes. Each checks its
core affinity and retains a distinct value in floating-point register `d8`
across a blocking delay. Each core also runs a lower-priority background task
with a different `d8` value and no voluntary delay.

Both background tasks update a shared count and 32 matching words under a mutex.
Every reader verifies the words before updating them. Completion requires ordered,
intact messages, preserved register values, at least 256 ticks, and progress from
both background tasks. Assertions, exception faults, emulator errors, and a
ten-second host timeout fail the check.

Three bounded trials passed on 2026-10-16. The three-trial batch investigated an
intermittent stall found during port adaptation; it is not a default requirement
for each change. The final ELF SHA-256 is
`3ed2e5e54051046c85f8c176350631e6f9229aa1670aa82fe93681474d456c0c`.
Its sections contain 30,400 bytes of code/read-only data and 150,880 bytes of
reserved RAM, including task stacks, exception stacks, and translation tables.
Raw results and source/toolchain identities are in
`~/.cache/sunstoneos/evidence/smp-20261016/{final,manifest}.json`.

After configuring the existing [build environment](build.md):

```sh
cache="$HOME/.cache/sunstoneos"
cmake --build "$cache/build/qemu-virt" --target smp-kernel-model -j 8
python3 tools/check-smp.py "$cache/build/qemu-virt/smp-kernel-model.elf" \
  --qemu "$cache/host/usr/bin/qemu-system-aarch64"
```

The checker defaults to one trial and reuses successful evidence when its inputs
match. The recorded investigation used `--trials 3` and an explicit evidence path.
It stops at the first failure. No board access or mimus rerun was needed for this
separate target.

## Register state survives task migration

The separate `smp-context-model` check moves two running tasks between cores
for 32 rounds. Each task loads distinct patterns into general registers
`x0`–`x30` and all 32 vector registers. It also sets the condition flags, all
four floating-point rounding modes across successive rounds, and floating-point
status flags. The assembly check records its stack position before switching.

A controller changes each task's affinity to the other core while the patterns
are live. The assembly loop reports its current core through a supervisor call.
The controller waits until both tasks report their destinations before releasing
them to capture and compare their state. Temporary loop values preserve the
register patterns and condition flags on the task's stack.
A yield supervisor call also runs with all register patterns loaded.

One bounded session passed all 64 migrations on 2026-10-16. Comparisons cover
`x0`–`x30`, the stack pointer, `q0`–`q31`, condition flags in `NZCV`, rounding
control in `FPCR`, and status flags in `FPSR`. This is functional emulator
evidence for those state values. It does not exercise every interrupt priority
or nesting combination, compiler thread-local storage, or physical coherence.
No port changes were needed for this check.

The ELF SHA-256 is
`305e611d82e79e34dd23fbbf06057565dcb6dc80c9473cc9fa3e38b7ea5f5547`.
Its sections contain 25,720 bytes of code/read-only data and 181,936 bytes of
reserved RAM. The image, successful output, and dependency manifest are under
`~/.cache/sunstoneos/evidence/smp-context-20261016/`.
The result file is `initial.json`; `manifest.json` records the compiled inputs.
The earlier queue/mutex evidence remains applicable to its unchanged image.

```sh
cache="$HOME/.cache/sunstoneos"
cmake --build "$cache/build/qemu-virt" --target smp-context-model -j 8
python3 tools/check-smp.py "$cache/build/qemu-virt/smp-context-model.elf" \
  --case context --qemu "$cache/host/usr/bin/qemu-system-aarch64" \
  --evidence "$cache/evidence/smp-context-check.json"
```

## Eight emulated cores pass a scheduling soak

The `smp8-soak-model` target builds the same port, platform and kernel sources
with `SUNSTONE_SMP_CORES=8`. The platform then starts seven secondaries through
PSCI and reserves eight exception stacks. The two-core targets are unchanged.

The [soak check](../checks/smp8-soak.c) runs four kinds of work at once:

- A token makes 64 laps of a ring of eight queues, one receiving task pinned to
  each core. Every hop wakes a task on the next core through a queue operation
  and its interprocessor interrupt. Each task checks the hop count it receives.
- Every ring hop and every mover iteration updates 16 shared words under one
  mutex and checks that no other core changed them mid-update.
- Two tasks migrate through all eight cores in 32 rounds. The control task sets
  each new affinity and waits, with a one-second bound, until the task reports
  running on that core. Each visit holds a callee-saved FP register across a
  blocking delay.
- A lowest-priority task pinned to each core never yields and checks its own FP
  register and core. The check requires progress from all eight, so the tick and
  interprocessor interrupts must preempt every core.

```sh
cache="$HOME/.cache/sunstoneos"
cmake --build "$cache/build/qemu-virt" --target smp8-soak-model -j 8
python3 tools/check-smp.py "$cache/build/qemu-virt/smp8-soak-model.elf" \
  --case soak8 --trials 10 --qemu "$cache/host/usr/bin/qemu-system-aarch64" \
  --evidence "$cache/evidence/smp8-soak.json"
```

Ten consecutive trials passed on 2026-10-20. With the port changes above, ten
trials of this soak and of the queue and migration checks passed on 2026-10-22
with `--cpu cortex-a55` and with `--cpu cortex-a76`. QEMU runs identical cores
of the selected model with sequentially consistent memory, so the soak
establishes kernel and port behavior with eight participants, not A733 timing,
differences between A55 and A76 cores, or physical coherence. It uses the QEMU
platform, not the A733 cached platform.

## Requirements before application and physical integration

The emulator's CPU_ON service does not implement the A733 core power, reset and
affinity operations. Those are implemented natively: all eight physical cores
reach their first FreeRTOS task context, pass the production barrier and 256
shared atomic increments, and shut down to independently observed OFF. Task
bodies, timer preemption on every core and sustained workloads remain modeled
only. [Supervised startup](a733-smp-supervised.md) records the physical
checkpoints; the [reference audit](a733-multicore-reference-audit.md) records the
architectural decisions.

The separate [SMP C/C++ runtime](smp-runtime.md) supplies task-local library
state and passes concurrent allocation and shared-stream checks in QEMU.
[Mimus now passes reference parity](mimus-smp.md) with that library and a second
inference worker.
The original single-core library remains unsuitable for concurrent use.

Further checks must cover interrupt priorities and nesting, all eight physical
cores, and concurrent device activity. QEMU does not establish physical cache
coherence, weak-memory behavior, performance, or power transitions. Mimus still
uses the physical single-core runtime; physical multicore recognition remains
outstanding.
