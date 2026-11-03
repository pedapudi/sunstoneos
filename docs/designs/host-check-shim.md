# Host check shim

Status: **implemented**. This is host-only test infrastructure. It runs the unmodified FreeRTOS kernel and SunstoneOS compilation units on the build machine with virtual time, and no board is involved, so it qualifies nothing on a board.

## Purpose

A check that replaces the RTOS under the code it tests passes or fails on the check's own assumptions. Writing its own `FreeRTOS.h`, `task.h` and `queue.h`, defining `xTaskGetCurrentTaskHandle` as a function that returns a constant, or copying functions out of a driver by regular expression and compiling the copy each hide faults. Two faults escaped host checks that way:

- `checks/deadline-host.c` used a fake session with a nonzero deadline. The board's sealed session has zero time fields, and the service wait certificate compared against that value, so every application wait was refused on the board.
- The CPU and NPU concurrency fixture stubbed the thermal throttle flag to zero. The OS reports throttled until the first complete sensor set, so the first worker faulted on the board.

Such checks also break when the code is refactored, because an extracted function or a hand-written stub stops matching the source. The shim links the real kernel and the real compilation units, so a check exercises the code the image runs. The survey of checks by pattern is in `docs/evidence/host-check-shim.jsonl` (record `host-check-shim-survey`).

## Real kernel on a host port

The shim links `tasks.c`, `queue.c` and `list.c` of the pinned FreeRTOS kernel (`sources.lock.json`) unmodified. It supplies a port (`checks/host/portmacro.h`, `checks/host/host_rtos.c`) and a configuration (`checks/host/FreeRTOSConfig.h`) that matches the board configuration in every setting that changes scheduling: preemption, time slicing, the 1 kHz tick, eight priorities, 64-bit ticks and base types, static allocation, and two notification indexes.

The kernel's POSIX port would run tasks in pthreads with signal-driven ticks, so interleavings and time would depend on the host scheduler. The shim instead runs each task as a user-level context (`makecontext`) and switches only where the kernel switches. The kernel's SMP patches (`patches/freertos-smp-*.patch`) are not applied: the host has one core, and the unpatched kernel is the one that the board's single-core images link. Core affinity is outside the shim; code that needs it is checked where it already runs.

## Deterministic time and interrupts

- One virtual clock in microseconds. The tick count follows it at 1 ms. The A733 counter reads clock times 24 (`a733_smp_counter()`).
- Time moves only when the running task calls `host_advance_us()`, or when every task is blocked and the idle task steps to the next tick. Ticks and the compare timer fall due at their exact times, so a task that advances time is preempted at the right instant.
- Interrupts are scripted: `host_irq_at(us, handler, argument)` runs a handler at an exact time, and `host_run_at()` runs a function in a highest-priority script task. Interrupts due at the same time all run before the woken task does. A critical section or interrupt mask defers delivery to its exit.
- Each task keeps its own critical nesting and mask across a switch, as the board's port saves them with the task.
- `sunstone_deadline_now_us`, `sunstone_deadline_program` and `sunstone_deadline_kick` are provided by `checks/host/host_deadline.c`, linked only with `runtime/deadline.c`. `host_timer_hold_until()` models a late or lost alarm, and `host_deadline_defer_kicks()` a cross-core rearm that arrives later.
- `host_server_create()` and `host_call()` run a call in another task, so code that identifies its caller by task handle sees a different task. They exchange through queues only, so they never disturb notification indexes.
- Sanitizers: the kernel, the port and the fixture build with the flags of `checks/harness.py`. Context switches carry the AddressSanitizer fiber annotations; AddressSanitizer prints a one-line warning about `swapcontext` on every run. ThreadSanitizer cannot run the context switches and is not supported.
- One scheduler session runs per process. A fixture puts its scenarios in the test task, or forks one process per scenario.

## Build

`checks/host_rtos.py` compiles a fixture with the kernel, the port and the sanitizer flags, and caches the kernel and port objects under `~/.cache/sunstoneos/build/host-shim-objects/`, keyed by the full command, the source and the shim headers. `host_rtos.mutated()` writes a copy of a source with one exact text replaced, for negative controls.

## Converted checks

Each converted check links the real compilation unit of the code it tests and runs a negative control by default: the same fixture, built against the code with the guarded bug reintroduced, must fail.

| Check | Real units | Negative control (fails as required) |
| --- | --- | --- |
| `deadline.py` / `deadline-host.c` | `runtime/deadline.c`, `apps/os/services.c` with the zero sealed deadline | The wait certificate bound read from the sealed session; no wake time on a notified return; waking every due owner instead of the earliest |
| `reset-latency.py` | `apps/os/services.c`, `runtime/deadline.c` | `services.c` without the pending-park word |
| `gpu-clients.py` | `drivers/gpu/gpu_queue.c`, `apps/os/gpu-share.c` | A `gpu_queue.c` with reversible close; a foreign task joining as the peer |
| `gpu-texture-frame.py` | `drivers/gpu/pvr_gpu_service.c` compiled whole, render and texture units | Three scheduling mutants with `--verify-mutations` (burst overrun, task starvation, submission delay) |
| `usb-host-deadline.py` | `runtime/deadline.c`, `apps/board/host_irq.c` | Burst bound raised by one; retry rounded up to 5 ms |
| `usb-host-ready-work.py` (and `usb-host-irq.c`) | USB host ready-work source, host IRQ source | The original wait condition; claiming a pending IRQ without masking the source |
| `usb-keyboard-client.py` | `apps/os/client.c` compiled whole | Dropping the reader-claim test |
| `queue-add.py` | `apps/queue-add/main.c` | Removing the session-live test in the wait loop |
| `gpu-job-timing.py` | `apps/os/gpu-timing.c` | A receive that clears the blocked-IRQ pairing |
| `os-gpu-thermal.py` | `apps/os/gpu.c` | A close that ignores clients |
| `os-thermal-cadence.py` | thermal service source | A 25 ms phase advance replaced by a fixed time |

`checks/os-npu-fault.py`, `os-npu-graphs.py`, no host check (evidence: docs/evidence/npu.jsonl, docs/evidence/npu-resources.jsonl) and `npu-pool-size.py` also use the shim.

Coverage notes:

- `deadline-host` does not reproduce the race of a stale timer notification against cancellation, because the second `ulTaskNotifyTake` after cancellation guards a timer interrupt on another core. It checks the observable (no pending notification after the wait). The simultaneous-owner case also checks the wake order of equal-priority owners.
- `gpu-texture-frame` runs the image's worker, burst, wait computation and CPU-run block. The wait computation blocks for one tick when a step makes no progress.
- `os-gpu-thermal` asserts more than one close when the model reports the queue busy, because `apps/os/gpu.c` retries the close every tick. The GPU owner reviews that expectation.
- `apps/os/gpu-timing.c` has a host seam: on a target other than AArch64 the counter read and barriers use the shim's clock and compiler barriers. The AArch64 instructions are unchanged.
- The GPU emulator no host check selects its isolation and application entry points by `--isolation-only` (`checks/a733-gpu-isolation.c`) and `--application-only` (`checks/gpu-application.c`), for a run with a hand-built ELF.

## Limits and open work

The shim does not cover checks whose fixture is an AArch64 image under Unicorn or QEMU, which cannot link the x86 host port: `camera-application`, `camera-application-output`, the `os-npu-*` image fixtures, `bluetooth-module-owner`, `bluetooth-headset`, `usb-host-execution`, `isp0-irq` and `dsi-scanout-native`. They stub the RTOS at the boundary. Converting them follows the approach of `gpu-texture-frame` (include the real source in the fixture and replace the RTOS calls with a small freestanding hook), not the x86 shim. `pvr-gpu-service` already runs the real SMP kernel under QEMU.

Open work:

- Link the NPU units (`npu-startup-host`, `npu-latency-client`, `npu-runtime-link`, `cpu-npu-*`) once the NPU streams settle.
- The scheduler and lock checks belong to the scheduler and lock streams.
- Text-extracting lint checks that remain (for example `board-return-preservation`, `gpu-linked-lifecycle`, `media-minimum-holds`, `pmic-tree.py`) enforce a policy that the code does not state about itself and do not model the RTOS; they stay.

## Qualified figures

Host-only; each converted check adds about 1 to 2 seconds of run time, mostly the instrumented link and the negative-control builds. Record: `docs/evidence/host-check-shim.jsonl`, `host-check-shim-conversions` (per-check times and line counts).
