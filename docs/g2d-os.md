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

`ss_g2d_run` runs one operation on the open engine. The application names its surfaces by large region, offset, width, height, stride and format; the owner maps each region an operation names for IOMMU master 10. Four operations are implemented:

- **Fill** writes a color into a rectangle of an ARGB8888 output.
- **Blit** copies a rectangle of a source surface into the output, converting from the source's format to the output's. The source and the output have the same width, height and stride, as the native builder (drivers/g2d/a733_g2d.c) requires; they may be the same region. A source pixel whose alpha byte is 0 is written as 0x00000000, and every other pixel is copied exactly (run `445859c1`). The vendor's `g2d_bsp_bitblt` programs the same alpha path:
  - pipe 0's fill colour is enabled, blender 0x00 bit 0, which every `bld_in_set` call sets (g2d_bld.c 97);
  - the fill colour at blender 0x10 is 0, in zeroed memory in both drivers;
  - alpha stays straight throughout: layer premultiply 0, `premulti_ctrl` 0, output `premul_en` 0.

  The blend function is the only difference: the vendor's SRCOVER (0x03010301) against the native COPY (0x00010001). With per-pixel alpha (layer alpha mode 0, as both drivers program it), pipe 1 is off in both, so neither blends with the destination. What a conversion writes to an X byte (formats 4 to 7) is not established on the board yet.

  The blit therefore treats alpha as coverage: a fully transparent pixel loses its colour. It suits composition and display, not data in which the alpha byte is a channel, such as camera frames or packed sensor words. No blender configuration tried copies such data exactly ([Copy paths](#copy-paths)). The rotate operation at 0 degrees does (run `1794743298175050163-4ed14c80`, [First rotate stage](#first-rotate-stage)).
- **Rotate** (`SS_G2D_ROTATE`) copies a rectangle of a source surface into the output, rotated by 0, 90, 180 or 270 degrees and optionally flipped (`SS_G2D_FLIP_H`, `SS_G2D_FLIP_V`), through the rotate module ([Rotate module](#rotate-module)), not the mixer. The source has its own width, height and stride, and the output has the source's format; the rotated rectangle (w and h swapped for 90 and 270) must lie inside the output. A fill or blit that names a degree or flip, and a rotate with a colour, a format change, another degree or a flip above 3, is refused before any mapping. Every pixel is copied exactly, alpha-0 pixels included, at all four degrees (run `1794743298175050163-4ed14c80`); 90 degrees turns clockwise.
- **Convert** (`SS_G2D_CONVERT`) converts a rectangle of an NV16 camera frame to RGB, scaling it to the output rectangle's size, through the mixer's video layer and scaler ([Camera-frame conversion](#camera-frame-conversion)). The job names one of four YUV-to-RGB tables in `csc`. The output has one of the eight RGB formats and alpha 0xff.

Choose the copy by what the alpha byte means:
- **Exact copy.** Use a rotate at 0 degrees for data whose alpha byte is a channel, such as camera frames or packed sensor words. It copies every byte. No separate copy operation exists. The rotate module adds three rules: the output stride is a multiple of 8 bytes, both rectangles start 4-byte aligned (every surface offset and stride is), and the output has the source's format. A job that breaks the stride rule is refused at step 50 with value 3, before any engine register write.
- **Blit.** Use a blit where alpha is coverage, as in composition and display. It writes an alpha-0 pixel as 0x00000000, and it converts between the RGB formats.

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

[g2d-rotate](../apps/g2d-rotate/main.c) rotates one 96 by 40 ARGB8888 rectangle of the "source" region at (24, 16) by 0, 180, 90 and 270 degrees, in that order, into four disjoint rectangles of the "surface" region. Both surfaces are 400 by 260 at stride 1664 and are filled from seed 0x3c6ef372. After each rotate the application records the output rectangle's CRC-32, its four corners and the pixel before it. It does so after a rotate that fails as well, so the record shows whether the engine wrote the rectangle, and then it stops. The rectangle holds 11 pixels with alpha 0, so an exact copy and the blender's zeroing give different CRC-32 values.

[The decoder](../apps/g2d-rotate/host.py) names the model that each rectangle matches: a placement (identity, half turn, a mirror, clockwise, counter-clockwise or a transpose) with a pixel rule (exact, zeroed or opaque), or the background left unchanged. All 13 candidates of each rotate have distinct CRC-32 values, and a rectangle that matches none is reported as unmatched. [g2d-rotate-host.py](../checks/g2d-rotate-host.py) builds the application on the host and checks the decoder against a CPU rotate and three mutant engines.

Run `1794743298175050163-4ed14c80` (port 3-1, image 7a078999890f, built at 3a0666e7b) met every prediction:

| Rotate | Degrees | ROT_CTL | Model | Rectangle CRC-32 | Launch to interrupt |
| --- | --- | --- | --- | --- | --- |
| 1 | 0 | 0x00000001 | identity/exact | 0x02afc4e4 | 23.5 µs |
| 2 | 180 | 0x00000021 | rot180/exact | 0x60c144df | 16.3 µs |
| 3 | 90 | 0x00000011 | cw/exact | 0xb9fa3207 | 18.4 µs |
| 4 | 270 | 0x00000031 | ccw/exact | 0xc1e60d6c | 18.1 µs |

The output CRC-32 was 0x5e1e0cf1, the one that follows from the four models, so nothing was written outside the rectangles. The source stayed 0x7c121e04. Each corner sample and each pixel before a rectangle matched. The run settled what the vendor source does not:
- **Direction.** Degree field 1 (90) turns clockwise: output pixel (row r, column c) is source pixel (h - 1 - c, r). Field 3 (270) turns counter-clockwise.
- **Exact copy.** The 11 alpha-0 pixels were copied exactly at every degree. The blit writes them as 0x00000000 ([Copy paths](#copy-paths)), so the rotate at 0 degrees is the exact copy.
- **Start bit.** ROT_CTL read back without bit 31 after each finish. The run does not tell a start bit that clears itself from one that never reads back as 1, so a busy check on bit 31 waits for a read during a running job.
- **Other words.** ROT_TIMEOUT read 0. The mixer's 0x24 read 0 after every rotate: a rotate sets nothing there.
- **Interrupt.** The handler read ROT_INT 0x00010001 each time, and 0x00010000 after its write-back. irq_count went from 1 to 4, with no stray.

The open and close words equal stage 5's (run `1794732202569746479-c9b5e324`). The stage runs no mixer operation, so no stray occurs. Whether clearing ROT_INT bit 16 lowers the rotator's part of the line stays model-only until a stage that runs a mixer job and a rotate together.

## Camera-frame conversion

A camera delivers YUV frames, and a model takes RGB input at a fixed size. The convert operation does both steps in one G2D job: it reads a 4:2:2 semi-planar frame, scales it with the video scaler and converts it to RGB in the blender.

### Source format and the job

An NV16 surface (`SS_G2D_FORMAT_NV16`) has two planes in one large region:
- the luma plane at `offset`, one byte a pixel, `stride` bytes a row (at least `width`);
- the chroma plane at `chroma_offset`, one U and V pair for every two pixels of each row, `chroma_stride` bytes a row (at least `width` rounded up to even).

Both planes start 4-byte aligned, and both strides are multiples of 4. An RGB surface leaves `chroma_offset` and `chroma_stride` 0. The convert reads the `w` by `h` rectangle at (`sx`, `sy`) and writes the `ow` by `oh` rectangle at (`dx`, `dy`); `sx` is even, so no chroma pair is split. Every size is 1 to 8192. The service refuses before any mapping:
- at step 48: a table outside 1 to 4, an odd `sx`, a source other than NV16, a degree, flip or colour, `ow`, `oh` or `csc` set on any other operation, and a nonzero `reserved` word;
- at step 49: an NV16 output, chroma fields on an RGB surface, and a chroma plane outside its region or narrower than the frame;
- at step 50: a rectangle that leaves its surface.

The vendor's format code 0x24, G2D_FORMAT_YUV422UVC_V1U1V0U0, names the bytes of a chroma word from the most significant, as its NV24 code 0x31 (V1U1V0U0) does. In memory the pairs are then U0 V0 U1 V1, which is NV16; code 0x25 is NV61. The first conversion stage measures the byte order.

The four tables (`SS_G2D_CSC_*`) name the source's colour space, and the RGB output is full range:

| Table | Source | Y coefficient |
| --- | --- | --- |
| `SS_G2D_CSC_BT601_LIMITED` | BT.601, Y 16 to 235, chroma 16 to 240 | 255/219, 1.164 (1192) |
| `SS_G2D_CSC_BT601_FULL` | BT.601, Y and chroma 0 to 255 | 1.0 (1024) |
| `SS_G2D_CSC_BT709_LIMITED` | BT.709, Y 16 to 235, chroma 16 to 240 | 255/219, 1.164 (1192) |
| `SS_G2D_CSC_BT709_FULL` | BT.709, Y and chroma 0 to 255 | 1.0 (1024) |

The converter computes each channel as (cy Y + cu U + cv V + k) >> 10, clamped to 0 to 255, with signed 13-bit coefficients and a signed 20-bit constant. [g2d-csc-tables.py](../tools/g2d-csc-tables.py) derives the four tables from the BT.601 and BT.709 matrices (ITU-R BT.601-7 section 2.5, ITU-R BT.709-6 items 3.2, 3.3 and 4.6, and ITU-T H.273 for full range) into [a733_g2d_csc_tables.h](../include/a733_g2d_csc_tables.h). Each coefficient is the real one times 1024, rounded to nearest. Each constant cancels the luma offset and both chroma offsets, and adds 512 so that the converter's truncating shift rounds to nearest.

### Vendor conversion path and the native diff

The reference is `g2d_bsp_bitblt` with flag G2D_BLT_NONE (g2d_mixer.c 304-371 in linux-orangepi 2ac08e8c, bsp/drivers/g2d/g2d_rcq) for a YUV source and an RGB output. [a733_g2d_convert_commands](../drivers/g2d/a733_g2d_convert.c) writes the same register images into the mixer's command page, with the block headers and their order of `a733_g2d_commands_at`:
- **Video layer V0** (`g2d_vlayer_set`, g2d_ovl_v.c 244-335): the format code, both plane addresses and pitches, and the rectangle.
- **Coarse decimation** (`g2d_ovl_v_calc_coarse`): applied when `w` is at least 8 times `ow` or `h` at least 4 times `oh`, with the scaler's input width capped at 2048.
- **Video scaler** (`g2d_vsu_para_set`, g2d_scal.c), enabled for every YUV source, also unscaled: control word 0x10101; luma steps at full rate; the chroma horizontal step half the luma's and the chroma vertical step the luma's. The filter banks come from `lan2coefftab32_full` through `fir_coef` (g2d_scal.c 248-268) for the luma horizontal, chroma horizontal and luma vertical steps, and every phase starts at 0.
- **Blender**: source-over (0x03010301), ROP 0xf0, `ch3_index0` 0x41000, pipe 0 on (0x101). The colour conversion follows the vendor's other path, `g2d_bsp_bld` (g2d_mixer.c 216-220 and 287): `bld_csc_reg_set(0, ...)` loads the pipe-0 input converter csc0 (control 1, fill colours 0x00108080, the table's 12 words at blender + 0x110), and `bld_cs_set` with an RGB format sets `out_color` 0, so the blender works in RGB. When the blender works in YUV and csc2 converts its output, as `g2d_bsp_bitblt` sets it, luma below 16 reaches the table as 16 ([second conversion stage](#second-conversion-stage)).
- **Write-back** (`g2d_wb_set`): the output's format, address, pitch and size.

In the vendor driver the csc0 path never runs for a real frame: g2d_mixer.c 202 reads `if (src->resize.w == 0 || src->resize.h)`, which refuses every job with a nonzero height. The builder takes only its conversion order; the scaler words come from g2d_scal.c, which the vendor's bitblt path reaches.

The vendor's YUV-to-RGB blocks (`Ycbcr2rgb_601` and `Ycbcr2rgb_709`, g2d_bld.c 37-50 and 66-79) differ from the derived tables, and their labels do not match their contents. [g2d-csc-tables.py](../checks/g2d-csc-tables.py) compares each derived table with the block that gives full-range RGB from the same source range, at neutral chroma (U = V = 128):

| Table | Vendor block | Vendor's grey for Y 0, 16, 128, 235, 255 | Derived table's grey |
| --- | --- | --- | --- |
| BT.601 limited | `Ycbcr2rgb_601` words 0 to 11 | 0, 0, 130, 254, 255 | 0, 0, 130, 255, 255 |
| BT.601 full | `Ycbcr2rgb_601` words 24 to 35 | 0, 15, 127, 234, 254 | 0, 16, 128, 235, 255 |
| BT.709 limited | `Ycbcr2rgb_709` words 0 to 11 | 0, 0, 130, 254, 255 | 0, 0, 130, 255, 255 |
| BT.709 full | `Ycbcr2rgb_709` words 24 to 35 | 0, 0, 112, 219, 239 | 0, 16, 128, 235, 255 |

The vendor's BT.709 full block subtracts 16 from luma: its constants are 16 times 1024 lower than its chroma coefficients cancel, so full-range BT.709 frames lose luma 0 to 16 and every value above 239. Its other blocks have no rounding term, so the truncating shift lowers most outputs by up to 1; the BT.601 full block's constant is also 1 lower than its chroma coefficients cancel, so grey Y becomes Y - 1. The derived and vendor tables differ in the constants of all four tables and in the BT.601 full table's G coefficients (-352 and -731 against the vendor's -353 and -732).

The builder writes nothing for a job that the vendor refuses or that the registers cannot hold:
- a format other than 0x24 and 0x25, an output format above 7, or an unknown table;
- a size of 0 or above 8192, or an odd `sx`;
- a stride narrower than its rectangle, or an address that is not 4-byte aligned;
- a rectangle whose first byte is at or above 2^32.

[g2d-convert-words.py](../checks/g2d-convert-words.py) compares the builder with a Python transcription of the vendor functions and the derived tables, word for word, over 46 jobs: the stage's geometries, both coarse-decimation boundaries, the 2048 limit, odd widths, offsets, both format codes, the eight output formats and the four tables. It also checks 22 refusals, that the bank equals a733_g2d.c's, and that every phase's four taps sum to 64.

The service runs a convert as a blit. It maps the output read-write and the source read-only, builds the page at the planes' device addresses, cleans and invalidates the output's lines, and cleans the luma plane's and then the chroma plane's lines. It launches the mixer with the blit's sequence and, after the interrupt, cleans and invalidates the output's lines again. A builder refusal after the mapping fails at step 50 with value 3, and a plane at or above 4 GiB in device space at step 50 with value 4. [os-g2d-operation.py](../checks/os-g2d-operation.py) runs the image's convert path with three converts in one session (unscaled, 224 by 224, and an offset window scaled 2:1 into a BGRA output). Each command page must equal the transcription's words at the device addresses the service used, with the plane maintenance in that order. It also runs 15 refusals of jobs with their own surfaces, each of which must touch nothing, among them a convert and a blit with a nonzero `reserved` word.

### First conversion stage

[g2d-convert](../apps/g2d-convert/main.c) runs four operations into the "surface" region, each output row with 64 bytes of padding, every output filled with 0x5a bytes first:

| Operation | Frame | Table | Output | Stride |
| --- | --- | --- | --- | --- |
| 1 | A, 640 by 480 | BT.601 limited | 640 by 480 | 2624 |
| 2 | A | BT.709 full | 640 by 480 | 2624 |
| 3 | B, 640 by 480 | BT.601 limited | 320 by 240 | 1344 |
| 4 | B | BT.601 limited | 224 by 224 | 960 |

Frame A, from seed 0x9e3779b9, has random luma. Its chroma is constant along each row in the left half and random pair by pair in the right half. Pixels 8 to 311 of every row form the exact region: an odd pixel's chroma is interpolated from its neighbouring pairs, and with constant chroma the interpolation returns the row's pair unchanged. There the output follows from the table, the pixel's luma and the row's chroma under a rounding rule and a byte order. The application counts mismatches against four candidate rules: rounding to nearest or down, each with U or V first. It stops before the scales when either conversion matches no candidate.

Frame B is one flat colour (Y 150, U 90, V 170) in its left half and a luma ramp from 16 to 235 over neutral chroma (128, 128) in its right half, the same on every row. For each scale the application counts the flat region's words that differ from the flat colour converted, the rows that differ from the middle row, and the steps where the ramp's R, G or B decreases.

The record keeps each output rectangle's CRC-32, the exact region's and the padding's, the alpha bytes, two whole rows of each conversion and the middle row of each scale, and each operation's launch-to-interrupt time. After the engine is closed, one A76 core converts frame A with the BT.601 limited table three times in C, as the CPU reference. That code is scalar integer C built with -O2 -march=armv8.1-a+lse -mgeneral-regs-only -mtune=cortex-a55, with no NEON, at the approved CPU-B point of 2002 MHz.

[The decoder](../apps/g2d-convert/host.py) rebuilds both frames and names, for each conversion, which of 32 models the exact region matches: each of the eight vendor blocks under each of the four rules. In the sampled rows it names the chroma interpolation (the scaler's half-phase taps -4, 36, 36 and -4 over 64, the nearest pair, or linear). For the scales it applies a model of the scaler and reports the largest difference over the ramp. It reports the engine's pixel rate in Mpixel/s over the input and the output pixels, and the CPU's over the frame. [g2d-convert-host.py](../checks/g2d-convert-host.py) builds the application on the host with a CPU model of the engine and checks the decoder against it and five mutant engines: rounding down, V first, uninterpolated chroma, a missed pixel and a scale that times out.

Run `1794749218366895381-6f184f31` (port 3-1, image 31ab76340302, built at 635fef995) missed the prediction and stopped before the scales, as the stop rule requires. Both conversions completed, and no candidate rule matched them:

| Conversion | Table | Rectangle CRC-32 | Mismatches in the exact region (of 145,920 pixels): nearest/uv, down/uv, nearest/vu, down/vu | Launch to interrupt |
| --- | --- | --- | --- | --- |
| 1 | BT.601 limited | 0xa04060fd | 103,385; 8,971; 144,326; 144,204 | 1050.5 µs |
| 2 | BT.709 full | 0x5676719f | 105,391; 8,973; 145,008; 144,559 | 1049.1 µs |

Every rectangle word had alpha 0xff, and the row padding and the source were unchanged. The open and the close returned 1, the close gated the engine (CCU words 0) and the session returned normally in 22.4 s.

One model of the engine reproduces both 640 by 480 rectangles: its CRC-32 values equal the board's.
- **Luma floor.** Y below 16 is raised to 16 before the table, for both tables, the full-range one included. There is no upper luma limit and no chroma limit. This accounts for every down/uv mismatch: the model differs from plain rounding down at 8,971 and 8,973 pixels, the counts the application measured.
- **Rounding.** The table's result is rounded down (the `>> 10` without the 512 term).
- **Byte order.** U is the first byte of each chroma pair, so code 0x24 is NV16.
- **Chroma interpolation.** An odd pixel's chroma is the scaler's half phase, (-4, 36, 36, -4) over 64 rounded to nearest, over the neighbouring pairs. The last pixel of a row (x = 639) takes its own pair.

The model was derived from the run's records: the exact region's mismatch counts, its CRC-32 (0xa2d55262 and 0x24b4dc15, equal to the model's) and the two sampled rows of each conversion. It has not been tested on a second frame. A full-range camera frame therefore loses luma 0 to 15. The [second conversion stage](#second-conversion-stage) tests the model on a new frame and measures which block applies the floor.

The engine converted 640 by 480 pixels in 1050 µs, 292 Mpixel/s at a module clock of 300 MHz. One A76 core at 2002 MHz took 2718 µs on the first run and 2696 and 2697 µs on the next two, 114 Mpixel/s, with the predicted rule rather than the engine's. That reference is scalar integer C without NEON, so it is a lower bound on the CPU's rate: the engine is 2.6 times faster than scalar C. Whether the engine is faster than a NEON conversion is not measured, and the 2:1 and 224 by 224 scales did not run.

### Second conversion stage

The second stage tested the engine model of the first stage on a new frame, measured which block raises luma below 16, and ran the two scales. It programmed the vendor's tables.

No register in the vendor's mixer layout (g2d_mixer_type.h) is named for a range, a clip or a clamp. These settings of the vendor's page could produce the floor or the rounding:
- **Fill colours.** `bld_csc_reg_set` writes both blender fill colours as 0x00108080, luma 16 with neutral chroma (g2d_bld.c 257-258), and `bld_in_set` turns pipe 0's fill colour on (`p0_fcen`, g2d_bld.c 97).
- **Blending in YUV.** With a YUV source, `bld_cs_set` sets the blender's output colour to YUV (g2d_bld.c 227-230), so the blender works on luma before csc2 converts it. The vendor's other path, `g2d_bsp_bld`, converts at pipe 0's input with csc0 (g2d_mixer.c 216-220) and blends in RGB (line 287); line 202 makes it unreachable.
- **Write-back rounding.** The write-back's format word has a `round_en` bit (bit 8, g2d_mixer_type.h 508), which the vendor never sets.

The stage's image, built at 59c11057a with the build option `SUNSTONE_G2D_CONVERT_VARIANTS`, accepted a convert's diagnostic variant in the job's `reserved` word. Each bit changed one setting after the vendor's words:

| Variant | Bit | Change to the vendor's page | Expected rule if this setting causes the effect |
| --- | --- | --- | --- |
| B | 1 | both fill colours 0x00008080 (luma 0) | luma kept, rounded down |
| C | 2 | pipe 0's fill colour off (`bld_en_ctrl` 0x100) | luma kept, rounded down |
| D | 4 | csc0 at blender + 0x110 converts at pipe 0's input, `cs_ctrl` 1, `out_color` 0, csc2 zero | luma kept, rounded down |
| E | 8 | write-back `round_en` | luma raised, rounded to nearest |

The builder no longer has the option or the variants: the [third conversion stage](#third-conversion-stage) makes the csc0 order the only path, and every image refuses a nonzero `reserved` word at step 48.

The stage's application ran seven operations into the "surface" region, each output filled with 0x5a bytes first:

| Operation | Frame | Table | Variant | Output | Stride |
| --- | --- | --- | --- | --- | --- |
| A | luma sweep, 640 by 480 | BT.709 full | none | 640 by 480 | 2624 |
| B to E | luma sweep | BT.709 full | B, C, D, E | 640 by 480 | 2624 |
| scale 1 | first stage's B, 640 by 480 | BT.601 limited | none | 320 by 240 | 1344 |
| scale 2 | first stage's B | BT.601 limited | none | 224 by 224 | 960 |

The luma-sweep frame, from seed 0x6a09e667, has the first stage's frame A layout with random luma, except that every fourth row y steps through every luma value across the exact region, (x - 8 + y) mod 256 at pixel x. Every value from 0 to 255 therefore appears in the exact region, the values below 16 and above 234 included. The scales read the first stage's frame B, its flat colour and luma ramp (the scale frame). The application counted the exact region's mismatches against four candidate rules: luma below 16 raised to 16 or kept, each rounded down or to nearest.

The engine model predicted operation A: luma raised and rounded down, rectangle CRC-32 0x5551ee74 and exact-region CRC-32 0x0f05ef24. If A had followed another rule or none, the stage would have stopped before the variants and the scales. Variants B to E were measurements: each one's result is the candidate whose rectangle CRC-32 it equals, or none.

| Rule | Rectangle CRC-32 | Exact-region CRC-32 |
| --- | --- | --- |
| luma raised, rounded down | 0x5551ee74 | 0x0f05ef24 |
| luma raised, rounded to nearest | 0x9c066707 | 0x43eb5d93 |
| luma kept, rounded down | 0xb9c1df94 | 0xe1f6034c |
| luma kept, rounded to nearest | 0x32db2f17 | 0xf7ff37dc |

For each scale, the flat region had to equal the flat colour converted under the model (0xffdf884f), every row the middle row, the ramp could not decrease, and the middle row had to lie within 2 of the decoder's scaler model in each channel.

After the engine was closed, one A76 core at 2002 MHz converted the luma-sweep frame with the BT.709 full table under the model's rule three times in scalar C, as in the first stage.

Run `1794752244516494656-bd8feb22` (port 3-1, image 40ef88476734, built at 59c11057a) matched the prediction and ran every operation:

| Operation | Rule matched | Rectangle CRC-32 | Exact-region CRC-32 | Launch to interrupt |
| --- | --- | --- | --- | --- |
| A, vendor page | luma raised, rounded down | 0x5551ee74 | 0x0f05ef24 | 1050.8 µs |
| B, fill colours at luma 0 | luma raised, rounded down | 0x5551ee74 | 0x0f05ef24 | 1049.0 µs |
| C, pipe 0's fill colour off | luma raised, rounded down | 0x5551ee74 | 0x0f05ef24 | 1049.4 µs |
| D, csc0 at pipe 0's input, RGB blender | luma kept, rounded down | 0xb9c1df94 | 0xe1f6034c | 1049.2 µs |
| E, write-back `round_en` | luma raised, rounded down | 0x5551ee74 | 0x0f05ef24 | 1048.9 µs |

In A, raising luma to 16 and rounding down matched all 145,920 exact-region pixels, and keeping luma differed at 9,078. The fill colours and the write-back's `round_en` do not change the output. Converting at pipe 0's input and blending in RGB keeps luma 0 to 15, so the floor is applied when the blender works in YUV, before csc2. Both converters round down. The vendor's BT.709 full block, which the stage programmed, still maps luma 0 to 16 to black at neutral chroma (see the table comparison above). Every rectangle word had alpha 0xff, the padding and the source were unchanged, and the two sampled rows of each conversion matched the half-phase chroma model.

Both scales passed. The flat region equalled 0xffdf884f, every row equalled the middle row, and no channel of the ramp decreased. The 320 by 240 middle row equals the decoder's scaler model, and the 224 by 224 ramp differed from it by up to 2 at 5 pixels; the scaler rounds each position to the nearest phase, which the model then truncated ([third conversion stage](#third-conversion-stage)). The engine took 1039.3 µs for 320 by 240 and 1038.4 µs for 224 by 224, close to a conversion's time, so the engine's time follows the input's 307,200 pixels (296 Mpixel/s).

One A76 core at 2002 MHz converted the frame in 2670.5 µs, then 2646.5 and 2648.2 µs (116 Mpixel/s), and its output equalled A's. That reference is scalar integer C without NEON, a lower bound on the CPU's rate: the engine is 2.5 times faster than scalar C. The open and the close returned 1, the close gated the engine, and the session returned normally in 24.1 s.

### Third conversion stage

The third stage tests the conversion path that the builder now programs for every table: csc0 at pipe 0's input with the blender in RGB, and the tables derived from the BT.601 and BT.709 matrices with round-to-nearest constants ([Source format and the job](#source-format-and-the-job)). It also tests the corrected scaler model on both scales.

Over all 2^24 (Y, U, V) inputs, each derived table run through the converter's integer arithmetic is within 1 of the real matrix rounded to nearest, in every channel ([g2d-csc-tables.py](../checks/g2d-csc-tables.py)). Without the lower clamp or without the upper one, the error reaches at least 83, and the luma-sweep frame drives every table past both clamps, so the frame's CRC-32 tests both bounds.

The path changes the output from the second stage's:
- **Full range.** BT.709 full maps luma 0 to 16 and 240 to 255 to distinct values; BT.601 full maps grey Y to Y.
- **Limited range.** Luma below 16 now reaches the table unchanged. With neutral chroma the result is still black, since the table maps Y 16 and below to 0. With other chroma, R, G and B include 1.164 (Y - 16), which differs from the raised value: in the luma-sweep frame, 19,273 of its 19,274 pixels with luma below 16 change under BT.601 limited and 19,255 under BT.709 limited. This is the matrix's own result for footroom luma.
- **Rounding.** Every table rounds to nearest instead of down, so most outputs differ from the second stage's by up to 1.

The scaler model of the second stage truncated each output pixel's position (x times the step, 20 fraction bits) to its phase, and the 224 by 224 row of run `1794752244516494656-bd8feb22` differed from it at 5 pixels. Rounding the position to the nearest of 32 phases, by adding half a phase (2^14) before taking the top 5 fraction bits and carrying into the integer part, reproduces both scaled rows of that run exactly. The 2:1 scale has a step of 2, so every position falls on phase 0 under either rule.

[g2d-convert](../apps/g2d-convert/main.c) runs six operations into the "surface" region, each output filled with 0x5a bytes first, and predicts each one exactly:

| Operation | Frame | Table | Output | Rectangle CRC-32 | Exact-region CRC-32 |
| --- | --- | --- | --- | --- | --- |
| 1 | luma sweep | BT.709 full | 640 by 480 | 0x687fdce1 | 0xc78e6d00 |
| 2 | luma sweep | BT.601 full | 640 by 480 | 0xa7108f8a | 0x0757b166 |
| 3 | luma sweep | BT.709 limited | 640 by 480 | 0xd1891536 | 0xc53940bf |
| 4 | luma sweep | BT.601 limited | 640 by 480 | 0x3d553270 | 0xf54a807c |
| 5 | scale frame | BT.601 limited | 320 by 240 | middle row 0xfcf93c8e | flat 0xffdf894f |
| 6 | scale frame | BT.601 limited | 224 by 224 | middle row 0xd8e3bcdb | flat 0xffdf894f |

The application counts each conversion's exact-region mismatches against two rules, luma kept (the prediction) and luma raised to 16, and stops before the next operation when a conversion does not follow the first. For each scale the flat region must equal the flat colour, every row the middle row, the ramp must not decrease, and the middle row must equal the scaler model. After the engine is closed, one A76 core at 2002 MHz converts the luma-sweep frame with the BT.709 full table three times in scalar C, as in the earlier stages.

[The decoder](../apps/g2d-convert/host.py) names each conversion's rule and the model its rectangle equals: the derived table or the vendor's block, with luma kept or raised. It reproduces the second stage's records: the vendor's BT.709 full block with luma raised gives 0x5551ee74, the vendor page's CRC-32, and with luma kept 0xb9c1df94, the csc0 variant's. [g2d-convert-host.py](../checks/g2d-convert-host.py) builds the application on the host with the engine model and checks the decoder against it and seven mutant engines: luma raised to 16, no lower clamp, no upper clamp, uninterpolated chroma, a truncated scaler phase, a missed conversion and a failed scale.
