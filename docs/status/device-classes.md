# Camera, display and USB device classes

**Status: implemented.**

The sensor and display interfaces separate device protocols from board wiring. The shared camera interface describes formats, frame intervals and retained buffers for CSI and USB providers. Host checks pass for sensor lifecycle, mode negotiation, EDID and DisplayID parsing, panel sequences, shared camera lifecycle, UVC, HID, serial and Type-C behavior.

**Software evidence:** host.

**Board-qualified scope:** None.

No CSI sensor is attached, no MIPI panel is selected, and no USB peripheral is attached, so no host check establishes board timing, throughput, power or visible pixels.

## Capabilities

| Capability | Status | Key figure | Evidence and design |
| --- | --- | --- | --- |
| Camera sensor class and shared camera frames | host-tested | sensor lifecycle, mode negotiation, retained CSI frames | `device-classes-host-checks` in [device-classes.jsonl](../evidence/device-classes.jsonl) |
| Display connector and panel classes | host-tested | 20,000 mutated EDIDs under sanitizers | same record |
| USB HID, serial, UVC and Type-C | host-tested | FUSB302 on S_TWI1 is the only Type-C adapter | same record |
| CSI RAW input to the ISP | open | RAW10 OV5647 refused | open work item 1 |
| Physical timing, throughput, power and pixels | open | none attached | `device-classes-physical-measurements` (not run) |

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
2 records in `docs/evidence/device-classes.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `device-classes-host-checks` | 2026-11-01 |  | `` | not-run | passed | mutated_edids_sanitized 20000 edids, edid_blocks_parser_max 8 blocks, edid_blocks_hdmi_transport 4 blocks, dp_result_bytes 256 bytes, innolux_refused_mode 56.9 MHz, lg_refused_lines 1280 lines |
| `device-classes-physical-measurements` | 2026-11-02 |  | `` | not-run | inconclusive |  |
<!-- evidence-table:end -->
