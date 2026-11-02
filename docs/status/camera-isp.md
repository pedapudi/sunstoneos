# Camera, CSI and ISP

**Status: implemented.**

OV5640 control, both CSI connector bindings, TWI9 for CAM2, the local PHY and parser, ISP1/VIPP0 pass-through and the DMA and IOMMU paths have software evidence. Default capture is nominal 640x480 at 30 frames per second. Sensors sit behind a class interface (`include/camera_sensor.h`).

**Software evidence:** emulator, source.

**Board-qualified scope:** None.

## Open work

1. Replay IRQ delivery and compare physical pixels against the reference.
2. Qualify the module wiring, electrical operation, sensor writes, deskew, address latching, DMA visibility, frame capture and drain.
3. Complete ongoing native frame delivery.
4. Establish whether the interpolation and color-conversion algorithm is equivalent to ISP600.
5. Qualify repeated operation; both IOMMU masters, allocations and provider leases stay owned until fresh status and fault checks pass.

## Evidence

Board results are records in `docs/evidence/camera-isp.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-camera-isp-20261101.md).

<!-- evidence-table:begin -->
No board result is recorded in `docs/evidence/camera-isp.jsonl`.
<!-- evidence-table:end -->
