# Camera DMA retains buffers until capture and memory writes finish

The native camera DMA controller configures DMA140 channel 0 for NV16 images of
one configured size, either one image at a time or continuously into a ring of
frame slots. NV16 stores luminance in one plane and interleaved U/V chroma in
another. Each plane has one row of width bytes per image line: at 640 × 480,
480 rows of 640 bytes, for 614,400 image bytes in total. The sensor and
receiver carry UYVY or YUYV packets; the DMA memory layout is NV16.
`a733_camera_dma_init` selects 640 × 480, and `a733_camera_dma_init_format`
accepts any width that is a multiple of 16 up to 2,592 and any even height up
to 1,944.

The implementation is source-checked and host-tested. It has not captured a
physical image. The [native camera route](camera-route.md) now configures a
YUV pass-through path and identity scaling. The [receiver](csi-receiver.md)
supports single-image parser capture. The [native capture path](camera-native.md)
composes these controllers with platform providers and memory admission.
Global bus drain and physical capture remain required.

## One admitted image owns both memory spans

[The interface](../include/a733_camera_dma.h) accepts a physical DMA block from
0 through 5, two bus-address spans and a nonzero frame identity. A block occupies
4 KiB at `0x05830000 + block * 0x1000`. The controller reserves the entire block
and programs channel 0. It refuses active channels or enabled interrupts.

Each image plane needs width × height bytes, 307,200 at 640 × 480. The caller
supplies the complete mapping
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
caller must not write, remap or free either span while ownership is reserved or
assigned to the device. Reads are prohibited until either drained ownership
transfer or the explicit retained-readout completion below. Admission also establishes:

- Exclusive DMA registers, stable clocks and reset state, and independent recovery.
- A configured route through the camera crossbar and video scaler.
- Progressive YUV422 input at the configured size, without scaling, cropping
  or overlays.
- Cache preparation for device writes, with no conflicting cache-line aliases.
- A quiet producer and a fence that permits one complete frame and prevents any
  part of a second frame from reaching the DMA.

The controller programs automatic buffer length, online channel 0, NV16 format,
width-byte strides, image dimensions, a flip size of two bytes per pixel, plane
addresses and disabled compression.
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

The separate retained-readout API permits read-only access to a completed still
frame while preserving device ownership. It requires the original capture-done
observation, self-cleared parser SCAP controls, CSI and route error checks, and
an IOMMU fault scan. Fresh DMA completion and inactive-capture observations
precede CPU cache invalidation. The coordinator rechecks time and authority
before publishing its result.

This path reaches `READABLE`, keeps both planes device-owned, and never calls
disable, drain, close or release. Only the width × height image payload of each
plane may be read; guards and padding are excluded. Every table, parameter,
frame, clock and hardware reservation remains retained. CPU writes and global
reclamation remain prohibited. The manual's still-frame completion definition
and the reference driver's image return support this limited access; they do
not establish global transaction drain.

`a733_camera_dma_rearm` returns a `READABLE` channel to `ARMED` for one more
image into the same planes. Its caller declares that every view and copy has
ended and keeps the producer stopped behind a cleared parser fence. Status must
still equal the final observation, and only those bits are acknowledged. Status
must then read clear, capture must be inactive and the channel must still be
enabled. The admission callback runs again with the retained token to prepare
the cache for device writes. Ownership stays with the device, and a fault
retains everything.

A returned privileged const view cannot be revoked. Its caller must serialize
use with the owner and stop on a later fault. Only a completed copy in separately
admitted application memory remains independently usable. The view does not
grant an EL0 mapping or change the memory pool's device-ownership record.

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

## Continuous capture rotates frame slots at each frame start

`a733_camera_dma_stream` starts continuous capture from `ARMED` with the
parser in video capture mode. It needs the optional `start_stream` and
`sync_slot` operations, 2–4 slots and a slot size that is a multiple of 4096,
covers one plane and fits both spans that many times. Slot k's planes start
k × slot size into the Y and UV spans. Configuration has already programmed
slot 0. Every slot stays device-owned until it is delivered.

In `STREAMING` each poll performs one operation: a pending slot invalidation,
a status read, the acknowledgement of the observed flags only, one of the
two address writes, or a status read after them. The ring counts frame-done
flags still owed: each observed frame start (bit 7) adds one, and each status
read that shows frame done (bit 1) removes one. A frame start activates the
programmed slot, delivers the previous slot only if no flag is then owed, and
programs the next free slot. A late or merged frame-done flag therefore drops
frames and never delivers a slot the device may still write; the reference driver's frame-start handler relies on the same latching
of the address registers. When no slot is free the active slot repeats and its
frame is lost. When the status read after the address writes shows another
frame start, the write may have missed it: the controller acknowledges only
that flag, delivers nothing, and does not deliver the active slot at the next
start either. The [capture coordinator](camera-capture.md#continuous-capture-delivers-complete-ring-slots)
describes the delivery rules and the frame counts.

The deadline given to `a733_camera_dma_stream` bounds `STREAM_START` and then
`STREAMING`. `a733_camera_dma_renew`, valid only in `STREAMING`, replaces it
with a deadline later than the last clock reading and performs no I/O. Without
renewal the stream faults at the deadline and keeps every resource.

`a733_camera_dma_take` hands the oldest delivered slot to the caller with its
frame-start count; `a733_camera_dma_give` returns it to the device. Neither
performs MMIO. `a733_camera_dma_stop_stream` refuses while a slot is held and
otherwise enters `STOP`; the producer must be stopped and fenced before the next
poll. The disable, drain and final-status sequence of a still image follows,
except that the final status needs no capture-done flag. `READY` returns both
complete spans to the caller.

## Native routing connects the parser to the frame DMA

The [route controller](camera-route.md) selects parser zero or one through
pass-through ISP1 and VIPP0 to DMA0. The A733 manual identifies ISP1–3 as
pass-through paths; ISP0 performs image processing. VIPP is the video image
postprocessor that performs scaling and output-format conversion.

The controller publishes a complete parameter block for unity YUV422 scaling,
reserves both complete shared selector registers, and retains ownership through
capture and independent drain. The receiver's single-image mode admits one frame;
DMA completion and memory visibility still require their separate observations.
Full ISP processing and other capture modes remain required.

The camera domain, bus reset, CSI/ISP clocks, memory-bus clocks, local gates,
IOMMU mappings and global analog startup remain platform integration work.
The manual defines the termination-calibration result as read-only; global
startup must program the software setting and verify its reflected result.

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

The reference check also compares the complete register image of block 0 at
320 × 240, 1280 × 720, 2592 × 1944 and 176 × 144 with the pinned functions,
using the NV16 lengths of the reference driver's video path. The fixture
refuses plane spans one byte short and widths that are not multiples of 16.
Ring admission refuses missing stream operations, one or five slots, slots
smaller than a plane, not page multiples or larger than the spans, and a
second start. Ring operation is exercised through the
[capture coordinator's model](camera-capture.md#composition-evidence-reuses-the-leaf-checks).

The completion fixture and AArch64 object have evidence for the image-mode
completion condition. The unchanged source reference executable and register
image remain reusable. Sensor and receiver evidence remains separate.
