# Camera DMA retains buffers until capture and memory writes finish

The native camera DMA controller configures DMA140 channel 0 for one 640×480
NV16 image. NV16 stores luminance in one plane and interleaved U/V chroma in
another. Each plane has 480 rows of 640 bytes, for 614,400 image bytes in total.
The sensor and receiver carry UYVY packets; the DMA memory layout is NV16.

The implementation is source-checked and host-tested. It has not captured a
physical image. Camera routing, scaler configuration, memory admission and a
single-frame input fence remain platform integration requirements. The
[existing receiver](csi-receiver.md) enables continuous capture and cannot satisfy
the single-frame contract without an explicit integration change.

## One admitted image owns both memory spans

[The interface](../include/a733_camera_dma.h) accepts a physical DMA block from
0 through 5, two bus-address spans and a nonzero frame identity. A block occupies
4 KiB at `0x05830000 + block * 0x1000`. The controller reserves the entire block
and programs channel 0. It refuses active channels or enabled interrupts.

Each image plane needs 307,200 bytes. The caller supplies the complete mapping
span, including any guard space required by bus access or prefetch behavior.
Both spans must be disjoint, start on 64-byte boundaries and fit within the
34-bit register encoding. The 64-byte alignment is a conservative API policy;
it does not establish cache coherency or the actual memory-bus address width.

The register source shifts addresses right by two into a 32-bit field. The
Linux video allocation path separately requests a **32-bit DMA mask**. Therefore,
a high address passing the encoding check does not establish that hardware can
access it. Platform admission must prove the specific CSI master-1 mapping,
including every byte of both spans and any guard requirements.

Opening reserves the frame immediately. Admission must publish its ownership
token before side effects, including partial cache or mapping changes. The
caller must not read, write, remap or free either span while ownership is
reserved or assigned to the device. Admission also establishes:

- Exclusive DMA registers, stable clocks and reset state, and independent recovery.
- A configured route through the camera crossbar and video scaler.
- Progressive YUV422 input at 640×480, without scaling, cropping or overlays.
- Cache preparation for device writes, with no conflicting cache-line aliases.
- A quiet producer and a fence that permits one complete frame and prevents any
  part of a second frame from reaching the DMA.

The controller programs automatic buffer length, online channel 0, NV16 format,
640-byte strides, image dimensions, plane addresses and disabled compression.
It leaves interrupts disabled and polls completion. Every configuration write
has masked readback verification. Stale completion flags are cleared and checked
while the producer remains quiet; capture enable is the last configuration write.

## Capture completion and bus drain have separate requirements

After the controller reaches `ARMED`, `capture` invokes the platform's
`start_one` operation. The required parser mode is single-image capture (`SCAP`).
The controller waits for DMA `CAPTURE_DONE`, status bit 0. Video `FRAME_DONE`,
bit 1, does not complete an image. The pinned image-mode interrupt handler uses
this same distinction.

Observed overflow, frame loss, address, pixel or line errors cause a terminal
fault. Unrecognized status bits also cause a fault. The controller retains the
observed status for diagnosis and does not acknowledge faults automatically.

On capture completion, the controller establishes a quiet producer, disables
DMA capture and the block, and verifies the enable bits. A separate platform
operation must then prove that every memory write has retired, no address remains
prefetched, the input path has no errors, and one undamaged image was captured.
Enable-bit readback and a completion flag cannot replace that proof.

After drain, the controller checks completion and errors again, checks that
capture is inactive, and acknowledges only the observed status. Cache
invalidation and acquire barriers follow. Only a successful transition to
`READY` returns frame ownership to the caller. Closing then releases the mapping
and resource leases; it does not erase the caller's image.

Each poll performs at most one MMIO or platform operation, with monotonic time
checks before and after it. A pending callback continues the same operation;
it must not start another frame or repeat a completed resource mutation. A deadline, clock regression, uncertain write,
readback mismatch or callback error terminates the transaction. Faults retain
ownership and stop further hardware callbacks. Recovery requires the external
coordinator; there is no automatic rollback, retry or reopen.

A successful release cannot be undone if its final time check fails. In that
case the controller records a fault with no resource token, and the caller
retains the image it already owned. There is no cancellation path from `ARMED`;
interruption before capture requires coordinator recovery.

## The A733 YUV route still needs a complete native implementation

The pinned A733 build selects combo CSI, ISP600, VIPP200 and DMA140. VIPP is the
video image postprocessor that performs scaling and output-format conversion.
The active pipeline selects a parser input for an ISP selector, then selects that
ISP output for a VIPP block. The direct parser-to-DMA function belongs to another
SoC branch and does not establish an A733 route.

The source exposes logical ISP devices 4, 5 and 6 without register resources.
Their selector numbers are 1, 2 and 3. These are candidate bypass paths, but the
board's enabled camera paths select the physical ISP. A complete configured
OV5640 YUV path through those empty ISP devices has not been established.
The DMA controller therefore does not program speculative crossbar values.

For a candidate selector `s`, parser `p`, channel `c` and VIPP block `v`, the
source's crossbar functions write `p * 4 + c` at camera TOP offset
`0x30 + s * 0x10 + input * 4`. They write `s * 4 + output` at offset
`0xa0 + v * 4`. Camera TOP is `0x05800800`. The valid input/output selections
must be resolved for the actual bypass path before these formulas become a
native routing sequence.

VIPP200 loads parameter blocks from memory and needs its own memory admission,
clocking and completion handling. The normal YUV path enables scaling and
copies parameter data into a DMA-visible load buffer. A native identity-scale
configuration must account for that load, even when image dimensions stay
unchanged. No assumption that disabling ISP also disables VIPP is safe here.

The camera domain, bus reset, CSI and ISP clocks, camera memory-bus clocks,
local camera clock gates and IOMMU state also remain required. The shared PHY
calibration source defect described in the receiver document remains unresolved.
No independent register description found during this work establishes a safe
correction.

## Source anchors and focused validation

All source anchors below refer to Linux camera revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` in
[the board kernel repository](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/vin).
Paths are relative to `bsp/drivers/vin` unless stated otherwise.

| Source | Evidence used |
| --- | --- |
| `Makefile`; `platform/platform_cfg.h`; `platform/sun60iw2_vin_cfg.h` | A733 component selection, physical DMA blocks and CSI IOMMU master 1 |
| `vin-video/dma140/dma140_reg.c:34–55, 66–166, 250–427, 520–607, 717–724` | Block/channel mapping, configuration, address shift, status and acknowledgment |
| `vin-video/dma140/dma140_reg_i.h:29–249` | Register offsets and masks |
| `vin-video/dma140/dma140_reg.h:77–143, 195–211` | Output-format codes, field selection and frame-counter terminology |
| `vin-video/vin_video.c:5737, 5816–5851, 5959–5964, 6050–6170` | 32-bit allocation mask, online setup, NV16 strides and enable/disable sequence |
| `vin-video/vin_core.c:1534–1552`; `vin-csi/sunxi_csi.c:436–439` | Image completion uses CAPTURE_DONE; parser selects SCAP |
| `vin.c:1774–1807, 2033–2044`; `top_reg.c:26–175, 362–374`; `top_reg_i.h:106–133` | Active A733 crossbar path and YUV sensor classification |
| `vin-isp/isp600/isp600_reg_cfg.c`; `vin-isp/sunxi_isp.c:3387–3392, 3519–3525` | Virtual ISP selectors and devices without registers |
| `vin-vipp/sunxi_scaler.c:638–678, 825–832, 899–906`; `vin-vipp/vipp200/vipp200_reg.c:47–62` | VIPP enable, YUV422 output, parameter loading and physical selection |

The changed risks are incorrect DMA addresses or formats, premature CPU access,
and release after incomplete capture. The focused check compares all 4 KiB of
modeled DMA registers against unchanged pinned register functions for channel 0
of every physical block. It also exercises full-span validation, pending
callbacks, dropped writes, completion/error distinctions and retained ownership.

The native fixture passed AddressSanitizer and UndefinedBehaviorSanitizer. The
source reference retains signed mask expressions and runs without sanitizers.
An AArch64 freestanding compile passed. These results establish configuration
agreement and modeled ownership behavior; they do not qualify capture hardware.
Raw commands, input identities and artifacts are outside Git under
`~/.cache/sunstoneos/evidence/camera-dma-20261016/`.

The completion fixture and AArch64 object have evidence for the image-mode
completion condition. The unchanged source reference executable and register
image remain reusable. Sensor and receiver evidence remains separate.
