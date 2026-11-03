# Camera, CSI and ISP

**Status: implemented.**

OV5640 control, both CSI connector bindings, TWI9 for CAM2, the local PHY and parser, ISP1/VIPP0 pass-through and the DMA and IOMMU paths have software evidence. Default capture is nominal 640x480 at 30 frames per second. Sensors sit behind a class interface (`include/camera_sensor.h`).

**Software evidence:** emulator, source.

**Board-qualified scope:** None.

## Capabilities

| Capability | Status | Key figure | Evidence or design |
| --- | --- | --- | --- |
| OV5640 sensor control, CAM1 and CAM2 selected by the MCLK mask | host-tested | 8,126 modeled register accesses on either connector | [design](../designs/camera-cam2-native.md), `camera-cam2-native-software` |
| Ten image sizes, 160 by 120 through 2592 by 1944, with a guarded 32 MiB pool for large modes | host-tested | largest stream source allocation 20,230,144 B | [design](../designs/camera-native-modes.md), `camera-native-modes-software` |
| ISP1/VIPP0 pass-through, DMA and IOMMU paths | host-tested | none | [camera-native.md](../camera-native.md) |
| Physical frames, receiver timing, deskew, drain | open | none | open work 1 to 3 |

## Open work

1. Replay IRQ delivery and compare physical pixels against the reference.
2. Qualify the module wiring, electrical operation, sensor writes, deskew, address latching, DMA visibility, frame capture and drain.
3. Complete ongoing native frame delivery.
4. Establish whether the interpolation and color-conversion algorithm is equivalent to ISP600.
5. Qualify repeated operation; both IOMMU masters, allocations and provider leases stay owned until fresh status and fault checks pass.

## Evidence

Designs: [camera connector selection](../designs/camera-cam2-native.md) (`docs/evidence/camera-cam2-native.jsonl`) and [image sizes and bounded memory](../designs/camera-native-modes.md) (`docs/evidence/camera-native-modes.jsonl`).

Board results are records in `docs/evidence/camera-isp.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-camera-isp-20261101.md).

<!-- evidence-table:begin -->
No board result is recorded in `docs/evidence/camera-isp.jsonl`.
<!-- evidence-table:end -->
