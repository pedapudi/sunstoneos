# A733 YUV capture uses a documented ISP pass-through channel

The native route controller connects parser 0 or 1, channel 0, through ISP1
channel 0 to VIPP0 and DMA0. ISP1 is a pass-through selector; ISP0 contains the
image processor. The A733 user manual states this distinction explicitly in
Table 16-1 and documents the selector encodings in sections 16.1.9.12 and
16.1.9.29. The Linux MIPI CSI guide also provides configurations through logical
ISP device 4, which selects hardware ISP1.

[The route driver](../drivers/camera/a733_camera_route.c) programs camera TOP
register `0x40` with `parser * 4` and register `0xa0` with `4`. Camera TOP begins
at `0x05800800`. Only the documented four-bit selector fields change. This route
does not enable processing in the physical ISP or require its tuning tables.

The route and [single-image receiver mode](../include/a733_csi_receiver.h) are
implemented and source-checked. Neither has run on the board. A native platform
backend still needs shared clock/power ownership, pin configuration, calibrated
PHY admission, DMA mappings, cache maintenance and independent recovery.

## VIPP receives one complete parameter image

VIPP, the video image postprocessor, sits before DMA0. The controller reserves
all of physical VIPP0 at `0x05910000`, including its four channels, and uses
channel 0 in online mode. Input and output are progressive 640×480 YUV422.
Scaling preserves both dimensions. Chroma downsampling, color conversion and
overlays remain disabled.

The controller constructs a complete 1,024-byte little-endian parameter image.
The image contains these nonzero words; every other byte is zero:

| Byte offset | Value | Meaning |
| --- | --- | --- |
| `0x200` | `0x00000001` | Scaler enabled |
| `0x204` | `0x08400100` | Horizontal/vertical ratios 256; weight shift 1 |
| `0x208` | `0x01e00280` | Output width 640, height 480 |
| `0x220` | `0x000000d0` | YUV422 input, scaler output and final output |
| `0x234` | `0x01e00280` | Crop width 640, height 480; origin is zero |

The source calculates ratios with eight fractional bits, so 256 represents
unity. Its weight-normalization function returns 1 for those ratios. The
reference check executes that function and the original parameter setters.

The newer generic Orange Pi scaler code assigns a precision field from an
uninitialized configuration member. The separately pinned Radxa A733 driver has
no such field and leaves offset `0x20c` zero. The native image follows that
A733-specific source layout. It does not reproduce the uninitialized assignment.

Platform admission binds a contiguous parameter mapping, its complete span and
any required prefetch guards. The API requires a 64-byte-aligned start and a span
of at least 1,024 bytes wholly below 4 GiB. These checks do not establish a valid
IOMMU mapping, cache isolation or guard size; admission must prove those
properties before programming registers.

A publication callback copies the complete image to that mapping and cleans the
cache before any load address is programmed. Parameter memory remains immutable
and owned until verified drain and release. The controller retains the source
image in its own storage and performs no dynamic allocation.

## The receiver captures one frame without automatic rearming

`a733_csi_open_image` uses parser `SCAP`, while the existing `a733_csi_open` API
retains continuous `VCAP`. A733 manual section 16.1.13.4 defines SCAP as capture
of the next complete frame followed by automatic clearing of the enable bit.
The producer must remain quiet throughout receiver configuration and readback.

The receiver does not rearm SCAP when its status is sampled. A new capture
requires a separately designed lifecycle; reopening an existing controller is
refused. The camera DMA controller waits for `CAPTURE_DONE`, which the manual
identifies as completion of writing the still image to its buffer. These
controls prevent a subsequent partial frame from overwriting the same buffer.

The platform can compose the components in this order:

1. Reserve the camera path, DMA0, parameter memory, image buffers and shared
   resources, with the sensor quiet.
2. Prepare the route and publish VIPP parameters, then prepare camera DMA0.
3. Open the receiver in image mode, with SCAP armed and the sensor still quiet.
4. Start the sensor through the DMA transaction's `start_one` callback.
5. On DMA capture completion, stop the sensor and verify the single-frame fence.
6. Disable DMA capture. During its drain operation, finish the VIPP route and
   prove all parameter reads, prefetches and image writes have retired.
7. Let DMA perform its final error checks and cache synchronization before
   returning image-buffer ownership. Release remaining receiver/sensor leases
   after their own quiescence requirements are satisfied.

Each component retains its own lease until its consumers have finished. A
parameter-memory release must not disable clocks still needed by DMA register
access. Platform callbacks must avoid circular waits between component close
operations. This ordering is an integration contract; the coordinating native
backend has not yet been implemented.

## Parameter loading does not substitute for capture completion

Before configuration, the route rejects active VIPP channels, enabled interrupts
and an active camera pattern generator. It clears stale VIPP status while input
is quiet. The load address, parameter-ready flag and channel capture enable are
programmed in that order. Parameter-ready may clear after loading, so its
readback is not treated as a persistent enable bit.

`ARMED` means configuration has been submitted. It does not mean that parameters
have loaded or that a frame has arrived. Finishing requires a callback to prove
quiet input, completed SCAP and DMA capture, and absence of upstream errors.
The controller then disables VIPP capture, polling the enable bit until the
current frame has finished, and disables the local VIPP clock.

An independent drain callback proves that parameter reads and downstream writes
have retired. After drain, the controller reads raw VIPP status with interrupts
masked. It requires the channel-0 parameter-load flag and rejects every other
status bit before releasing resources. The source's status helper masks raw
status by interrupt enables; the native controller reads raw status directly.

Every poll performs at most one platform/MMIO operation plus monotonic time
checks. Pending operations continue the same transaction. Deadline expiry,
clock regression, uncertain access, missing load completion or any error causes
a terminal fault that retains ownership. There is no automatic reset, retry or
cancellation. Successful release remains irreversible if the final time check
subsequently fails.

## The manual resolves the PHY calibration result-field defect

A733 manual section 16.1.10.2 marks terminal-resistor calibration RESULT bits
20:16 as read-only. In software mode the result reflects SET bits 8:4, and the
completion FLAG reads one. Therefore, changing the source's erroneous mask to
write RESULT would still be incorrect.

A native calibration sequence must configure SET and software mode, then verify
RESULT and FLAG under a deadline. The source's automatic-calibration reset bit
applies only to automatic mode. Both inspected BSP versions contain the same
erroneous RESULT write. This route component does not initialize shared analog
power or calibration; those operations require ownership of the shared PHY.

The manual and BSP also use different names for the reference-voltage fields
at bits 11:10 and 9:8. Hardware admission must resolve the intended values before
claiming analog qualification. The existing receiver's register readback does
not establish that qualification.

## Pinned sources and focused evidence

The [A733 documentation project](https://gitlab.com/tina5.0_aiot/product/docs/-/tree/d0dc4d615be00b76e3a0818d0818eff8f69ffa15)
is pinned to revision `d0dc4d615be00b76e3a0818d0818eff8f69ffa15`.
Its A733 user manual is `A733/Hardware硬件类文档/芯片手册/A733_User Manual_V1.00.pdf`,
with SHA256 `cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d`.
The relevant printed pages are 793–794, 832, 847, 886–887, 938 and 984–985.
The Linux MIPI CSI guide supplies the YUV device selection and complete ISP4
example in section 3.2.4 and printed pages 61–62.

The [Orange Pi camera source](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/vin)
provides `top_reg.c`, parser configuration and the unity-ratio calculation.
The [Radxa A733 camera source](https://github.com/radxa/allwinner-bsp/tree/2045a3ca2a01f088c0314dc924bda59d154e363e/drivers/vin/vin-vipp/vipp200)
provides the independently checked parameter layout and VIPP register functions.
The source-reference check pins each file by SHA256 and preserves function
bodies unchanged in its generated harnesses outside Git.

The credible changed risks are incorrect routing or parameter bytes, premature
parameter release, and accidental continuous capture. The focused check compares
complete TOP/VIPP register and parameter images for both parsers. It compares
complete receiver images for SCAP and VCAP on both camera ports and verifies
that sampling a completed image does not rearm capture.

The ownership fixture passed AddressSanitizer and UndefinedBehaviorSanitizer.
It covers publication failure, delayed disable, missing parameter load, raw
status errors, deadline/clock faults and retained leases. Both affected native
AArch64 objects compile. Original sensor and DMA tests were not rerun.
These checks establish source agreement and modeled behavior; physical capture
and integrated platform admission remain unqualified.

Commands, exact input identities, source proposals and compiled artifacts are
under `~/.cache/sunstoneos/evidence/camera-route-20261016/`. The shared manual and
extracted text are under `~/.cache/sunstoneos/sources/camera-research/`.
