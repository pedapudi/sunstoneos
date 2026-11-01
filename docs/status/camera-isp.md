# Camera, CSI and ISP

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

Recorded ISP0 accepts one 640x480 BGGR12 input through TOP DDR, ISP600,
VIPP0 and DMA0 into copied NV16. Native defaults reuse GPL source without an
opaque tuning blob. `isp0-native-20261020/` records bounded EL1h calls with
modeled pixels; it does not qualify full task scheduling or numerical output.
See [source provenance](../../drivers/camera/isp0-sources.json).

Remaining work includes OV5640 module qualification on both connectors,
continuous native frame delivery, raw receiver/parser routing into ISP0,
independent pixel comparison, IRQ completion, scaling, auto-exposure/white-balance
control, sensor tuning and fault recovery. Hawkeye 64 MP is a separate feasibility
question: published ISP dimensions are below a full 64 MP frame; lower modes or
bypass need independent evidence.

See [camera](../camera.md), [capture](../camera-capture.md),
[native path](../camera-native.md), [application](../camera-application.md),
[platform](../camera-platform.md), [CSI](../csi-receiver.md) and [ISP0](../camera-isp0.md).
