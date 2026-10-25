# Route the shared SRAM to the NPU for the numerical finite run

Status: implemented as a separate variant of the finite numerical NPU image
and checked on its linked instructions. It has not run on a board.

## Problem and reference

The A733's 512 KiB shared SRAM appears to the CPU at `0x00074000`–`0x000f3fff`
and is the NPU's internal SRAM. Bit 1 of SYSCTRL `0x03000008` selects its
owner:

- The boot ROM sets bit 1 at every reset, before its first use of the stack at
  `0x000f3400` in that SRAM (FIXS-BROM, SHA-256 `3a2a7d28…`, ROM offsets
  `0x20c`–`0x220`). The FEL scratchpad at `0x000ee800` is also in it.
- Board U-Boot clears bit 1 and reads it back in normal boot
  (`arch/arm/mach-sunxi/board_sun60iw2.c:119-133` and
  `board/sunxi/board.c:248-251`, u-boot-orangepi `b791be84`). Its comment
  reads "set sram to npu".

SunstoneOS images load through FEL, so no U-Boot runs. The read-only state
bundle (`board-bundles-20261025-6`) read `0x03000008` = `0x2` on both boards.
The numerical image `9e4adabf…` ran two jobs on port 3-1 with the SRAM routed
to the CPU, and both outputs were wrong and equal. The model uses 101,376
bytes of NPU SRAM. [The NPU reference backsolve](../reference-backsolve-npu.md)
ranks this as the divergence that best fits that failure.

## Change

The variant is `9e4adabf…`'s sources plus two changes, linked into a copy of
the default image by `apps/npu-finite/build-sram-route.py`:

1. **SRAM routing** (`apps/npu-finite/sram-route.c`, EL1, and
   `apps/npu-finite/sram-route-el3.c`, EL3).
   - After startup reaches READY and before the service activates, the
     bootstrap task reads `0x03000008`, requires bit 1 set, clears it and
     reads it back. Any other entry value or readback refuses activation, so
     the controller never opens.
   - After local stop has closed the NPU gates and asserted its resets, the
     same task sets bit 1 again and reads it back. A failed readback returns
     RETAINED, so the client never reports quiescence and no normal return
     follows.
   - At EL3, immediately before the forced-FEL reset, the monitor reads the
     word, sets bit 1 if it is clear, and parks with status 7 if the readback
     still shows the NPU routing.
2. **READY guard.** The NPU register callbacks refuse every access unless
   bit 1 has been cleared by the step above and the startup stage is READY.
   A refusal makes no bus access; the controller then faults with an I/O
   failure and retains every resource.

Five linker redirections (`--wrap`) attach the two files to the unchanged
objects: `a733_npu_startup_init` records the one sequencer,
`npu_service_activate` and `npu_convolution_stop_poll` gain the routing steps,
`a733_npu_native_operations` gains the guard, and `npu_finite_rom_return` gains
the EL3 restore. No existing source file changes, so the default finite image
and every other image are unchanged.

## Exit paths and the window with the SRAM routed to the NPU

The bit is clear from the activation step until the stop step. Every way out
of that window:

| Path | Restore |
| --- | --- |
| Normal: two jobs, CLOSE, local stop, quiescence, SMC, EL3 receipt check, ROM return | EL1 stop step, then the EL3 step before `RMR_EL3` |
| Local stop fails or retains | No quiescence, so `platform_exit` issues SMC 4; EL3 parks in `a733_smp_stop` |
| Any EL1 fault, deadline or refused receipt | SMC or exception to EL3; the finite monitor parks in `a733_smp_stop` (status 6) |
| EL3 recovery timer (FIQ) | The finite monitor parks in `a733_smp_stop` (status 6) |
| Bus stall | The CPU never returns |

The parked paths do not restore the bit. They never return control to the
ROM either: `a733_smp_stop` waits in `WFE` at EL3 with interrupts masked and
writes no reset flag. Only a power removal recovers them, and the ROM's reset
code then sets bit 1 itself before it touches the SRAM. Changing
`a733_smp_stop` would change the shared monitor of every single-core image,
so the variant leaves it as it is. The window on a failure path therefore
ends at the power removal that every failure already requires.

## Shared SRAM use while it belongs to the NPU

- **Image.** Every loadable segment and symbol lies in DRAM
  (`0x41000000`–`0x42200000`); the check confirms that none lies in the shared
  SRAM. The monitor stack, exception stack, page tables, result page and
  platform log are DRAM sections of `apps/npu-finite/link.ld`.
- **EL1 mapping.** The first gigabyte is one Device block
  (`platform/a733/cpu_coherence.c:59`), so the CPU makes no speculative or
  prefetch access to the shared SRAM; only an explicit load or store could
  touch it, and no image source names an address in it.
- **Launcher and host.** The launcher, request, report and stack regions
  (`0x52000`–`0x54000`) and the FEL scratch bytes the readers use
  (`0x71000`–`0x71080`) lie in SRAM A2, below `0x74000`. The host reads the
  shared SRAM only after the ROM has returned.
- **ROM.** The ROM's own stack and scratchpad are in the shared SRAM. It does
  not run between the launch and the forced-FEL reset, which happens after
  the EL3 restore. Whether the SRAM keeps the ROM's earlier scratchpad bytes
  across the two routings is unknown; the ROM rebuilds its FEL state after
  the reset in every earlier return.

## Cost

- **Hot path.** One NPU register access, at most a few hundred per job. The
  guard adds one acquire load of the routing flag and one acquire load of the
  startup stage: 20 instructions before the native callback in the check's
  count, beside the callback's two `DSB SY`. This is the single acquire load
  of [section 1.1](../performance-architecture.md#11-ownership-liveness-and-deadline-checks-repeated-on-every-register-access)
  for a state that can change during the session; the other NPU callbacks
  gain one forwarding branch each.
- **Once per lifetime.** Three SYSCTRL accesses at activation, three at stop
  and one to three at EL3.
- **Size.** `.text` grows by 696 bytes (170,296 to 170,992); `.bss` and the
  RAM layout are unchanged.
- **Section 3 NPU budgets.** Unchanged: no digest, translation, copy, queue
  round trip or cache operation is added.

## Failure classes

- Routing refused at activation: the controller never opens; startup
  resources stay retained until power removal (as for any activation
  failure).
- Guard refusal: an I/O fault of the controller; all resources retained.
- Restore readback failure at EL1: RETAINED; at EL3: park with status 7.
  Both need power removal.

## Checks

- `apps/npu-finite/sram-route-check.py` runs the linked wrappers under
  Unicorn with SYSCTRL modeled and the redirected functions stopped at their
  entry: 25 cases, including refusals before READY, after STOP and without
  routing, refused entry states and readbacks, the restore on each path, and
  the link redirection of every call site and of the native callback table.
- `apps/npu-finite/check.py` (26 return and cleanup cases) and
  `apps/npu-finite/profile-check.py` pass on the variant unchanged.
- The default finite image built from clean archives is identical with and
  without the two files, and its objects equal those of `9e4adabf…`.

Evidence: `~/.cache/sunstoneos/evidence/npu-sram-route-20261025/`.

## Predicted board result

Startup, both jobs, CLOSE, local stop and the ROM return behave as in the
`9e4adabf…` run, and both outputs match the reference. If the outputs still
differ, routing is ruled out as the cause; the stopped-memory capture, with
bit 1 restored, is then the next observation.
