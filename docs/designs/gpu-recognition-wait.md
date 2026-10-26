# Bounded GPU replies in supervised recognition

Status: implemented and checked with modeled service replies and host admission.
The integrated OS supports supervised recorded recognition with the GPU service. It does not qualify physical execution,
GPU drain after failure, or reuse of a failed board lifetime.

## Service and caller deadlines

The recognizer and GPU service communicate through the existing single job
slot and completion queue. `pvr_mimus_service_submit` returns success once the
worker owes a completion, including when a failed wake cannot reclaim the slot.
The service bounds ring admission by one second and starts another one-second
deadline at the accepted kick. Only a timely completion, matching timeline and
completed output maintenance publish a successful record.

`apps/os/gpu.c` bounds the caller's observation separately from the service's
device deadline. OPEN and CLOSE use one-second absolute deadlines, and an
accepted OPEN whose reply never arrives enters terminal retained failure.
Supervised recorded GPU builds use these bounds without depending on the
resident service window or watchdog.

The supervised board supervisor closes admission on failure and retains the
running lifetime. It does not return to FEL or reset the device. The recognizer's
existing `failed()` sets its stopped state, fails the board and suspends forever.
That path preserves the borrowed inference context and all GPU-accessible
storage. It is the terminal path for the new caller failures.

## Request lifetime

Before publishing each job, the recognizer captures one absolute deadline:
the minimum of now plus two seconds, the current board live-until value minus
the existing 10 ms guard, and the session deadline minus that guard. Checked
arithmetic rejects an invalid window. The deadline is never renewed by queue
polling, unrelated CPU progress or a later board renewal.

Two seconds covers the service's two one-second stages without adding a third
unbounded queue interval. Because it starts before publication, this is a
conservative caller limit: a sufficiently delayed service may lose caller
admission before its own final deadline. Such a timeout does not establish
device failure or completion. No physical timing claim follows from this limit.

The native caller records the pending job and deadline only for a successful
submission. A refusal that reclaimed the slot still permits the existing CPU
handling. A pending job prevents another submit, CLOSE, reopen or retirement.
The completion function returns a separate caller outcome and writes a PVR
record only when it actually received one. The hook treats a protocol timeout
as a distinct dispatch failure, not a fabricated LOST completion.

Every receive blocks for at most the existing ten ticks. The caller checks
board admission, monotonic counter progress and its captured deadline before
and after the receive, including a successful receive. Equality is late. A late
record cannot lend its sums to the engine. A captured FreeRTOS tick budget
also bounds repeated empty receives if the architectural counter stops
advancing while FreeRTOS continues to schedule. A finite observation count
covers broken queues returning while both clocks remain frozen. Counter
reversal is a terminal protocol failure. No wait loop reports recognition
progress.

On timeout, revocation or an invalid reply, the caller latches failure and the
hook enters `os_dispatch_failed` before clearing its in-flight state or returning
to the engine. It sends no CLOSE or reset, does not clear the service slot or
accepted request identity, and releases no mapping, buffer, clock or rail ownership.
The service collector clears its own pending ticket when it receives a reply,
even when the caller subsequently finds it late. The caller preserves the
accepted identity and failure classification; it cannot submit another request
or reuse storage in either case. The backend's existing board-fatal admission gate prevents further normal device
access once `board_fail` publishes failure. A later queued completion cannot
resume the suspended caller. The GPU may remain active; manual whole-board
power removal remains the recovery requirement.

OPEN and CLOSE keep their one-second absolute windows. Their finite receive
budget spans the complete operation: OPEN reply and session-ready record, or
CLOSE reply and every STATUS query. No helper resets that budget.
Once an OPEN request has entered the service queue, any missing, late, failed or mismatched reply,
or missing session-ready record, is terminal. A refusal before the request
enters the queue retains the existing CPU-only startup behavior. Successful
thermal park/reopen and verified terminal retirement keep their current
semantics. These paths use the same bounded observation rules without adding
a general request framework.

These bounds assume the caller can be scheduled and bounded queue operations
return. They cannot interrupt a stuck CPU, interconnect or MMIO instruction.

## Supervised composition and admission

The added composition is recorded recognition with the existing two inference
workers and GPU service. The A55 pair remains cores 2/3; admitted A76 remains
cores 6/7. The GPU worker stays at priority 4 on the recognition core, above
the priority-3 recognizer, with INTID 95 routed to the same core. The existing
eight-worker, NPU and performance-mode exclusions remain. Supervised audio,
radio and headset GPU combinations remain unsupported. Resident compositions
retain their existing options.

The linked image must contain the exact-sum hook, GPU worker and interrupt
entry, firmware with the existing pinned digest, and the placement table
generated from the unchanged model assets. It must retain the writable arena
at 0x46000000–0x46c00000 and read-only CPU view at 0x46c00000–0x47000000.
The existing firmware/session pool allocations and all image, stack and heap
extents must remain disjoint. No model, engine pin, placement arithmetic,
scalar sum validation or operating-point change is part of this work.
Before constructing providers, GPU startup also excludes the sealed OS thermal
owner from its fixed firmware and session owner identities, alongside the
existing board-owner exclusions.

A small retained configuration record uses the actual native deadline,
priority and core constants. The link checker reads it together with the
embedded firmware and placement bytes. Preparation and frozen-entry validation
compare the resulting GPU configuration with the exact ELF and reviewed entry;
an old GPU image without the bounded-wait capability cannot be admitted as a
supervised GPU image. Non-GPU images retain their existing admission.

The reviewed entry must name the exact GPU configuration and provide evidence
for exclusive GPU/remote ownership, the initial GPU and IRQ state, shared clock
and power availability, and the existing image/input/arena DDR reservations.
Firmware identity and ranges are checked from the artifact rather than accepted
as physical facts. The existing explicit whole-board power-removal and
no-reuse-before-power-removal requirements remain mandatory. Missing physical
facts continue to refuse launch. This does not authorize a current-board run.

The native GPU provider headers permit this concrete supervised caller when its
reviewed entry establishes exclusion of other GPU/DMA users and prior device
access, ownership of the GPU power/clock/IRQ controls, actual memory mappings
without aliases, and available whole-board power removal. Other native GPU
callers retain their existing watchdog recovery requirement. The separate
PCK-only supervised exception does not authorize GPU DMA.

Reserved preparation initially records 400 MHz as required by the existing
backend's admission checks. Successful native backend binding replaces that
initial value with `A733_GPU_CORE_HZ` before publishing callbacks. Service OPEN
reconstructs the firmware state from this configuration before publication and
startup. Both the system-init clock at byte offset 344 and runtime clock at byte
offset 12 then record 1008 MHz, matching the shared power controller's PLL_GPU0
setting and 960 mV supply. Refused binding preserves its input configuration.
The power sequence and PERI0 parent validation remain unchanged. A separately
reviewed actual binding/serializer check verifies both firmware clock fields.

Firmware is already embedded in the ELF; no new preload format is needed.
Host preparation adds the existing gpu-linear application and INTID 95 to the
expected tables and freezes the GPU entry evidence through the existing cache.
The launcher repeats the same exact-ELF admission on the frozen bytes.

## Validation and cost

Use the actual AArch64 native caller with modeled queue/service replies for a
timely job, empty queue through deadline, equality/late successful reply,
revocation, backwards/stalled counter, and pending-job rejection. Check the
actual hook and generated recognizer failure path: no sum loan, CPU fallback,
context return or second submission after timeout, even if a late completion
becomes available. Cover an accepted OPEN timeout and a pre-submit refusal.
Reuse the existing lifecycle fixture for one healthy park/reopen and one
uncertain CLOSE case affected by the helper changes.

Build one actual supervised recorded GPU image using the accepted toolchain
and pinned local engine. Inspect linked task/interrupt placement, firmware and
table identities, and stack/heap/pool bounds. Check only affected configuration
refusals, metadata tampering/missing evidence and frozen launcher admission.
Reuse existing engine, recognition parity and GPU service completion evidence;
do not run an inference or benchmark sweep.

The normal job path retains one slot publication, one wake and one completion
receive. It adds one 8-byte completion-record copy after validation, with no
allocation, workload-buffer copy, device MMIO or translation.
Added work is fixed-size caller state and counter/admission checks around the
existing queue operations. Count those calls and record native instruction and
static-storage differences. A two-second missing-reply path is bounded by
200 ten-tick empty receives; lifecycle waits by 100, with any shorter session
limit ending sooner. The separate observation bound is at most 4004 calls
per job and 2004 per lifecycle operation; it matters only if both clocks fail
to advance or the queue contract is broken. Scheduling delay remains outside
that counted budget.

## Recorded software evidence

The evidence directory is
`~/.cache/sunstoneos/evidence/resume-gpu-wait-20261026/`. Its final snapshot
preserves the source dependencies, effective commands, artifacts and tools.
The supervised A76-pair image has SHA-256
`549acba531fcd3f20e9f220049e9de042ca67cef45797aa3a5927b33a64c303c`.
Its linked contract is supervised with an 808-byte session. The image ends at
0x421f5040, below the model at 0x44000000. The 4 MiB heap and two 2 MiB device
pools fit below their existing limits; the fixed model, input, arena and read-only
GPU view remain disjoint.

The actual linked image passed five cases: timely reply, absent job reply, late
successful job reply, invalid completion and accepted OPEN without a reply.
The failure cases execute the generated recognizer's terminal suspension and
retain its context and GPU input/output ownership even after a modeled late
completion and attempted resume. Native caller cases also cover equality,
revocation, counter reversal, stalled clocks, pending work, wrong caller and
shared OPEN/CLOSE budgets. The healthy thermal reopen and uncertain CLOSE cases
pass. These checks model queues and scheduling; they do not execute the GPU.

Seventeen host cases pass through actual preparation, native admission and frozen
entry validation. They cover firmware, placement, IRQ and startup metadata,
missing physical facts, changed evidence and attempts to relabel a resident or
legacy GPU image. Six affected CMake combinations refuse configuration. An
existing non-GPU two-worker image still passes its unchanged host admission.
The actual launcher reaches its supervisor readiness check without transport.
All nominal physical facts in these fixtures are synthetic and cannot authorize
a board run.

With the same native compiler flags, the caller translation unit grows from
3196 to 4484 executable bytes and its static GPU state from 78448 to 78504 bytes.
The added 56 bytes hold the captured deadline and pending-request identity.
A timely native begin/finish adds nine architectural-counter reads, three RTOS
tick reads and two current-task queries. It retains one submission and one
completion receive. It adds one 8-byte completion-record copy after validation,
with no allocation, extra request, workload-buffer copy or device access.
The missing-reply case reaches terminal suspension after 200 ten-tick receives;
the accepted OPEN case stops at its shared one-second limit. These measured
software costs enforce the caller's ownership and observation bounds; they do
not establish physical latency, GPU drain or recovery.
