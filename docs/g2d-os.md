# G2D in the OS image

The G2D 2D engine at 0x05440000 is an OS-image service. An application declares the `g2d` device in its manifest, the session grants it as `SS_DEVICE_G2D`, and the application calls the class interface in include/sunstone_g2d.h. One owner task on core 0 (apps/os/g2d.c, priority 4) makes every G2D, CCU and GIC access for the service. The G2D service writes no PCK600 or PMIC register: the shared power-domain owner (drivers/power/a733_power_domains.c) powers domain 1 (DE_SYS) for it.

Status: open, close, fill, and copy (unconverted, and ARGB8888 to BGRA8888), each with INTID 83 completion through IOMMU master 10, are implemented and qualified on port 3-1 (docs/evidence/display-g2d.jsonl). The first fill run failed before the launch, because the cache maintenance refused a surface size that is not a whole number of 64-byte lines; the range is now widened to whole lines, and the second run (`1794714427314630502-a44050c7`) wrote the predicted surface, CRC-32 0xa872b4d7, with one interrupt 11.5 µs after the launch. [os-g2d-operation.py](../checks/os-g2d-operation.py) runs the linked operation path of the image under emulation over every surface size and start class the stage-2 to stage-4 plans draw, with a refused cache maintenance, a timeout and four closes after a fault. A blit with conversion between the eight 32-bit RGB orders is implemented. Its first board run (stage 3, `1794722460072814405-2b77c3eb`) converted the first copy as predicted, with all four samples matching. The second copy failed its launch check at step 61, because 0x24 bit 2 (cfg_finish) read set: a job sets it, and neither a write of one nor the mixer reset clears it. The launch now requires only task end to read clear, as the vendor does. The second run (`1794723622887180087-445859c1`) completed both copies with every sample, register word and the source CRC as predicted. Its output CRC-32 was 0x50879a4a, not the predicted 0x289570d9: every copied pixel whose source alpha is 0 was written as 0x00000000, and every other pixel was copied exactly. Of the output models tried on the host, only that rule gives the board's CRC. The rule is now part of the reference, and the samples include an alpha-0 and an alpha-0xff pixel. The third run (`1794725199336568290-5c9e9da6`) met every prediction: output CRC-32 0x50879a4a, the source unchanged, all twelve samples, and launch to interrupt 130.5 µs for operation 1's 32,148 pixels and 36.4 µs for operation 2's 8,256. The blit treats alpha as coverage: no blender configuration copies an alpha-0 pixel exactly (run `1794732202569746479-c9b5e324`, [Copy paths](#copy-paths)). Scaling is not implemented yet.

## Vendor sequence and the native diff

The reference is the vendor driver in linux-orangepi 2ac08e8c (bsp/drivers/g2d/g2d_rcq, hashes in the source-identities.json beside the copy), the clock tree in ccu-sun60iw2.c and the device tree node g2d@5440000 in sun60iw2p1.dtsi.

| Vendor step | Reference | OS image |
| --- | --- | --- |
| Attach to PCK600 domain 1 before any clock or reset changes | genpd attach at probe; pck600_domains.c 104-155 | The start entry registers a domain-1 row before the scheduler starts. Open first waits for the owner's DCDC2 verification until `a733_power_supply_deadline`, then requests ON and blocks on the row's wake until `a733_power_row_deadline`: the PCK600 bound plus two ticks, 12,000 µs at the 1,000 Hz tick. Both are clipped to the open's deadline (include/a733_power_domains.h). The owner adopts an ON domain with no write. |
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

`ss_g2d_open` returns a `struct ss_g2d_info`: the 23 platform words before the first write and after the open (PLL and PERI0 control and gates, the G2D, DE_SYS, DE and DI clock words, the critical gates, the PPU clock, the domain-1 policy, status and delay words, the IOMMU gates, and the INTID 83 enable, pending and active words), the 11 engine-local words including the version register at 0x10, and the timing of the open. The timing covers the row request and the row's ON answer in the power owner's microsecond clock. The record also carries the domain-1 transition record that the owner published with the row's answer (`struct ss_g2d_transition`, the layout of `struct a733_power_transition`): the policy and status words before the request, the policy written, the first, last pending and target status words, the read and command counts, and the command and read times. `commands` is 0 when the owner adopted the domain. A refused row leaves the transition record zero. The record's magic is `G2DT`; images before the transition record wrote `G2DI`, which the decoder refuses. Bit 8 of the policy and status words is recorded separately and refuses nothing. apps/g2d-check/host.py decodes the record of the g2d-check application.

The open also records the supply verification. `verify_at_us` is the power owner's time at which verification may start, and `verified_seen_us` is the time the open first found DCDC2 verified. The power owner records no verification time of its own, so `verified_seen_us` is an upper bound on it. `settled_supply` is 1 when the start entry marked the supply settled.

## The supply verification wait

The power-domain owner verifies DCDC2 and DCDC4 once per boot. It starts A733_POWER_SUPPLY_SETTLE_US (5,160 µs) after its first step, which covers a 1,000 µs enable delay and a 1,040 mV ramp, unless a consumer calls `a733_power_domains_settled_supply()` before the service first runs. The G2D start entry makes that call, as the NPU startup does. The settle is not needed in the OS image for three reasons:

- **DCDC2 has no writer.** Every OS-image user of DCDC2 (G2D, the NPU, PCIe, the USB 2 platform) holds a shared rail lease and reads it.
- **DCDC4 has two writers, and each covers its own settle.** The GPU platform raises DCDC4 from 800 mV to 960 mV (drivers/gpu/a733_gpu_platform.c, RAISE_SUBMIT). It writes under an exclusive DCDC4 lease, releases the lease (RAISE_RELEASE), and then waits its own settle (RAISE_SETTLE) before the GPU's first ON. The verifier can therefore read DCDC4 during that settle. It then reads the new selector, which it accepts, and the only power domain on DCDC4 is the GPU's (domain 5), whose first ON waits for the raise's settle. The GPU performance service changes DCDC4 between operating points (drivers/gpu/a733_gpu_performance.c) only after A733_POWER_SUPPLY_DONE, when verification has finished.
- **Earlier writers finish before the image starts.** The vendor boot0 writes DCDC2 and DCDC4 at power-up, and an earlier GPU image of the same power lifetime can leave DCDC4 raised. Both finish before the runner uploads this image, which takes seconds.

The verifier reads each rail's enable bit and voltage selector, and passes only a rail enabled at an accepted selector: 800 mV for DCDC2, and 800 mV or 960 mV for DCDC4. It reads no output voltage, so it cannot detect a rail still ramping to its selector. The settle covers that case, and the three facts above show that no rail of the image ramps at the first step. G2D's power domain depends on DCDC2 only.

## Operations

`ss_g2d_run` runs one operation on the open engine. The application names its surfaces by large region, offset, width, height, stride and format; the owner maps each region an operation names for IOMMU master 10. Three operations are implemented:

- **Fill** writes a color into a rectangle of an ARGB8888 output.
- **Blit** copies a rectangle of a source surface into the output, converting from the source's format to the output's. The source and the output have the same width, height and stride, as the native builder (drivers/g2d/a733_g2d.c) requires; they may be the same region. A source pixel whose alpha byte is 0 is written as 0x00000000, and every other pixel is copied exactly (run `445859c1`). The vendor's `g2d_bsp_bitblt` programs the same alpha path:
  - pipe 0's fill colour is enabled, blender 0x00 bit 0, which every `bld_in_set` call sets (g2d_bld.c 97);
  - the fill colour at blender 0x10 is 0, in zeroed memory in both drivers;
  - alpha stays straight throughout: layer premultiply 0, `premulti_ctrl` 0, output `premul_en` 0.

  The blend function is the only difference: the vendor's SRCOVER (0x03010301) against the native COPY (0x00010001). With per-pixel alpha (layer alpha mode 0, as both drivers program it), pipe 1 is off in both, so neither blends with the destination. What a conversion writes to an X byte (formats 4 to 7) is not established on the board yet.

  The blit therefore treats alpha as coverage: a fully transparent pixel loses its colour. It suits composition and display, not data in which the alpha byte is a channel, such as camera frames or packed sensor words. No blender configuration tried copies such data exactly ([Copy paths](#copy-paths)), so an exact copy stays on the CPU until the rotate module qualifies.
- **Rotate** (`SS_G2D_ROTATE`) copies a rectangle of a source surface into the output, rotated by 0, 90, 180 or 270 degrees and optionally flipped (`SS_G2D_FLIP_H`, `SS_G2D_FLIP_V`), through the rotate module ([Rotate module](#rotate-module)), not the mixer. The source has its own width, height and stride, and the output has the source's format; the rotated rectangle (w and h swapped for 90 and 270) must lie inside the output. A fill or blit that names a degree or flip, and a rotate with a colour, a format change, another degree or a flip above 3, is refused before any mapping. The board has not run a rotate yet.

Before each launch the owner cleans and invalidates the whole 64-byte lines of the output and cleans those of a blit's source. After the interrupt it cleans and invalidates the output's lines again. [g2d-check-host.py](../checks/g2d-check-host.py) builds the g2d-check application on the host with a CPU model of the copies, and checks that its plan, samples and CRC-32 values equal the decoder's reference. Its two mutant engines copy without converting and copy alpha-0 pixels exactly. The decoder names each outcome, and the second fails both operations' alpha-0 samples.

| Vendor step | Reference | OS image |
| --- | --- | --- |
| Build the RCQ command blocks | `g2d_fillrectangle`, g2d_mixer.c 83-135; `g2d_vlayer_set`, `g2d_ovl_v_fc_set` | `a733_g2d_commands_at` (drivers/g2d/a733_g2d.c), the reviewed native builder with the caller's device addresses, into one non-cacheable 4 KiB command page. |
| Map the buffers | the IOMMU domain of the g2d node, `iommus = <&mmu_aw 10 1>` | `os_iommu_attach` (apps/os/iommu.h) at the first operation after an open: the output's large region read-write, a blit's source region read-only (read-write when it is also the output's), and the command page read-only, at device addresses from `os_iommu_iova` in that order. A later operation that names other regions, or a region in another role, unmaps the mappings it does not use and maps the missing ones. The first attach of the lifetime installs the one directory (docs/iommu.md), and the record keeps both banks' prefetch words after it. |
| Update and task-end interrupt off, RCQ header address and length, interrupt on, update on | `g2d_mixer_apply`, g2d_mixer.c 996-1030 | 0x28 = 0, then the owner requires 0x28 bit 0 and 0x100 bit 31 to read clear; 0x20 = 0, 0x24 = 1 (clear task end) with bit 0 reading clear, 0x2c = command page address, 0x30 = 0, 0x34 = 128, 0x20 = 0x10, 0x28 = 1. The vendor writes the update bit to 0 before every job and never relies on the engine clearing it, so a second operation in one open does not depend on that; on run `2b77c3eb` the bit read 0 after the job. cfg_finish (0x24 bit 2) stays set after a job, and a write of one does not clear it (run `2b77c3eb`). The vendor never clears or reads it (`g2d_top_rcq_cfg_irq_query`, g2d_top.c 142-149, has no caller), and its interrupt enable, 0x20 bit 6, is never set: `g2d_top_rcq_irq_en` sets only bit 4 (g2d_top.c 122-126), and the owner writes 0x20 = 0x10. The record keeps the 0x24 word read before the launch, and 0x28 and 0x100 as the owner reads them after the interrupt. |
| Wait for task end, 100 ms per frame | `g2d_wait_cmd_finish`, g2d.c 778-800; `WAIT_CMD_TIME_MS` | The owner blocks on its task notification until the interrupt or 100,000 µs. A timeout disarms the handler and marks the service FAULT. The vendor's reset recovery (`g2d_bsp_reset`, g2d_top.c 86-93) runs in the close that follows the fault. |
| Interrupt: acknowledge task end, pulse the mixer AHB reset, wake | `g2d_handle_irq`, g2d.c 802-812; g2d_top.c 95-100, 133-140 | `os_g2d_irq`, built with -mgeneral-regs-only: 0x24 = 1, 0x08 bit 0 cleared then set, then the owner is notified. An interrupt with no operation armed, or with task end clear, masks INTID 83 at the distributor and fails the operation. |

An operation that fails with the engine idle and the IOMMU consistent is refused: the command build and the CPU cache maintenance before the launch and after the interrupt. The service stays open, the failing step and word are in the fault record, and the application's close detaches master 10 and gates the engine, so the session ends with a normal return. A run on a service that is not open is refused with nothing recorded, so a fault that the quiesce step may already have reported keeps its step and word. Any other failure marks the service FAULT: a busy engine before the launch, a launch register that does not read back, no interrupt within 100 ms, an interrupt without task end, an IOMMU fault, or an IOMMU attach or map that faulted. Then the engine may still write memory, or a register is in an unknown state.

A FAULT service gets one close, from `ss_g2d_close` or from its quiesce step ([the open's fault rule](#vendor-sequence-and-the-native-diff); docs/designs/failure-return-audit.md, "A faulted device service closes before it reports"). With the engine's bus clock and reset open, the close first stops the engine, before master 10 returns to bypass and could reach all of memory:

1. 0x20 bits 4 and 6 cleared (the interrupt enables), and INTID 83 disabled and observed disabled, as in every close.
2. 0x28 bit 0 cleared, the RCQ update off, as `g2d_mixer_apply` does before a job (g2d_mixer.c 1003).
3. 0x08 bits 0 and 1 cleared and then set, each read back: the mixer and rotator AHB resets pulsed, the vendor's recovery after a timeout (`g2d_wait_cmd_finish`, g2d.c 786; `g2d_bsp_reset`, g2d_top.c 86-93).
4. The engine reads idle: 0x28 bit 0 and 0x100 bit 31 (the mixer's start) clear.
5. Master 10 detached when the IOMMU owner records it attached. A first attach that faulted after its clock lease records the master attached before any of its writes, so the close detaches it although the service never saw the attach complete. The vendor's timeout path resets the IOMMU master only on other SoCs (g2d.c 789-792).
6. The module and bus gates and the resets, as in every close.

When every step reads back, the service is CLOSED, its quiesce step reports DONE and the session returns normally; `ss_g2d_fault` and the done record carry the failure. When a step fails (the engine does not read idle, or the detach fails, for example when the IOMMU owner cannot write its registers), the service stays FAULT with its gates, leases and mapping, its quiesce step reports site 0x931, and the session ends at the return's 3 s bound (site 0x742) with the watchdog reset, which stops the engine. [os-g2d-operation.py](../checks/os-g2d-operation.py) runs the close after a timeout, after a launch that faulted and after a faulted attach.

INTID 83 is enabled at the end of the open and disabled, and observed disabled, at the start of the close. The close then detaches master 10 before it gates the engine; the last detach disables both IOMMU banks and returns the IOMMU clock lease. The CPU caches are cleaned and invalidated over the 64-byte lines that the output surface touches before the operation, and again after it, so the application reads the engine's writes through its own mapping.

`ss_g2d_run` returns a `struct ss_g2d_done`: the 0x24 word the handler read before its acknowledgment, the 0x08 word it read before the reset pulse, the 0x24 word after, the 0x24 word the owner read before the launch, the IOMMU entry words of the first attach (also when it refused), master 10's fault words after the operation, the IOMMU owner's failure step, register offset and read-back value, the count of interrupts taken with no operation armed, the surface's and directory's physical addresses, and the system-counter times of the submission, the update bit, the interrupt and the end. A rotate adds the rotate module's words: ROT_CTL after the register writes and after the finish, the ROT_INT word the handler read, ROT_INT after its write-back, and ROT_TIMEOUT. Every operation records the stray finishes its handler masked ([Rotate module](#rotate-module)) and the first stray's status word.

## Copy paths

The blit writes a source pixel whose alpha byte is 0 as 0x00000000 (run `1794723622887180087-445859c1`). An experiment looked for a blender configuration that copies such pixels exactly, and found none.

Run `1794732202569746479-c9b5e324` (image 8e5cdd4c6929, source at 0da3883fe) copied one 96 by 40 ARGB8888 rectangle, unconverted, five times in one session. Each copy used different blender words, into its own output rectangle. Each copy changed only the page words listed below from the vendor path's, and os-g2d-operation.py checked that on the host at that revision. It still checks that every copy and fill page equals the qualified image's, by SHA-256. The rectangle holds 11 pixels with alpha 0.

| Copy | Page words that differ from the vendor path's | Source of the words | Result |
| --- | --- | --- | --- |
| Vendor path | none | `g2d_bsp_bitblt` with G2D_BLT_NONE | alpha-0 pixels 0x00000000, every other pixel exact |
| Markers | blender 0x10 (pipe 0's fill colour) 0x00a1b2c3, blender 0x44 (background) 0x00d4e5f6 | the vendor path with a marker in each colour that could fill an alpha-0 pixel | the same as the vendor path: neither marker appears |
| Premultiplied | video layer 0x00 0xff010001 (`lay_premul_ctl` 1), blender 0x40 (`premulti_ctrl`) 1, blender 0x60 (`out_color`) 1 | the vendor's `bpremul` words, which it never runs because `g2d_image_check` clears `bpremul` (g2d.c 503) | colour multiplied by alpha: 0x5602541b became 0x56011d09, and alpha-0 pixels 0x00000000 |
| ROP copy | blender 0x80 (`rop_ctrl`) 0, blender 0x84 (`ch3_index0`) 0x61082 | `bld_rop2_set`'s `G2D_BLT_COPYPEN` encoding (g2d_bld.c 447-530), which the vendor never programs | the same as the vendor path |
| No fill | blender 0x00 (`bld_en_ctrl`) 0x100 | pipe 0 with `p0_fcen` cleared, which no vendor path does | the same as the vendor path |

So the zero comes from neither pipe 0's fill colour, the background nor the blend ROP. The layer and blender alpha path drops the colour of a pixel with alpha 0 in every configuration tried. The premultiplied words leave the output premultiplied. Every register word, interrupt and IOMMU word was as on the stage-3 runs, and every copy completed in 19 to 21 µs.

The candidates are removed. The rotate module (`g2d_rotate_set_para`, reached by the vendor's `g2d_blit_h` with flag G2D_ROT_0) bypasses the blender, and it is the next candidate for an exact copy.

## Rotate module

The G2D's rotate module copies a rectangle with a rotation of 0, 90, 180 or 270 degrees and optional horizontal and vertical flips, without the blender. The service runs it as the rotate operation ([Operations](#operations)). The reference is `g2d_rotate_set_para` in g2d_rotate.c (linux-orangepi 2ac08e8c, bsp/drivers/g2d/g2d_rcq; the board kernel builds it, CONFIG_G2D_ROTATE=y). The vendor reaches it from a blit whose flag sets any of bits 8-15 (`g2d_blit_h`, g2d.c 717-728), under the same mutex as the mixer.

The vendor programs the module by direct register writes at G2D + 0x28000 (0x05468000), not through the mixer's RCQ command page. The image maps that address as Device-nGnRnE: the first gigabyte is one device block in the kernel's level-1 table (platform/a733/platform.c), and every task's table copies it. [a733_g2d_rotate_words](../drivers/g2d/a733_g2d_rotate.c) gives a job's 24 words in the vendor's order:
- the control word: mode 1, the degree field in bits 4-5 (0 to 3 for 0 to 270), the vertical flip in bit 6 and the horizontal flip in bit 7;
- the input format, the input size ((h - 1) << 16 | (w - 1), 13 bits each), three pitches and three plane addresses with their high bytes;
- the output size, with w and h swapped for 90 and 270, three pitches and three addresses;
- ROT_INT 0x10000, the finish-interrupt enable;
- the control word again with the start bit, bit 31.

The rotator does not convert formats. The vendor writes the second and third plane addresses for a single-plane format from the buffer layout on its dma-buf path, and from the caller on its physical-address path; the builder writes 0, as for a caller that gives none. The vendor refuses an input or output origin that is not 4-byte aligned and an output pitch that is not 8-byte aligned, but only after writing every register before the check. The builder refuses those jobs before any write, and also every job whose fields would overflow: w or h of 0 or above 8192, a rectangle wider than its stride, or an origin at or above 2^40. [g2d-rotate-words.py](../checks/g2d-rotate-words.py) compares the builder with a transcription of the vendor function, word for word, over 240 jobs: four degrees, four flip settings, three formats and five geometries, including a 40-bit address. The transcription was compared once with the vendor function itself, compiled on the host with its register writes recorded, over the same jobs and the three vendor refusals.

The rotator shares everything else with the mixer:
- **Gates and resets.** `g2d_bsp_open` sets the rotator's sclk and hclk gates and its AHB reset (bit 1 of 0x00, 0x04 and 0x08) with the mixer's, as the service's open does. The module clock, bus gate, DE_SYS reset and the PCK600 domain belong to the whole G2D.
- **IOMMU.** The device tree gives the G2D one IOMMU master, `iommus = <&mmu_aw 10 1>`.
- **Interrupt and timeout.** `g2d_handle_irq` (g2d.c 802-832) serves INTID 83 for both blocks: the mixer's task end first, then ROT_INT bit 0, which it clears by writing the read word back, with no reset pulse. The timeout is the mixer's 100 ms, after which `g2d_bsp_reset` pulses both AHB resets.

The service runs one operation at a time, so its handler dispatches by the kind the owner armed ([a733_g2d_irq.h](../include/a733_g2d_irq.h)). Only the armed kind's finish completes the operation. A finish of the other kind is a stray. The line is level-triggered, so a stray left raising it would re-enter the handler until the armed operation ended. The handler therefore masks the stray at its block: it clears 0x20 bit 4 for the mixer, or ROT_INT bit 16 for the rotator, writing 0 to the status bit so that the status stays set for the close to read. It counts the stray, records its word and leaves the operation armed. A finish counts only while its enable is set, so a masked stray is not counted again. The next launch of that kind writes its enable again.

[g2d-irq-dispatch.py](../checks/g2d-irq-dispatch.py) checks the dispatch in two ways:
- for every armed kind, both finish bits, both enables and the status words' other bits;
- in a level-triggered model of the line that counts handler entries, with a stray before, at and after the armed finish. A stray costs one entry. With the stray's enable left on, the model re-enters the handler at every step until the finish, and the check fails.

A rotate runs as follows. The owner refuses a rectangle that leaves its surface, maps the regions as for a blit (the source read-only), builds the 24 words and does the cache maintenance of a blit. It then writes ROT_INT = 1, which clears the finish status and leaves the enable off, and requires bit 0 to read clear. It writes the first 22 words and requires ROT_CTL to read back the control word. Then it arms the handler for a rotate, writes the enable and the start word, and waits for the finish as for the mixer, 100 ms. The handler writes the finish word back. The owner then requires ROT_INT bit 0 to read clear, and records ROT_CTL, ROT_TIMEOUT and 0x24. It touches no mixer register, so the mixer's reset is not pulsed.

[os-g2d-operation.py](../checks/os-g2d-operation.py) runs the image's own rotate path and handler against a register model of the rotate module and the shared line. It checks:
- the four degrees into disjoint rectangles, as the first board stage draws them, and each flip setting: each must write the status clear, then exactly the vendor's words at the service's device addresses, then the handler's write-back, and no mixer register;
- a fill, a rotate, a blit and a rotate in one session;
- a mixer stray during a rotate and a rotator stray during a fill: each is masked, counted once with its word, and costs one handler entry;
- a rotate that never finishes, which faults at step 62 with the ROT_INT word and then closes;
- each refusal, which must touch no register, mapping or cache line.

Images with the stray's enable left on, with no write-back of the finish, or with a rotate armed as a mixer operation fail it.

### First rotate stage

[g2d-rotate](../apps/g2d-rotate/main.c) rotates one 96 by 40 ARGB8888 rectangle of the "source" region at (24, 16) by 0, 180, 90 and 270 degrees, in that order, into four disjoint rectangles of the "surface" region. Both surfaces are 400 by 260 at stride 1664 and are filled from seed 0x3c6ef372. After each rotate the application records the output rectangle's CRC-32, its four corners and the pixel before it. It stops at the first rotate that fails. The rectangle holds 11 pixels with alpha 0, so an exact copy and the blender's zeroing give different CRC-32 values.

[The decoder](../apps/g2d-rotate/host.py) names the model that each rectangle matches: a placement (identity, half turn, a mirror, clockwise, counter-clockwise or a transpose) with a pixel rule (exact, zeroed or opaque), or the background left unchanged. All 13 candidates of each rotate have distinct CRC-32 values. [g2d-rotate-host.py](../checks/g2d-rotate-host.py) builds the application on the host and checks the decoder against a CPU rotate and three mutant engines.

The stage measures what the vendor source does not settle:
- which way degree field 1 (90) turns;
- whether ROT_CTL bit 31 clears itself;
- whether a rotate sets anything in 0x24;
- whether clearing ROT_INT bit 16 lowers the rotator's part of the line.
