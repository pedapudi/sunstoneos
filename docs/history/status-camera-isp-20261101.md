# Camera, CSI and ISP: historical status record 2026-11-01

This log holds the prose evidence of [the current status](../status/camera-isp.md) as it stood on the date above. It is not updated. Board results that have queue entries are in the records `docs/evidence/camera-isp.jsonl`.

**Status: implemented.**

OV5640 control, both CSI connector bindings, TWI9 for
CAM2, local PHY/parser, ISP1/VIPP0 pass-through and DMA/IOMMU paths have software
evidence. Default capture is nominal 640x480 at 30 fps.

**Board-qualified scope:** None.

No MIPI camera is attached. Module wiring,
electrical operation, sensor writes, deskew, address latching, DMA visibility,
frame capture and drain remain unqualified.

Applications support ten reference UYVY sizes, with up to 10,077,696 output
bytes in a camera-only 32 MiB reservation. The coordinator implements two-to-four
continuous slots and drop accounting for UYVY/YUYV. The native finite-stream
application copies one completed frame and retains hardware. Complete ongoing
native delivery remains incomplete.

Evidence under `~/.cache/sunstoneos/evidence/` includes
`resume-camera-native-1025/final-snapshot/`, `camera-cam2-1026/final-snapshot/`,
`camera-modes-1026/final-snapshot/` and `camera-binding-20261018/`.
The mutex-ownership correction has separate modeled evidence in
`mutex-enter-20261020/root-integration.json`; the later camera snapshots include
the corrected adapter.

Recorded ISP0 replays one immutable 640x480 BGGR12 input through TOP DDR,
ISP600, VIPP0 and DMA0. Each copied NV16 frame stays immutable through USB
completion and sequence-bound acknowledgment. Fresh status and fault checks
precede retained reuse; both IOMMU masters, allocations and provider leases
remain owned. Native defaults reuse GPL source without an opaque tuning blob.
See [source provenance](../../drivers/camera/isp0-sources.json).

IRQ completion uses DMA0, VIPP0 load and ISP0 SAVE within the original session
deadline. Completion still requires ISP LOAD. Sanitizer, linked-image and
FreeRTOS QEMU checks cover a second capture after USB acknowledgment and
staggered IRQ events. Evidence and frozen host-only images are under
`~/.cache/sunstoneos/build/camera-isp-1031/`. Device effects, pixels, USB and IRQ
entry are modeled.

An identified host FFmpeg build supplies independent CPU comparison files.
Its interpolation and color-conversion algorithm is not established as
equivalent to ISP600. Physical pixels, IRQ delivery and repeated operation
remain unqualified. The retained ISP ELF fails the probe runner's watchdog
admission. A source-backed global drain condition and shared-probe conversion
must cover TOP reads, ISP/VIPP parameters, output writes and IOMMU page walks
before a board run. No run is queued.

Remaining work includes OV5640 module qualification on both connectors,
continuous native frame delivery, raw receiver/parser routing into ISP0,
physical pixel comparison, IRQ qualification, scaling, auto-exposure/white-balance
control, sensor tuning and fault recovery. Hawkeye 64 MP is a separate feasibility
question: published ISP dimensions are below a full 64 MP frame; lower modes or
bypass need independent evidence.

See [camera](../camera.md), [capture](../camera-capture.md),
[native path](../camera-native.md), [application](../camera-application.md),
[platform](../camera-platform.md), [CSI](../csi-receiver.md) and [ISP0](../camera-isp0.md).
