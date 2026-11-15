# G2D in the OS image

The G2D 2D engine at 0x05440000 is an OS-image service. An application declares the `g2d` device in its manifest, the session grants it as `SS_DEVICE_G2D`, and the application calls the class interface in include/sunstone_g2d.h. One owner task on core 0 (apps/os/g2d.c, priority 4) makes every G2D, CCU and GIC access for the service. The G2D service writes no PCK600 or PMIC register: the shared power-domain owner (drivers/power/a733_power_domains.c) powers domain 1 (DE_SYS) for it.

Status: open, close and fill with INTID 83 completion through IOMMU master 10 are implemented and qualified on port 3-1 (docs/evidence/display-g2d.jsonl). The first fill run failed before the launch, because the cache maintenance refused a surface size that is not a whole number of 64-byte lines; the range is now widened to whole lines, and the second run (`1794714427314630502-a44050c7`) wrote the predicted surface, CRC-32 0xa872b4d7, with one interrupt 11.5 µs after the launch. [os-g2d-operation.py](../checks/os-g2d-operation.py) runs the linked operation path of the image under emulation over every surface size and start class the stage-2 to stage-4 plans draw, with a refused cache maintenance, a timeout and four closes after a fault. A blit with conversion between the eight 32-bit RGB orders is implemented and host-checked, and its first board run (stage 3) is pending. Scaling is not implemented yet.

## Vendor sequence and the native diff

The reference is the vendor driver in linux-orangepi 2ac08e8c (bsp/drivers/g2d/g2d_rcq, hashes in the source-identities.json beside the copy), the clock tree in ccu-sun60iw2.c and the device tree node g2d@5440000 in sun60iw2p1.dtsi.

| Vendor step | Reference | OS image |
| --- | --- | --- |
| Attach to PCK600 domain 1 before any clock or reset changes | genpd attach at probe; pck600_domains.c 104-155 | The start entry registers a domain-1 row before the scheduler starts. Open requests ON and blocks on the row's wake, with a deadline of 10,000 + 4,000 µs. The owner adopts an ON domain with no write. |
| Deassert the G2D reset (0xa44 bit 16) | `g2d_clock_enable`, g2d.c 580-586 | Same, under the shared CCU register lock, with readback. |
| Deassert the shared DE_SYS reset (0xa74 bit 16) | g2d.c 587-593; optional shared reset, g2d.c 1534 | Same. |
| Enable the DESYS MBUS and DE AHB gates (0x5e0 bit 11, 0x5c0 bit 5) | g2d.c 599-602; both `CLK_IS_CRITICAL` (ccu-sun60iw2.c 597-600, 673-676) | Not written. The power-domain owner requires both open before it admits the row. |
| Set the module rate to 300 MHz from PERI0_300M | `assigned-clock-rates` in sun60iw2p1.dtsi, applied at probe with the gate off | With the gate off, the open writes the CCU 0xa40 word 0x0 (mux 0, divide by 1). PLL_PERI0 is never programmed; the open refuses unless CCU 0xa0 reads it enabled and locked at 1,200 MHz with the 300M output gated on. |
| Enable the bus gate (0xa44 bit 0), then the module gate (0xa40 bit 31) | g2d.c 607-610 | Same; 0xa40 must then read 0x80000000. |
| Open the engine's own gates and resets | `g2d_bsp_open`, g2d_top.c 62-72 | 0x00 \|= 3, 0x04 \|= 3, 0x08 \|= 3, 0x18 \|= 1, each read back. |
| Close: module gate, bus gate, G2D reset, DE_SYS reset | `g2d_clock_disable`, g2d.c 642-669 | Same order. The DE_SYS reset is asserted only when the DE and DI module and bus gates (0xa00 bit 31, 0xa04 bit 0, 0xa20 bit 31, 0xa24 bit 0) read closed; otherwise it stays deasserted and the close record names the open gates. The critical gates stay open. Domain 1 stays ON. |
| Clear the engine's own gates at close | `g2d_bsp_close`, g2d_top.c 74-84 | Not written. It follows the bus gate in the vendor order, and the asserted G2D reset clears those registers. The close clears the RCQ interrupt enables (0x20 bits 4 and 6) before the gates. |

The open adds checks that the vendor driver does not make, each read before the first write:

- CCU 0xa44 & 0x10001 reads 0: the bus gate is closed and the G2D reset asserted.
- INTID 83 is neither pending nor active (GICD_ISPENDR2 and GICD_ISACTIVER2 bit 19).
- A module clock found gated on already reads 0x80000000.
- The power-domain owner's read-only domain-1 check passes: both critical gates and the PPU bus clock (R_CCU 0x1ac bit 0) open, and policy and status agreeing at 0 or 8.

A refusal at any of these writes nothing and leaves the engine closed. A failure after the first write marks the service FAULT and keeps the fault record (step and word) for `ss_g2d_fault`. The service then gets one close, from `ss_g2d_close` or from its quiesce step: the vendor-order close of the next section, run from the state the fault left. The close touches the engine's registers only while its bus clock and reset read open. When every close step reads back, the service is CLOSED and its quiesce step reports DONE, so the session returns normally with the fault in the application's record. When a close step fails, the service stays FAULT and keeps its leases and gates, and the quiesce step reports fault site 0x931 with the first failing step and word; the failure return then cannot finish and the watchdog ends the session. [os-g2d-fault-return.py](../checks/os-g2d-fault-return.py) runs that return through the image's own quiesce registry.

## The open record

`ss_g2d_open` returns a `struct ss_g2d_info`: the 23 platform words before the first write and after the open (PLL and PERI0 control and gates, the G2D, DE_SYS, DE and DI clock words, the critical gates, the PPU clock, the domain-1 policy, status and delay words, the IOMMU gates, and the INTID 83 enable, pending and active words), the 11 engine-local words including the version register at 0x10, and the timing of the open. The timing covers the row request, the owner's PCK command (0 when it adopted the domain) and the row's ON answer in the power owner's microsecond clock, and the domain-1 status word read at the request and at each row wake. Bit 8 of the policy and status words is recorded separately and refuses nothing. apps/g2d-check/host.py decodes the record of the g2d-check application.

The open also records the supply verification. `verify_at_us` is the power owner's time at which verification may start, and `verified_seen_us` is the time the open first found DCDC2 verified. The power owner records no verification time of its own, so `verified_seen_us` is an upper bound on it. `settled_supply` is 1 when the start entry marked the supply settled.

## The supply verification wait

The power-domain owner verifies DCDC2 and DCDC4 once per boot. It starts A733_POWER_SUPPLY_SETTLE_US (5,160 µs) after its first step, which covers a 1,000 µs enable delay and a 1,040 mV ramp, unless a consumer calls `a733_power_domains_settled_supply()` before the service first runs. The G2D start entry makes that call, as the NPU startup does. The settle is not needed in the OS image for three reasons:

- **DCDC2 has no writer.** Every OS-image user of DCDC2 (G2D, the NPU, PCIe, the USB 2 platform) holds a shared rail lease and reads it.
- **DCDC4 has two writers, and each covers its own settle.** The GPU platform raises DCDC4 from 800 mV to 960 mV (drivers/gpu/a733_gpu_platform.c, RAISE_SUBMIT). It writes under an exclusive DCDC4 lease, releases the lease (RAISE_RELEASE), and then waits its own settle (RAISE_SETTLE) before the GPU's first ON. The verifier can therefore read DCDC4 during that settle. It then reads the new selector, which it accepts, and the only power domain on DCDC4 is the GPU's (domain 5), whose first ON waits for the raise's settle. The GPU performance service changes DCDC4 between operating points (drivers/gpu/a733_gpu_performance.c) only after A733_POWER_SUPPLY_DONE, when verification has finished.
- **Earlier writers finish before the image starts.** The vendor boot0 writes DCDC2 and DCDC4 at power-up, and an earlier GPU image of the same power lifetime can leave DCDC4 raised. Both finish before the runner uploads this image, which takes seconds.

The verifier reads each rail's enable bit and voltage selector, and passes only a rail enabled at an accepted selector: 800 mV for DCDC2, and 800 mV or 960 mV for DCDC4. It reads no output voltage, so it cannot detect a rail still ramping to its selector. The settle covers that case, and the three facts above show that no rail of the image ramps at the first step. G2D's power domain depends on DCDC2 only.

## Operations

`ss_g2d_run` runs one operation on the open engine. The application names its surfaces by large region, offset, width, height, stride and format; the owner maps each region an operation names for IOMMU master 10. Two operations are implemented:

- **Fill** writes a color into a rectangle of an ARGB8888 output.
- **Blit** copies a rectangle of a source surface into the output, converting from the source's format to the output's. The source and the output have the same width, height and stride, as the native builder (drivers/g2d/a733_g2d.c) requires; they may be the same region. What a conversion writes to an X byte (formats 4 to 7) is not established on the board yet.

Before each launch the owner cleans and invalidates the whole 64-byte lines of the output and cleans those of a blit's source. After the interrupt it cleans and invalidates the output's lines again. [g2d-check-host.py](../checks/g2d-check-host.py) builds the g2d-check application on the host with a CPU model of the copies, and checks that its plan, samples and CRC-32 values equal the decoder's reference. Its mutant engine copies without converting, and the decoder names that outcome.

| Vendor step | Reference | OS image |
| --- | --- | --- |
| Build the RCQ command blocks | `g2d_fillrectangle`, g2d_mixer.c 83-135; `g2d_vlayer_set`, `g2d_ovl_v_fc_set` | `a733_g2d_commands_at` (drivers/g2d/a733_g2d.c), the reviewed native builder with the caller's device addresses, into one non-cacheable 4 KiB command page. |
| Map the buffers | the IOMMU domain of the g2d node, `iommus = <&mmu_aw 10 1>` | `os_iommu_attach` (apps/os/iommu.h) at the first operation after an open: the output's large region read-write, a blit's source region read-only (read-write when it is also the output's), and the command page read-only, at device addresses from `os_iommu_iova` in that order. A later operation that names other regions, or a region in another role, unmaps the mappings it does not use and maps the missing ones. The first attach of the lifetime installs the one directory (docs/iommu.md), and the record keeps both banks' prefetch words after it. |
| Update and task-end interrupt off, RCQ header address and length, interrupt on, update on | `g2d_mixer_apply`, g2d_mixer.c 996-1030 | 0x28 = 0, then the owner requires 0x28 bit 0 and 0x100 bit 31 to read clear; 0x20 = 0, 0x24 = 5 (clear) with bits 0 and 2 reading clear, 0x2c = command page address, 0x30 = 0, 0x34 = 128, 0x20 = 0x10, 0x28 = 1. The vendor writes the update bit to 0 before every job and never relies on the engine clearing it, so a second operation in one open does not depend on that. The record keeps 0x28 and 0x100 as the owner reads them after the interrupt. |
| Wait for task end, 100 ms per frame | `g2d_wait_cmd_finish`, g2d.c 778-800; `WAIT_CMD_TIME_MS` | The owner blocks on its task notification until the interrupt or 100,000 µs. A timeout disarms the handler and marks the service FAULT. The vendor's reset recovery (`g2d_bsp_reset`, g2d_top.c 86-93) runs in the close that follows the fault. |
| Interrupt: acknowledge task end, pulse the mixer AHB reset, wake | `g2d_handle_irq`, g2d.c 802-812; g2d_top.c 95-100, 133-140 | `os_g2d_irq`, built with -mgeneral-regs-only: 0x24 = 1, 0x08 bit 0 cleared then set, then the owner is notified. An interrupt with no operation armed, or with task end clear, masks INTID 83 at the distributor and fails the operation. |

An operation that fails with the engine idle and the IOMMU consistent is refused: the command build and the CPU cache maintenance before the launch and after the interrupt. The service stays open, the failing step and word are in the fault record, and the application's close detaches master 10 and gates the engine, so the session ends with a normal return. Any other failure marks the service FAULT: a busy engine before the launch, a launch register that does not read back, no interrupt within 100 ms, an interrupt without task end, an IOMMU fault, or an IOMMU attach or map that faulted. Then the engine may still write memory, or a register is in an unknown state.

A FAULT service gets one close, from `ss_g2d_close` or from its quiesce step ([the open's fault rule](#vendor-sequence-and-the-native-diff); docs/designs/failure-return-audit.md, "A faulted device service closes before it reports"). With the engine's bus clock and reset open, the close first stops the engine, before master 10 returns to bypass and could reach all of memory:

1. 0x20 bits 4 and 6 cleared (the interrupt enables), and INTID 83 disabled and observed disabled, as in every close.
2. 0x28 bit 0 cleared, the RCQ update off, as `g2d_mixer_apply` does before a job (g2d_mixer.c 1003).
3. 0x08 bits 0 and 1 cleared and then set, each read back: the mixer and rotator AHB resets pulsed, the vendor's recovery after a timeout (`g2d_wait_cmd_finish`, g2d.c 786; `g2d_bsp_reset`, g2d_top.c 86-93).
4. The engine reads idle: 0x28 bit 0 and 0x100 bit 31 (the mixer's start) clear.
5. Master 10 detached when the IOMMU owner records it attached. A first attach that faulted after its clock lease records the master attached before any of its writes, so the close detaches it although the service never saw the attach complete. The vendor's timeout path resets the IOMMU master only on other SoCs (g2d.c 789-792).
6. The module and bus gates and the resets, as in every close.

When every step reads back, the service is CLOSED, its quiesce step reports DONE and the session returns normally; `ss_g2d_fault` and the done record carry the failure. When a step fails (the engine does not read idle, or the detach fails, for example when the IOMMU owner cannot write its registers), the service stays FAULT with its gates, leases and mapping, its quiesce step reports site 0x931, and the session ends at the return's 3 s bound (site 0x742) with the watchdog reset, which stops the engine. [os-g2d-operation.py](../checks/os-g2d-operation.py) runs the close after a timeout, after a launch that faulted and after a faulted attach.

INTID 83 is enabled at the end of the open and disabled, and observed disabled, at the start of the close. The close then detaches master 10 before it gates the engine; the last detach disables both IOMMU banks and returns the IOMMU clock lease. The CPU caches are cleaned and invalidated over the 64-byte lines that the output surface touches before the operation, and again after it, so the application reads the engine's writes through its own mapping.

`ss_g2d_run` returns a `struct ss_g2d_done`: the 0x24 word the handler read before its acknowledgment, the 0x08 word it read before the reset pulse, the 0x24 word after, the IOMMU entry words of the first attach (also when it refused), master 10's fault words after the operation, the IOMMU owner's failure step, register offset and read-back value, the count of interrupts taken with no operation armed, the surface's and directory's physical addresses, and the system-counter times of the submission, the update bit, the interrupt and the end.
