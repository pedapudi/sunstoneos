# Native NPU controller core

The native controller core is **implemented** and passes a host register and
buffer-ownership model. It compiles for AArch64. Physical initialization,
translation, command execution, cancellation, and recovery remain untested.
The core is excluded from default builds and is not linked into a board image.

The [interface](../include/a733_npu_controller.h) provides open, submit, poll,
cancel, reap, and close operations. The
[implementation](../drivers/npu/a733_npu_controller.c) adapts the pinned driver
sequences recorded in [npu.md](npu.md) and retains their MIT notice. The native
state machine controls deadlines and resource ownership.

Startup and cancellation follow `vip_drv_hardware.c`; bootstrap commands follow
`vipdrv_device_prepare_initcmd` in `vip_drv_device.c`. Event acknowledgment follows
the status read in `os/linux/vip_drv_device_driver.c`. The source manifest records
the hashes of these files and the inspected feature database.

## Platform and memory requirements

The [privileged FreeRTOS service](npu-service.md) serializes every call. The
[native adapter](npu-native.md) supplies register, clock, delay, barrier, and
memory-publication callbacks and passes an emulator check. The service
must establish power, clocks, reset ownership, and independent watchdog recovery
before opening the controller. The [platform controller](npu-platform.md) and
[startup adapter](npu-startup.md) implement resource acquisition and deferred
service activation with model evidence. Physical platform qualification remains
outstanding.
A synchronous bus stall can prevent a software deadline from running; the
independent watchdog requirement still applies.

The polling core reads event status at register offset `0x10`. The platform
must mask the corresponding GIC interrupt and prevent concurrent status reads.
The optional interrupt service keeps event acknowledgment in the same worker;
its ISR only masks the source and notifies that worker.

The memory-publication callback receives the selected
[MMU image](npu-mmu.md), command descriptor, and bootstrap indicator. It must
validate the image against reserved, pinned memory and the actual CPU mappings.
It must also validate the trusted command stream, clean device-read buffers,
and clean and invalidate device-written buffers. Publication must finish before
command submission. The [memory policy](npu-memory.md) implements private buffer
admission, table validation, and cache-operation ordering through callbacks.
The native adapter binds those callbacks to CPU operations. Command-semantic
validation remains outstanding.

The controller borrows the MMU image, bootstrap allocation, and associated
resource bundle until successful close. Each application job remains borrowed
until successful reap. A fault retains all borrowed resources. Pointer tokens
identify privileged resource bundles; they are not application pointers or
proof that a command is valid.

## Startup and command completion

Open checks for product ID `0x1000003b` and revision `0x9202`. It publishes the
bootstrap resources and performs the reference driver's two reset passes.
A failed pass ends the attempt. The core then configures memory counters,
clock-gating controls, page-directory MMU mode, reorder behavior, and event
reporting. Board power and clock configuration must already be valid.

The bootstrap encoder writes 192 bytes containing a translation-cache flush,
reference initialization commands, internal-SRAM base configuration, completion
event zero, END, and trailing padding. The internal SRAM base is `0x400000`.
The platform must reserve the SRAM virtual window from ordinary allocations;
its complete translation and access behavior still needs implementation and
physical qualification.

The reference trigger writes a command address at `0x654`, orders memory, and
writes `0x1ffff` to `0x3a4`. That trigger includes a fixed fetch count of 65,535
eight-byte units. The native core preserves this count and requires a readable
mapping covering at least 524,280 bytes from each command address.
Smaller fetch counts remain unqualified. Command addresses must be 64-byte
aligned, fit the 32-bit device address space with their readable span, and
avoid the reserved SRAM window.

Commands must end with event zero from the pixel engine followed by END.
The core does not validate arbitrary commands. The model loader and privileged
service must enforce register permissions, relocation bounds, tensor ownership,
and termination before publication.

Before each trigger, the core verifies the selected directory, MMU enable mode,
event mask, full idle status, and clear event status. Polling then accumulates
events. Completion requires event zero and idle status `0x7fffffff`, covering
all processing units. A second event read after idle detects faults that became
visible during the idle check.

Any event bit other than completion causes a retained fault, including AXI and
MMU errors. An idle device without a completion event remains pending until the
deadline. This prevents an ignored trigger from appearing successful.

After confirmed completion, a memory callback invalidates device-written buffers
and orders CPU visibility. Callback failure retains ownership. Successful
bootstrap completion enters READY; successful application completion enters
DONE until the caller reaps the result.

## Cancellation and shutdown preserve ownership

Cancellation asserts the request bit at `0x3a8`, keeps it asserted for at least
100 microseconds, then clears it. Subsequent polls require the cancellation
acknowledgment bit and full idle status. Acknowledgment alone cannot release
buffers. A separate bounded stop deadline applies during cancellation.

Successful cancellation enters CANCELED. Reaping returns the job token and
enters DRAINED, which permits close but refuses further submissions. The
controller must close and reopen before executing another job. Canceling the
bootstrap does not shorten the bootstrap allocation's lifetime; close is still
required before releasing session resources.

Close requires READY or DRAINED, unchanged MMU configuration, full idle status,
and clear event status. It disables event reporting and the MMU, checks their
readback, orders memory, and verifies idle again. Only successful close releases
the session's ownership requirement.

A register-access failure ends all register callbacks for that instance.
Unexpected register state, deadline expiry, failed memory synchronization, or
an accelerator fault enters FAULT. Poll, cancel, close, reopen, and reap cannot
clear that state. External recovery must establish that device accesses have
stopped before software discards the instance or releases its resources.
Automatic fault-reset recovery is not implemented.

Timeouts use a monotonic microsecond counter modulo 2^64 and durations from one
through `INT64_MAX`. Cancellation needs more than 100 microseconds. The platform
must provide a consistent counter across callers and bounded callback execution.

## Focused validation

The [host check](../checks/npu-controller.c) uses a stateful register model,
read-to-acknowledge events, delayed completion and idle transitions, and separate
CPU/device buffer copies. It builds a real native MMU image for the command
allocations and checks that publication receives that selected image.

The check covers:

- Bootstrap decoding, initialization state, buffer publication, and command trigger.
- Event arrival before memory idle, output visibility, single result consumption,
  and successful shutdown.
- Counter wrap, ignored starts, stale events, directory changes, AXI/MMU faults,
  and a fault that appears during the final idle read.
- Cancellation pulse timing, acknowledgment without idle, failed cancellation,
  and refusal to submit after cancellation until close.
- Register failures, publication and completion failures, retained ownership,
  identity mismatch, startup expiry, and insufficient command mappings.

An initial sanitizer check and target compile passed. API review then made the
selected MMU image explicit in the publication callback. One focused sanitizer
check and target compile passed on that final interface. Earlier inputs are
superseded for this component; unchanged MMU-builder, model-inspection, DMA, and
mimus evidence remains reusable.

Final evidence is under
`~/.cache/sunstoneos/evidence/npu-controller-20261016/final/`.
The AArch64 object has SHA-256
`0667e3f2255966cfb30441445550602d689c43bb15262d308391670b82a48cae`.
It contains 5,508 bytes of code and constants, no static data, and an unresolved
`memcpy` reference supplied by the runtime. These are object measurements;
service storage is measured in its separate implementation record. Physical
stack usage and timing remain unmeasured.

Reproduce the focused host check with:

```sh
clang-21 -std=c11 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude \
  checks/npu-controller.c drivers/npu/a733_npu_controller.c \
  drivers/npu/a733_npu_mmu.c -o /tmp/npu-controller-check
/tmp/npu-controller-check
```

After configuring the AArch64 build as described in [build.md](build.md), compile
only the controller with:

```sh
cmake -S . -B "$HOME/.cache/sunstoneos/build/qemu-virt"
cmake --build "$HOME/.cache/sunstoneos/build/qemu-virt" --target a733-npu-controller
```

The [native assembler](nbg-assembly.md) now supplies relocation and command
construction for the recorded model profiles. Complete command-memory admission,
physical platform startup, numerical inference and error recovery remain
outstanding. No hardware request occurred during these controller checks.

## Optional notified progress

`a733_npu_advance` separates event sampling from deadline, cancellation and
previously latched completion progress. The interrupt service samples `0x10`
only after its notification and a propagated mask with no active handler. The
worker holds that mask after event zero while waiting for full idle, then reads
events again before acquiring output visibility. Cancellation still requires
its request pulse, acknowledgment and full idle even when no event interrupt
arrives. `a733_npu_check_timeout` checks only time while masking is deferred.
It cannot acknowledge events or touch NPU registers.

The original `a733_npu_poll` preserves its inactive-state no-op behavior and
continues sampling events for the default polled profile. An event observed
between jobs by the optional notified path cannot be attributed to a later job.
Neither a notification nor an idle observation alone establishes completion.
