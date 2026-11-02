# Host check shim

# Host check shim

Status: **implemented**. This is host-only test infrastructure. It runs the unmodified FreeRTOS kernel and SunstoneOS compilation units on the build machine with virtual time, and no board is involved, so it qualifies nothing on a board.

## Purpose

Many host checks in `checks/` replaced the RTOS under the code they test. A check wrote its own `FreeRTOS.h`, `task.h` and `queue.h`, or defined `xTaskGetCurrentTaskHandle` as a function returning a constant, or copied functions out of a driver by regular expression and compiled the copy. Such a check passes or fails on the check's own assumptions. Two faults escaped for that reason:

- `checks/deadline-host.c` used a fake session with a nonzero deadline. The board's sealed session has zero time fields, and the service wait certificate compared against that value, so every application wait was refused on the board. The host check passed.
- The CPU and NPU concurrency fixture stubbed the thermal throttle flag to zero. The OS reports throttled until the first complete sensor set, so the first worker faulted on the board. The host check passed.

The same checks also break when the code is refactored, because an extracted function or a hand-written stub stops matching the source.

## Survey

The survey reads every file in `checks/` at `a96c8c1a`. A check counts as "own RTOS" when it writes its own FreeRTOS headers or defines kernel functions (task handle, delay, tick, queue, notification). It counts as "extracts text" when it slices source text by pattern and compiles or executes the slice. A third pattern, stubbing a value that the code under test depends on, cannot be found by pattern; the instances below were found by reading.

| Pattern | Checks |
|---|---|
| Own RTOS headers or kernel functions | 62 |
| Extracts or patches source text | 50 (many of them are lint checks of source text that stay) |
| Stubs a value the code depends on | at least 4 (named below) |

Ranking by edits in the 7 days from 2026-10-25 (`git log --since=2026-10-25 --name-only -- checks/`, summed over the check's files), for the checks that fit the host shim:

| Check | Edits | Pattern | Disposition |
|---|---|---|---|
| `usb-host-deadline` | 6 | Frozen copy of `runtime/deadline.c`, own headers | Converted |
| `gpu-texture-frame` | 6 | Extracts six scheduler functions by regular expression | Converted |
| `gpu-clients` | 6 | Own headers, queue and task stubs | Converted |
| `usb-keyboard-client` | 5 | Extracts client functions by regular expression | Converted |
| `queue-add` | 5 | Writes `FreeRTOS.h` and `task.h` inline | Converted |
| `deadline-host` | 3 | Own task and notification layer, nonzero session deadline | Converted |
| `reset-latency` | 3 | Fake task handle and deadline calls, unbounded board deadline | Converted |
| `gpu-job-timing` | 3 | Regular expression replaced `ticks()` in the source | Converted |
| `os-thermal-cadence` | 3 | No-op critical sections, constant task handle | Converted |
| `usb-host-ready-work` | 2 | Own critical section, scheduler and notification stubs | Converted |
| `os-gpu-thermal` | 2 | No-op critical sections, constant task handle | Converted |
| `usb-host-irq` | 1 | Own stubs; no driver script, so it never ran | Converted and run from `usb-host-ready-work.py` |
| `sunstone-app-runtime` | 3 | Did not compile; no gate or index entry | Retired |
| `camera-application`, `camera-application-output` | 3, 2 | Two RTOS stubs in an AArch64 image under Unicorn | Later candidate |

Values stubbed to a convenient number, found by reading:

- `deadline-host`: the sealed session deadline (fixed in the previous commit; the shim keeps the zero value and supplies the runtime deadline through `board_runtime_deadline_us()`).
- `reset-latency`: the runtime deadline was `UINT64_MAX / 24`, which the supervisor can never reach. It is now the board's order of magnitude (one hour).
- `gpu-clients`: the worker task handle was the integer 4 and the scheduler state was a variable.
- The CPU and NPU concurrency fixture: the throttle flag (owned by the NPU qualification stream).

Not converted because other streams own them: `checks/npu-*` (for example `npu-startup-host`, 8 edits; `npu-latency-client`, 6), `checks/cpu-npu-*`, and the scheduler and lock checks. Not converted because the fixture is an AArch64 image under Unicorn or QEMU, which cannot link the x86 host port: `camera-application`, `camera-application-output`, the `os-npu-*` image fixtures, `bluetooth-module-owner`, `bluetooth-headset`, `usb-host-execution`, `isp0-irq`, `dsi-scanout-native`. `pvr-gpu-service` is not a candidate: it already runs the real SMP kernel under QEMU.

## Design

### Real kernel on a host port

The shim links `tasks.c`, `queue.c` and `list.c` of the pinned FreeRTOS kernel (`sources.lock.json`) unmodified. It supplies a port (`checks/host/portmacro.h`, `checks/host/host_rtos.c`) and a configuration (`checks/host/FreeRTOSConfig.h`) that matches the board configuration in every setting that changes scheduling: preemption, time slicing, the 1 kHz tick, eight priorities, 64-bit ticks and base types, static allocation, and two notification indexes.

The shim does not use the kernel's POSIX port. That port runs tasks in pthreads with signal-driven ticks, so its interleavings and time depend on the host scheduler. The shim instead runs each task as a user-level context (`makecontext`) and switches only where the kernel switches. A hand-written model of tasks and queues was rejected because it would restate the kernel's behavior and would drift from it. The kernel's SMP patches (`patches/freertos-smp-*.patch`) are not applied: the host has one core, and the unpatched kernel is the one that the board's single-core images link. Core affinity is outside the shim; code that needs it is checked where it already runs.

### Deterministic time and interrupts

- One virtual clock in microseconds. The tick count follows it at 1 ms. The A733 counter reads clock times 24 (`a733_smp_counter()`).
- Time moves only when the running task calls `host_advance_us()`, or when every task is blocked and the idle task steps to the next tick. Ticks and the compare timer fall due at their exact times, so a task that advances time is preempted at the right instant.
- Interrupts are scripted: `host_irq_at(us, handler, argument)` runs a handler at an exact time, and `host_run_at()` runs a function in a highest-priority script task. Interrupts due at the same time all run before the woken task does. A critical section or interrupt mask defers delivery to its exit.
- Each task keeps its own critical nesting and mask across a switch, as the board's port saves them with the task.
- `sunstone_deadline_now_us`, `sunstone_deadline_program` and `sunstone_deadline_kick` are provided by `checks/host/host_deadline.c`, linked only with `runtime/deadline.c`. `host_timer_hold_until()` models a late or lost alarm. `host_deadline_defer_kicks()` models a cross-core rearm that arrives later.
- `host_server_create()` and `host_call()` run a call in another task, so code that identifies its caller by task handle sees a different task. They exchange through queues only, so they never disturb notification indexes.
- Sanitizers: the kernel, the port and the fixture build with the flags of `checks/harness.py`. Context switches carry the AddressSanitizer fiber annotations. AddressSanitizer prints a one-line warning about `swapcontext` on every run; the warning is expected. ThreadSanitizer cannot run the context switches and is not supported.
- One scheduler session runs per process. A fixture puts its scenarios in the test task, or forks one process per scenario.

### Build

`checks/host_rtos.py` compiles a fixture with the kernel, the port and the sanitizer flags, and caches the kernel and port objects under `~/.cache/sunstoneos/build/host-shim-objects/` keyed by the full command, the source and the shim headers. `host_rtos.mutated()` writes a copy of a source with one exact text replaced, for negative controls.

## Converted checks

Each converted check links the real compilation unit of the code it tests and runs a negative control by default: the same fixture, built against the code with the guarded bug reintroduced, must fail.

| Check | Real units | Negative control (fails as required) |
|---|---|---|
| `deadline.py` / `deadline-host.c` | `runtime/deadline.c`, `apps/os/services.c` with the zero sealed deadline | The wait certificate bound read from the sealed session; no wake time on a notified return; waking every due owner instead of the earliest |
| `reset-latency.py` | `apps/os/services.c`, `runtime/deadline.c` | `services.c` without the pending-park word |
| `gpu-clients.py` | `drivers/gpu/gpu_queue.c`, `apps/os/gpu-share.c` | The historical `gpu_queue.c` (`9f7c1b00`, reversible close); a foreign task joining as the peer |
| `gpu-texture-frame.py` | `drivers/gpu/pvr_gpu_service.c` compiled whole, render and texture units | Three scheduling mutants with `--verify-mutations` (burst overrun, task starvation, submission delay) |
| `usb-host-deadline.py` | `runtime/deadline.c`, `apps/board/host_irq.c` | Burst bound raised by one; retry rounded up to 5 ms |
| `usb-host-ready-work.py` (and `usb-host-irq.c`) | USB host ready-work source, host IRQ source | The original wait condition; claiming a pending IRQ without masking the source |
| `usb-keyboard-client.py` | `apps/os/client.c` compiled whole | Dropping the reader-claim test |
| `queue-add.py` | `apps/queue-add/main.c` | Removing the session-live test in the wait loop |
| `gpu-job-timing.py` | `apps/os/gpu-timing.c` | A receive that clears the blocked-IRQ pairing |
| `os-gpu-thermal.py` | `apps/os/gpu.c` | A close that ignores clients |
| `os-thermal-cadence.py` | thermal service source | A 25 ms phase advance replaced by a fixed time |

Coverage notes:

- `deadline-host` keeps every case of the earlier fixture. The simultaneous-owner case now also checks the wake order of equal-priority owners. The case in which a stale timer notification races cancellation is not reproducible on one core: the second `ulTaskNotifyTake` after cancellation guards a timer interrupt on another core. The check keeps the observable (no pending notification after the wait) and drops the race itself. The counts of `wait_calls` and `programs` are dropped because they restated the implementation.
- `gpu-texture-frame` no longer extracts the scheduler functions. The worker, burst, wait computation and CPU-run block are the image's code, so the probe counts changed: scheduling yields 20 to 19 in the second probe and CPU blocks 5 to 6 in the third, and the per-frame instruction counts rise by 0.1 to 0.2 percent. The earlier fixture replaced the wait computation with a constant; the real one blocks for one tick when a step makes no progress.
- `usb-keyboard-client` drops its `--thread-sanitizer` option because ThreadSanitizer cannot run the kernel's context switches. Nothing in the repository used it.
- `os-gpu-thermal` changes one expectation: when the model reports the queue busy, `apps/os/gpu.c` retries the close every tick, so the case asserts more than one close where it asserted exactly one. The earlier check did not link on this tree, so the old assertion was never confirmed against the current code. This needs review by the GPU owner.
- `apps/os/gpu-timing.c` gains a host seam: on a target other than AArch64 the counter read and barriers use the shim's clock and compiler barriers. The AArch64 instructions are unchanged.
- `a733-gpu-isolation.py` and `gpu-application.py` patched the source text of `a733-gpu-backend-emulate.py` and executed it. The pattern they replaced no longer existed, so they could not run. The emulator now has `--isolation-only` and `--application-only`, and the two scripts select them. Both still need the emulator ELF argument and were not run here.

## Retired checks

| Check | Reason |
|---|---|
| `sunstone-app-runtime.py`, `.c` | Did not compile (it built `apps/os/application.c`, which has AArch64 inline assembly, for the host); no gate or index entry referenced it, so it protected nothing |
| `os-image-stage2.py` | Compiles `checks/os-image-model.c`, which moved to mimus in `797103c2`; it cannot run |
| `os-npu-resident.py` | Loads `checks/os-npu.py`, which moved to mimus in `797103c2`; it cannot run |

No check was retired for asserting constants only. Candidate checks that read source text and assert patterns (for example `pmic-tree.py`, which refuses voltage writes from unlisted sources) enforce a policy that the code does not state about itself, and `storage-read-returning-host.py` exercises a tool. They stay.

## Later candidates

Checks that stub the RTOS or extract text and are not converted, with the reason:

- `camera-application`, `camera-application-output`, `os-npu-first-failure`, `os-npu-sdk-report`, `os-npu-perf-mailbox`, `os-npu-health-events`, `os-npu-sdk-events`, `bluetooth-module-owner`, `bluetooth-headset`, `usb-host-execution`, `isp0-irq`, `dsi-scanout-native`: AArch64 fixtures under Unicorn. Use the same approach as `gpu-texture-frame` (include the real source in the fixture and replace the RTOS calls at the boundary with a small freestanding hook), not the x86 shim.
- `npu-startup-host`, `npu-latency-client`, `npu-runtime-link`, `cpu-npu-*`: owned by the NPU streams. The shim can link their units once those streams settle; the throttle flag must start as the OS reports it.
- The scheduler and lock checks: owned by the scheduler and lock streams.
- Text-extracting lint checks that remain (for example `board-return-preservation`, `gpu-linked-lifecycle`, `media-minimum-holds`): they inspect source text for a policy and do not model the RTOS.

## Measurements

Measured on the build machine with sanitizers; times are wall clock for one run with a warm object cache.

| Check | Before | After | Note |
|---|---|---|---|
| `deadline.py` | 0.14 s | 1.4 s | Now includes three negative-control builds |
| `reset-latency.py` | 9.7 s | 13.4 s | 8000 parks; the added time is the real tick per millisecond step; includes a control |
| `gpu-clients.py` | 0.4 s | 1.0 s | Real kernel and control build |
| `gpu-texture-frame.py` | 13.1 s | 15.5 s | With `--verify-mutations` |
| `usb-host-deadline.py` | 0.2 s | 2.2 s | Two controls |
| `usb-host-ready-work.py` | 2.6 s | 4.5 s | Includes the host IRQ fixture and controls |
| `usb-keyboard-client.py` | 0.3 s | 1.4 s | One control |
| `queue-add.py` | 0.4 s | 4.1 s | Two kernel builds (the tick-wrap case starts the tick near the limit) |
| `gpu-job-timing.py` | 0.2 s | 1.1 s | One control |
| `os-gpu-thermal.py`, `os-thermal-cadence.py` | not runnable before | 1.7 s, 1.9 s | Neither old check ran on this tree |

Lines in `checks/` between `a96c8c1a` and this branch: 1964 added and 1525 removed. The shared shim accounts for 750 of the added lines. The converted checks and their controls add 1214 lines, and the conversions and the retired checks remove 1525, a net reduction of 311 lines outside the shim.

The run time of the converted checks rises by about 1 to 2 seconds each. Most of that is the sanitizer-instrumented link and the negative-control builds; the shared object cache removes the kernel compile.
