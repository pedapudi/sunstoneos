# Finite inference uses interrupt completion and copied outputs

Observed board evidence: the startup-only image returned with
`NPU_STARTUP=READY_CLOSED_STOPPED; inference_jobs=0` and
`npu: returned; matched`. The coordinator recorded the run under
`runs/01-startup-only-28c8e15a2b62-834973ec` in the NPU build directory.
This establishes bootstrap, close and normal return on that run. Numerical
inference and a watchdog reset during an active job remain unqualified.

The comparisons below are observed source behavior. The vendor paths are
relative to `vip2/` at revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`; the source is cached beneath
`~/.cache/sunstoneos/sources/npu/board-vip2-` followed by that revision.
The native paths are relative to the repository. The reader is assumed to
understand memory-mapped registers, interrupts and DMA cache ownership.

## Initialization completes before the first inference

Observed: the [fabric comparison](FABRIC-STAGE.md) covers supplies, domain,
clocks, resets, SRAM routing and the EL3 transition to the 600 MHz MBUS.
Both inference images retain its register-pass markers `0x0630–0x0643`.
Their NPU operating point is the bin-4 maximum, 852 MHz at 800 mV.

| Step | Vendor source and sequence | Native source and comparison |
| --- | --- | --- |
| Identity and feature selection | `inc/vip_feature_database.h:161–183`: product `0x1000003b`, revision `0x9202`, page-directory MMU, 32-bit virtual addresses, 512 KiB VIP SRAM, no AXI SRAM and the shader conformance fix. | `drivers/npu/a733_npu_controller.c:135–139` refuses other product/revision pairs. Its selected register settings match this entry. |
| Two reset passes | `vip_drv_hardware.c:114–228`: write offsets/value pairs `000/70900`, `104/0`, `10c/15a0881`, `10c/15a0880`, `000/70b00`; wait 5 µs; `000/70900`; wait 20 µs; `000/f0900`, `3a8/1`; wait 20 µs; `3a8/0`; wait 40 µs; `000/f0900`, `000/70900`; wait 50 µs. Each pass requires `004 & 1 = 1`, `000 & 30000 = 30000`, and `388 & 1 = 0`. | `a733_npu_controller.c:99–121` implements these writes and delays twice, then `55c/ffffff`, clears bit 6 at `090` and requires exact `004/7fffffff`. A failed pass retains the device instead of retrying an unresolved failure. |
| Configure the front end | `vip_drv_hardware.c:430–510`: clock/control, MMU page directory, reorder bypass, clock gating and interrupts. | `a733_npu_controller.c:144–160`: `000/70900`, `000/70100`, `3a8/2`, `03c/ffffffff`, `03c/0`, `100/140021`, `3b4/<page directory>`, `388/21`, `104/0`; clear bit 16/set bit 17 at `10c`, set bit 6 at `090`, clear bit 0 at `414`, then `014/c0000001`. Drain `010`, wait 10 µs, verify configuration, full idle and no pending event. |
| Publish and run initialization | `memory/vip_drv_mmu.c:1154–1159` supplies the MMU flush prefix. `vip_drv_device.c:210–244` emits initialization, including eight SRAM-bank writes. `vip_drv_hardware.c:795–850` triggers, polls front-end idle every 100 µs for at most 100 attempts, then waits 10 µs. | `a733_npu_controller.c:a733_npu_bootstrap` inlines flush and initialization, names VIP SRAM at `0x400000`, explicitly zeros the unused AXI SRAM fields and ends with EVENT0 and END. Completion requires EVENT0 and full idle. After successful memory completion it now waits 10 µs and rechecks the original deadline before publishing READY. |

Observed difference corrected: the 10 µs post-initialization delay was absent.
The host controller case fails without it, checks that ordinary jobs gain no
delay, and refuses READY if settling exhausts the OPEN deadline.
Unknown: whether this delay changes numerical behavior on the board.

## Submission preserves the vendor command and memory ordering

Observed: `drivers/npu/nbg_command.h` brackets the relocated network commands
with shader semaphore/stall commands. The footer uses the full flush/stall
token `0x30000701`, the shader flush value `0xc23`, EVENT0 value `0x40`,
then END. `drivers/npu/nbg_assembly.c:342` initializes the remaining fetch
allocation with END pairs. Relocations bind descriptors, coefficients,
inputs, outputs and working memory to the admitted NPU address ranges.

Observed: the vendor queue can append WAIT/LINK commands and dynamically
switch page tables. These finite images have one outstanding job and stable
page tables. They submit END-terminated buffers instead. Their EVENT0 mask
also enables the AXI/MMU error bits (`0xc0000001`); other event numbers are
outside this command contract. There is no firmware or microcode upload or
SCP handshake on this path: the NBG holds commands and network descriptors.

Observed: `drivers/npu/a733_npu_memory.c:218–231` cleans tables, immutable
streams, constants and inputs before device ownership. It cleans and
invalidates writable buffers before submission. The runtime performs the
equivalent operations in `a733_npu_runtime.c:245–308,512–563`; each LOAD
cleans its newly assembled data, and each job also zeros and publishes its
working pool. Cache operations include their completion barriers.

Observed: `vip_drv_hardware.c:882–893` writes the command address at offset
`0x654`, executes a memory barrier, then writes `0x1ffff` at `0x3a4`.
`a733_npu_controller.c:trigger` follows that order, after publication and a
barrier. It adds no status read between those writes. The 512 KiB fetch
allocation and END padding keep prefetch inside the readable extent.

## Interrupts precede output access and resource release

Observed: `os/linux/vip_drv_device_driver.c:382` reads interrupt status once.
The native ISR masks INTID 97 and notifies its service; the service worker
reads the clear-on-read device event at `0x10`. The controller rejects every
bit except DONE, then requires full idle `0x7fffffff` at `0x04` before
invalidating output or publishing completion. Deadlines bound missing events
and delayed idle. A timeout retains resources; it does not authorize reuse.

Observed difference corrected: the shared runtime optionally used full idle
as a hint to sample the device event without an ISR notification. That path
could increment the counter reported as interrupt completions. The finite
runtime binding in `apps/npu-finite/runtime-irq.c` now sets `idle_hint=0`.
The shared runtime's optional behavior remains available to its other users.

Observed: `a733_npu_memory.c:236–251` and the runtime `finish` callback
invalidate device-written output only after full idle. The single-inference
client copies output and reaps its ticket. The runtime additionally checks
immutable data before releasing its SUBMIT reply; `drivers/npu/npu_job.c`
then copies the tensor output. Both finite clients compare owned copies
against linked reference bytes. A numerical mismatch returns status 1.

Observed: CLOSE requires no outstanding job, exact configuration, full idle
and no pending event. `a733_npu_controller.c:a733_npu_close` disables and
verifies interrupt enable at `0x14`, disables and verifies MMU control at
`0x388`, barriers, and checks idle again. The platform then retires the IRQ
route, stops local clocks/resets, restores SRAM routing and verifies
quiescence before the monitor disarms WDT0 and returns to ROM.

Observed upstream comparison: Linux etnaviv `etnaviv_gpu.c:1723–1741` stops
its persistent stream by replacing WAIT with END before idle/suspend.
These images already terminate each stream with END. Copying that queue
patch into them would not add a stop operation. The cached upstream revision
is `cbbddb6b08cc9900a87cccaa9b22bbe06aa4f50a`.

## Host evidence bounds the next physical test

Observed host checks cover the bootstrap settling deadline, linked strict
runtime binding, common service timeout with an event but no ISR notification,
cache publication/invalidation, relocated command execution models, copied
output, close and watchdog return. The strict-binding check models GIC
readiness and the runtime callback; it does not establish physical delivery.

Inferred healthy single-inference report: `NPU_INFERENCE=MATCH;
inference_jobs=1`, two interrupt completions, stopped IRQ route, then
`npu: returned; matched`. Inferred healthy runtime report: the same report
with two jobs and three completions, after LOAD/SUBMIT/DONE/COPIED for each
of `conv12` and `conv8`. Each image directory contains its exact command,
hashes and retained-stage failure table. Stop after a failed run; run the
runtime only after single inference returns normally with matching output.
