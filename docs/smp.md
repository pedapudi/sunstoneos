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
EL1 for interrupt masks, core identification, and context switches. The kernel
has a 1 kHz tick on core zero. Software-generated interrupt zero requests a
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
