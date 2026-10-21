# GPU interrupts wake the privileged service worker

GPU interrupt delivery is implemented for the FreeRTOS lifecycle service and
checked with QEMU's GICv3 interrupt controller. Physical A733 delivery remains
unqualified. GPU firmware and workloads have not run through this path on the
board. This document assumes familiarity with interrupt handlers, FreeRTOS tasks,
and memory-mapped peripheral registers.

## The interrupt handler masks delivery and wakes the worker

The [service configuration](../include/pvr_gpu_service.h) accepts optional mask
and rearm callbacks. Both must be supplied together. The platform configures the
interrupt's security group, trigger mode, CPU route, and priority before service
initialization. The interrupt must initially be disabled, exclusively owned, and
assigned a priority permitted to call FreeRTOS interrupt APIs.

The dispatcher calls `pvr_gpu_service_interrupt` after acknowledging the assigned
interrupt. The handler issues a mask command and queues one internal wake message.
The dispatcher owns end-of-interrupt and the FreeRTOS yield operation. Firmware
queues, GPU status, and resource handlers remain worker operations.

FreeRTOS critical sections serialize mask and rearm operations across the handler
and worker, including when they run on different cores. The handler coalesces
wake messages. If the request queue is full, existing messages already guarantee
worker progress; the handler retains the software mask without requiring a retry.

The worker masks delivery before advancing device control. It rearms only after
control reports a drained event batch. A batch that exhausts its budget keeps
delivery masked for the next worker iteration, and the worker steps again
within one tick. Once delivery is rearmed and no work is outstanding, a ready
worker sleeps until the next interrupt or request. The existing one-tick polling
fallback continues to advance deadlines and command returns without interrupts.
Startup, compatibility checking, and shutdown use that fallback with delivery
masked. CLOSE masks delivery before beginning the shutdown protocol.

A rearm callback can report that a prior disable is still propagating. The worker
allows at most 32 consecutive deferred attempts before retaining an interrupt
failure. This is an attempt bound; scheduling and callback duration determine
elapsed time. Successful rearm resets the count.

A mask or rearm failure retains ownership, allocations, and diagnostics. The
service makes no further backend calls. An interrupt handler that cannot disable
delivery returns failure to the dispatcher; the platform must initiate independent
recovery if delivery cannot remain disabled. A failure return does not establish
hardware quiescence or permit buffer reclamation.

## The GIC adapter performs bounded register operations

The [GICv3 adapter](../drivers/gpu/gicv3_spi.c) supports one preconfigured shared
peripheral interrupt, identified by an interrupt number from 32 through 1019.
Its immutable binding contains the mapped distributor address and interrupt
number. The platform supplies Device-nGnRnE mapping and serializes ownership.

Masking writes one bit to `GICD_ICENABLER` and completes CPU writes with a data
synchronization barrier. It does not spin on distributor propagation in interrupt
context. Rearming reads the distributor's Register Write Pending bit once and
returns pending while that bit remains set. Once clear, the adapter orders prior
memory writes, sets `GICD_ISENABLER`, completes the write, and checks the enable bit.
A failed enable readback issues another disable command before returning failure.
The adapter neither clears pending interrupts nor acknowledges or ends an interrupt.

The register operations follow the
[Trusted Firmware-A GICv3 implementation](https://github.com/ARM-software/arm-trusted-firmware/blob/0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e/drivers/arm/gic/v3/gicv3_main.c)
and its distributor accessors. Source snapshots and hashes accompany the evidence.
The native adapter separates disable issuance from propagation checking so the
interrupt handler has no register-poll loop. A stalled register access or barrier
still requires independent watchdog recovery.

For A733, the board description assigns GPU SPI 63, corresponding to GIC interrupt
95. The separate DVFS SPI 64 is outside this binding. The distributor address is
`0x03400000`; QEMU uses `0x08000000`. Platform security-group ownership, routing,
priority setup, and dispatcher attachment remain required before board use.

## Two emulator cases check delivery and retained failure

The [focused fixture](../checks/pvr-gpu-irq.c) reuses the lifecycle service's
software firmware model and reserved allocations. It sends actual software-pending
GIC interrupt 95 to core zero while the privileged worker runs on core one.
Both cases cover:

- No backend callback, mask or rearm for twenty ticks while an armed ready
  session has no outstanding work.
- Interrupt delivery, masking, worker wakeup, and cross-core serialization.
- A full request queue with no previously queued wake message. Internal wake
  messages fill the queue, and the interrupt handler's failed enqueue preserves
  progress after the worker resumes.
- Nine firmware events, exceeding the four-event worker budget, with pending
  interrupts retained while delivery is disabled.
- Temporary deferred rearm, successful shutdown, and a stale pending interrupt
  delivered after reopening with a fresh firmware epoch.
- Retained ownership and no additional backend calls after terminal failure.

The first case ends with a rejected rearm. The second ends after all 32 permitted
deferred attempts. Both pass. A refused-enable check also uses a QEMU SPI beyond the implemented
interrupt count, exercising failed readback and subsequent disable. The deferred condition is injected at the platform
callback; QEMU does not establish physical distributor propagation timing.
Firmware instructions, physical GPU interrupt assertion, and mask failure recovery
remain untested. No board access or performance measurement occurred.

Evidence is retained under `~/.cache/sunstoneos/evidence/gpu-irq-20261016/`.
The `failed-enable-disable` directory contains passing final images and logs; the input
manifest records source, dependency, toolchain, and firmware identities. Earlier
attempts remain separate. Existing component suites and mimus checks were reused.

Include `cmake/gpu-irq.cmake` after `cmake/gpu.cmake`. In a build configured with
the pinned GPU firmware and AArch64 toolchain, run only after relevant inputs change:

```sh
cmake --build ~/.cache/sunstoneos/build/gpu-irq --target gpu-irq-model -j 4
python3 checks/pvr-gpu-irq.py \
  --elf ~/.cache/sunstoneos/build/gpu-irq/gpu-irq-model.elf \
  --qemu ~/.cache/sunstoneos/host/usr/bin/qemu-system-aarch64 \
  --nm /usr/lib/llvm-21/bin/llvm-nm \
  --output ~/.cache/sunstoneos/evidence/gpu-irq-local
```

## Physical qualification requires a complete board executable

The first physical check must follow independent watchdog and ROM-return
qualification. A board executable must supply validated GPU power/reset control,
reserved memory and cache operations, GIC setup, and dispatcher attachment.
No physical test has been queued because that executable is not available yet.

Once those dependencies exist, the physical coordinator can boot firmware, obtain
health readiness, observe interrupt 95, service a firmware event, and verify that
shutdown leaves delivery disabled. Preserve event and interrupt counters, retained
failure diagnostics, watchdog recovery evidence, and the executable identity.
The coordinator must hold the exclusive board lock and stop on a transport fault.
