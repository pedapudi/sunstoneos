# Bounded GPU replies in supervised recognition

Status: **host-tested**. The caller bounds are implemented in `apps/os/gpu.c` and checked with modeled service replies and host admission. Physical execution, GPU drain after failure and reuse of a failed board lifetime are **open**.

Supervised recorded recognition uses the GPU service through the single job slot and completion queue of the [generic queue](../gpu-mimus-service.md). Every wait of the recognizer ends at a captured absolute deadline, so a missing reply reaches a terminal failure without depending on the resident service window or the watchdog.

## Service and caller deadlines

The service bounds ring admission by one second and starts another one-second deadline at the accepted kick. Only a timely completion, a matching timeline and completed output maintenance publish a successful record.

`apps/os/gpu.c` bounds the caller's observation separately from the service's device deadline (`runtime_window`, `runtime_live`, `runtime_observe`). OPEN and CLOSE use one-second absolute deadlines (`OS_GPU_OPEN_US`), and an accepted OPEN whose reply never arrives enters terminal retained failure. A refusal before the request enters the queue keeps the CPU-only startup.

The supervised board supervisor closes admission on failure and retains the running lifetime. It does not return to FEL or reset the device. The recognizer's `failed()` sets its stopped state, fails the board and suspends forever, which preserves the borrowed inference context and all GPU-accessible storage.

## Request lifetime

Before publishing each job, the recognizer captures one absolute deadline: the minimum of now plus two seconds (`OS_GPU_REPLY_US`), the board live-until value minus the 10 ms guard, and the session deadline minus that guard. Checked arithmetic rejects an invalid window. Queue polling, unrelated CPU progress and a later board renewal never extend the deadline.

Two seconds covers the service's two one-second stages without a third unbounded queue interval. Because the deadline starts before publication, a sufficiently delayed service may lose caller admission before its own final deadline. Such a timeout does not establish device failure or completion, and no physical timing claim follows from it.

- The caller records the pending job and deadline only for a successful submission. A refusal that reclaimed the slot permits CPU handling. A pending job prevents another submit, CLOSE, reopen or retirement.
- The completion function returns a caller outcome separate from the PVR record and writes a record only when it received one. The hook treats a protocol timeout as a distinct dispatch failure, not a fabricated LOST completion.
- Every receive blocks for at most `OS_GPU_WAIT_TICKS` (ten ticks). The caller checks board admission, monotonic counter progress and its captured deadline before and after the receive, including a successful one. Equality is late, and a late record cannot lend its sums to the engine.
- A captured FreeRTOS tick budget bounds repeated empty receives if the architectural counter stops while FreeRTOS keeps scheduling. A finite observation count (at most 4,004 calls per job and 2,004 per lifecycle operation) covers a broken queue returning while both clocks stay frozen. Counter reversal is a terminal protocol failure. No wait loop reports recognition progress.

On timeout, revocation or an invalid reply, the caller latches failure and the hook enters `os_dispatch_failed` before clearing its in-flight state or returning to the engine. It sends no CLOSE or reset, does not clear the service slot or accepted request identity, and releases no mapping, buffer, clock or rail ownership. The service collector clears its own pending ticket when it receives a reply, even if the caller finds it late. The backend's board-fatal admission gate prevents further normal device access once `board_fail` publishes failure, and a later completion cannot resume the suspended caller. The GPU may remain active; manual whole-board power removal is the recovery requirement.

OPEN and CLOSE keep their one-second windows, and their finite receive budget spans the whole operation: OPEN reply and session-ready record, or CLOSE reply and every STATUS query. Successful thermal park and reopen and verified terminal retirement use the same bounded observation rules.

These bounds assume the caller can be scheduled and bounded queue operations return. They cannot interrupt a stuck CPU, interconnect or MMIO instruction.

## Supervised composition and admission

The composition is recorded recognition with two inference workers and the GPU service. The A55 pair is cores 2 and 3, and admitted A76 is cores 6 and 7. The GPU worker runs at priority 4 on the recognition core, above the priority-3 recognizer, with INTID 95 routed to the same core. Eight-worker, NPU and performance-mode compositions are covered by their own designs ([gpu-eight-workers.md](gpu-eight-workers.md), [os-gpu-npu.md](os-gpu-npu.md), [gpu-performance.md](gpu-performance.md)). Supervised audio, radio and headset GPU combinations are unsupported.

- The linked image contains the exact-sum hook, the GPU worker and interrupt entry, firmware with the pinned digest and the placement table generated from the model assets. It keeps the writable arena at `0x46000000`–`0x46c00000` and the read-only CPU view at `0x46c00000`–`0x47000000`, and image, stack and heap extents stay disjoint.
- GPU startup excludes the sealed OS thermal owner from its fixed firmware and session owner identities, alongside the board-owner exclusions, before constructing providers.
- A retained configuration record uses the native deadline, priority and core constants. The link checker reads it with the embedded firmware and placement bytes, and preparation and frozen-entry validation compare it with the ELF and reviewed entry. An image without the bounded-wait capability cannot be admitted as a supervised GPU image.
- The reviewed entry names the exact GPU configuration and provides evidence for exclusive GPU ownership, the initial GPU and IRQ state, shared clock and power availability, and the image, input and arena DDR reservations. Firmware identity and ranges are checked from the artifact. Whole-board power removal and no reuse before it remain mandatory, and missing physical facts refuse launch.
- Reserved preparation records 400 MHz as the backend's admission check requires; successful native binding replaces it with `A733_GPU_CORE_HZ` before callbacks are published. OPEN reconstructs the firmware state from that configuration, so the system-init clock at byte offset 344 and the runtime clock at byte offset 12 both record 1008 MHz, matching PLL_GPU0 and the 960 mV supply.

## Qualified figures

The figures come from modeled queues and scheduling; they do not execute the GPU. Records are in [gpu-recognition-wait.jsonl](../evidence/gpu-recognition-wait.jsonl).

| Figure | Value | Record |
| --- | --- | --- |
| Image cases (timely, absent, late and invalid reply; accepted OPEN without reply) | 5 pass | `gpu-recognition-wait-software` |
| Host admission cases; refusing CMake combinations | 17; 6 | `gpu-recognition-wait-software` |
| Caller executable size | 3,196 to 4,484 B; static state 78,448 to 78,504 B | `gpu-recognition-wait-software` |
| Added work for a timely job | 9 counter reads, 3 tick reads, 2 task queries, one 8-byte record copy | `gpu-recognition-wait-software` |
| Missing-reply terminal suspension | after 200 ten-tick receives | `gpu-recognition-wait-software` |

## Limitations and open work

- The normal job path keeps one slot publication, one wake and one completion receive, with no allocation, workload-buffer copy, device access or translation added.
- Physical latency, GPU drain after a timeout and recovery are not established.
- Open: qualify the bounds on a board in a supervised run with whole-board power removal available.
