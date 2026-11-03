# Bounded GPU firmware startup

The native GPU startup controller is **implemented**; no host check remains. It applies
the pinned MIPS startup sequence through platform callbacks and waits for the
firmware-started flag, following the reference functions. It has not accessed a physical GPU or booted firmware on the board.

This document assumes familiarity with MMIO, cache maintenance, and device
reset. The [API](../include/pvr_gpu_start.h) separates offline initialization,
starting a deadline, and bounded polling. A privileged FreeRTOS worker must
serialize calls. Board power, clocks, identity, allocation, and independent
watchdog recovery remain prerequisites for physical use.

## Firmware features determine reset and cache setup

Initialization revalidates the pinned A733 firmware format and decodes its
feature parameters in ABI order. The supported profile requires one cluster,
36-bit physical addressing, 40-bit virtual addressing, 16 KiB system-level cache,
and the address-zero workaround required by the MIPS wrapper. Missing parameters,
unknown feature bits, malformed counts, and unsupported profile values are refused.
The caller must separately verify the firmware hash and physical device identity.

Reset-mask selection, the second reset register, and system-bus protection setup
follow the corresponding feature bits. Cache setup preserves the existing hazard
check bit, selects the reference address hash, and bypasses the burst combiner
when the cache-line size is below 1,024 bits. The reference lazy-writeback
workaround applies only when its quirk is present and GPU multicore support is absent.

The [controller](../drivers/gpu/pvr_gpu_start.c) copies a validated wrapper plan
from [MIPS preparation](gpu-mips.md). It checks register offsets, widths, fixed
values, remap alignment, address limits, and agreement between the two boot-code
remaps. Firmware preparation and platform admission still own complete physical
allocation and mapping checks.

No callback runs during initialization. Refused inputs leave controller state
unchanged. Accepted inputs produce a private sequence; later polling does not
borrow the firmware image, wrapper record, or callback table.

## Startup publishes memory before releasing the firmware processor

The pinned firmware selects a sequence of 29 steps:

1. Publish prepared firmware code, data, tables, queues, and initialization
   records. Read the 32-bit firmware-started word and require it to be clear.
2. Disable the reference system-bus protection and read back the register.
3. Assert the selected reset masks. Release the Rascal and Dust processing
   blocks, then hold only the firmware processor in reset. Each stage includes
   the reference readbacks. Upstream also writes `SOFT_RESET2` (`0x108`) at
   each stage when the firmware reports XE_TPU2; the vendor DDK never writes
   it for this core, so the A733 sequence does not
   ([firmware start design](designs/gpu-firmware-start.md)).
4. Read and update the low word of cache control, preserving its hazard bit.
   Apply the eleven prepared MIPS wrapper writes and configure the AXI-ACE interface.
5. Wait at least three microseconds, release firmware reset, read back reset,
   and wait at least another three microseconds.
6. Acquire the 32-bit firmware-started word from system initialization until it becomes
   one or the absolute deadline expires.

The reference MIPS path skips GPU BIF setup. [GPU workload page tables](gpu-vm.md)
remain separate from the firmware's MIPS translations. Firmware command queues
must already be formatted, but their host submission service is activated only
after boot and the remaining compatibility and health checks.

## Each poll performs at most one device operation

Platform callbacks provide ordered 32/64-bit register access, a monotonic
microsecond clock, firmware-memory publication, and an acquired 32-bit readiness word.
Writes must complete before their callback returns. Register readbacks fence
preceding writes. Shared-memory callbacks must maintain visibility without
writing stale CPU cache lines over firmware-owned data.

A poll performs at most one register access or publication operation. A delay
step starts after the preceding callback has finished and waits on the clock
inside the step for its 3 µs. The clock may report whole microseconds rounded
down, so the step waits until the clock passes its first sample by more than
3 µs. The readiness check re-reads the shared word for up to 50 µs
(`A733_GPU_SPIN_US`) and then returns to the caller, which polls again on its
next wake. A wait whose clock shows no advance for 1,024 consecutive samples
(`A733_GPU_SPIN_STALLED_READS`) also returns to the caller without advancing the
step; the next poll restarts the delay, so a stopped clock cannot shorten it.
Every wait ends at the startup deadline. The service
must continue polling and keep its independent watchdog protection active.

The caller supplies one absolute deadline for the complete startup. Clock
regression, callback failure, stale readiness, an invalid readiness word,
cancellation, or timeout latches failure and the current step index. Readiness
observed after the deadline is rejected by a second clock read.

After failure, the controller makes no further callbacks. It does not attempt
an unqualified reset, release allocations, or replay startup. A failed write may
have reached the GPU. The service must retain all backing and diagnostics until
an independent shutdown or recovery procedure proves device access has stopped.
Cancellation only stops this controller's work; it does not stop GPU execution.

The booted state establishes an acquired firmware-started flag. Firmware identity
exchange, command health, GPU job completion, shutdown, fault recovery, and physical
qualification are separate requirements. Reinitialization requires discarded
controller state after independently verified shutdown.

## No host check covers ordering, deadlines or retained failures

No host check runs the controller with the pinned firmware and a saved wrapper
plan against a device model that charges one microsecond for each clock read and
two for each device callback. The design is: publication precedes register
access, minimum delays are waited inside one poll, a readiness wait is bounded
to 50 µs per poll, a deadline can expire during a delay, and each poll makes at
most one register access. Under a clock that counts one 24 MHz tick per clock
read and callback and reports whole microseconds rounded down, as the board
counter does, the two 3 µs delays separated their bracketing callbacks by 98 and 95 counter ticks. Both are at least the 72 that 3 µs requires. With a stopped clock, a
delay and a readiness wait each return to the caller without advancing and
complete once the clock runs. A synthetic feature variant covers alternate
reset-mask selection, omission of the secure-bus operations, a 1,024-bit cache
line and the lazy-writeback workaround; it is not another physically supported
board profile.

No host check covers the 36 retained-failure cases (a failure for every device
callback in the successful sequence, clock failure and regression, stale or
malformed readiness, firmware boot timeout, timeout during a minimum delay,
cancellation, malformed wrapper settings, unsupported feature data and
repeated API calls after failure). No host check covers the 40 atomic input refusals. No host
check compares the startup, cache, AXI-ACE and MIPS wrapper register traces
with the reference functions. The reference makes twenty-four register accesses
for the pinned firmware (twenty-two for the synthetic variant), omitting its six
`SOFT_RESET2` accesses, and requests two three-microsecond delays. A trace
would establish neither physical reset, cache coherence nor firmware execution.

Clang 21 compiles the component for AArch64 with strict alignment and
freestanding options. It contains 3,724 bytes of code/read-only data and no
writable static data. Individual stack frames are 2,080 bytes for offline
initialization, 96 for polling, and 32 each for begin and the clock helper. The
caller must account for nested helpers, firmware decoding and callbacks. No
device access or performance measurement has occurred.

The recorded results of the removed host checks are in [the evidence record](evidence/gpu-start.jsonl).
