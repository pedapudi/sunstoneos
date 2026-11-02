# Camera, display and USB device classes

**Status: implemented.**

The sensor and display interfaces separate device protocols from board wiring. The shared camera interface describes formats, frame intervals and retained buffers for CSI and USB providers. Host checks pass for sensor lifecycle, mode negotiation, EDID and DisplayID parsing, panel sequences, shared camera lifecycle, UVC, HID, serial and Type-C behavior.

**Software evidence:** host.

**Board-qualified scope:** None.

No CSI sensor is attached, no MIPI panel is selected, and no USB peripheral is attached, so no host check establishes board timing, throughput, power or visible pixels.

## Open work

1. Connect sensor-negotiated RAW media-bus formats and dimensions to the ISP0 input configuration, add RAW conversion and tuning, and keep the completion and retained-buffer contract. The native CSI workload requires YUV422 input and NV16 output, so the RAW10 OV5647 is refused.
2. Add exposure, gain and white-balance control to the sensor adapters, which report them unsupported.
3. Complete CSI EL0 publication and independent drain.
4. Admit more display modes: the PLL planner refuses the Innolux 56.9 MHz mode, the LCD planner refuses the 1280-line LG panel, parallel LCD has no native board route, and DisplayPort admits one EDID extension in its 256-byte result.
5. Add a Type-C controller adapter for any controller other than the FUSB302 on S_TWI1.
6. Fix the rejected hunk in `apps/storage-read-single-core/platform.patch` that blocks the standalone DSI application configure.
7. Measure latency percentiles, throughput, CPU cost, idle power and visible pixels once the sensor, panel and peripherals are attached.

## Evidence

Board results with a queue entry are records in `docs/evidence/device-classes.jsonl`, added with `tools/evidence.py add`. Results cited by path, and earlier prose evidence, are in [the historical log](../history/status-device-classes-20261101.md).

<!-- evidence-table:begin -->
No board result is recorded in `docs/evidence/device-classes.jsonl`.
<!-- evidence-table:end -->
