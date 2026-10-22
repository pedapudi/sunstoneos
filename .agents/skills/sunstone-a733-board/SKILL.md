---
name: sunstone-a733-board
description: Prepare and coordinate SunstoneOS physical tests on Orange Pi Zero 3W A733 boards, including FEL access, RAM-only boot, per-board evidence and failure recovery. Use for board operations or offline preparation of a board-test proposal.
---

# Operate the selected A733 board with recoverable ownership

Use this repository's [working rules](../../../AGENTS.md) and current
[handoff](../../../docs/handoff.md). This board is the A733-based Zero 3W;
H618/H616 Zero 3 instructions are not interchangeable. RAM capacity, fitted
components and attached peripherals require evidence for the particular board.

## Bind access to a physical port

Only the primary coordinator operates hardware. Workers prepare artifacts and
procedures offline. Use the [board queue](../../../docs/board-test-queue.md) for
the current port, queue and token assignments. Token contents stay private.

Set `SUNSTONE_USB_PORT` for each runner process. Use
[`tools/usb-access.py`](../../../tools/usb-access.py) to discover that port,
check its identity and session access, and acquire its existing `board_lock()`.
The lock is per physical A733 port and survives USB re-enumeration. It does not
reserve unrelated mimus boards. Queue ownership and the port lock serve different
purposes; acquire both for a queued physical test.

USB node numbers change. Bind a proposal to the observed instance and check it
again under the lock. A visible FEL descriptor does not prove that ROM commands
work after a failed launch. Respect the helper's failed-instance records.

## Establish the prerequisites the next operation actually uses

Follow [boot evidence](../../../docs/boot.md) and the existing runner for the
chosen operation. Preserve the existing contents of both microSD cards; write only as [the goal](../../../docs/goal.md) section 4 permits. Inspect the command before running
it; a tool's filename is not proof that it only touches RAM.

Read-only MMIO can still stall the processor if the target block is inaccessible.
Establish the required power, clock and reset state before reading its registers.
An exception handler or host timeout cannot guarantee recovery from a stalled bus
access. Use documented always-accessible status registers for initial discovery.
Check register read semantics too: a read can acknowledge or clear device state.
During FEL communication, ROM owns the USB controller; inspecting its endpoint
or interrupt registers can disrupt the transport used to collect the result.
Prefer accessible clock/reset status for initial USB discovery.

- After a cold reconnect, establish protocol identity and DDR initialization
  before native DRAM execution. DDR training and two page comparisons do not
  qualify complete capacity, stability or DMA reachability.
- After a demonstrated warm ROM return, reuse valid DDR evidence through that
  recorded continuity. Refresh the current launcher state required by its
  admission checks. Warm ROM may change cache and stack state; do not treat it
  as the original cold session or rerun DDR training without a reason.
- After native execution returns to a new ROM instance, its live SRAM belongs
  to that ROM. Do not restore saved bytes over its current stack or scratch area.
  Follow the runner's separate SRAM and DRAM restoration rules.
- A second board needs its own identity, initialization and physical evidence.
  Common immutable code and software checks may be reused. Electrical state,
  ownership, power history and memory reservations do not transfer between boards.

Keep source-backed expectations, physical observations and assumptions distinct.
An admission record describes established authority; setting its booleans cannot
create that authority. Conversely, watchdog expiry and full multicore operation
are not blanket prerequisites: inspect the selected image's actual dependencies.

## Make one bounded attempt and preserve its outcome

Before claiming the queue, freeze the image, host tools and required inputs.
The proposal needs the target, changed behavior, prerequisite evidence, expected
result, timeout, stop condition and evidence directory. Reuse the existing queue
schema and component procedure instead of creating another execution framework.
When freezing a host tool, include files that it hashes at runtime as well as
Python imports. `run-launch-survey.py`, for example, requires
`boot/a733/launch-survey.S` beneath its frozen repository root. Verify those
paths and the final invocation's artifact hashes before claiming the queue.
A preflight refusal before device access does not invalidate passing checks;
repair the missing package input and resume the unexecuted operation.

Require reconnect permissions before intentional re-enumeration. On the first
transport failure or disappearance, stop commands to that instance. Do not reset,
redownload, probe repeatedly or use the other board to repeat an unchanged failure.

A timeout is not process completion. Poll the existing process/session handle;
retain its queue claim and lock while it is live or its recovery is unresolved.
Do not kill a retained runner merely to free a lock. After explicit physical
recovery, verify the new instance and the exact old process before closing the
old claim. Mark a claim finished only after the associated process has stopped.

On success, keep the copied result and required restoration evidence before
releasing ownership. Report the observed function and its limits. Missing camera,
display or microphone accessories block their physical qualification, while
independent work continues. The [wiring guide](../../../docs/board-wiring.md)
holds connector and recovery details; do not invent a recovery button.
