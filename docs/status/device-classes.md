# Camera, display and USB device classes

**Status: implemented and host-checked; physical qualification is open.**

The sensor and display interfaces separate device protocols from board wiring.
The shared camera interface describes formats, frame intervals and retained
buffers for CSI and USB providers. The existing USB application ABI remains
separate from privileged CSI access.

## Interfaces and configuration

| Interface | Class operations | Per-part data | Board description |
| --- | --- | --- | --- |
| Camera sensor | Acquire and power sequencing, chip-ID probe, mode enumeration and selection, stream start/stop, control query/set and asynchronous completion | ID registers/values, address, mode dimensions, media-bus format, frame timing, link frequency, register sequences and minimum holds | CSI port/parser, lanes/channel, I2C controller/address, clock, pin functions, reset/power-down polarity and supply references |
| Camera frames | Enumerate formats/intervals, start, next, release and stop through `sunstone_camera_ops` | NV16 planes for CSI; YUYV or MJPEG spans for UVC | The admitted provider retains the selected hardware and memory |
| Display connector | Detect with attachment generation, read modes, validate and select generic pixel timing | Monitor EDID/DisplayID; fixed-panel modes | Connector kind/controller, lanes/map, shared-resource identity and selected panel |
| Display panel | Validate a description and execute bounded preparation, enable and shutdown steps | Timings, lane or parallel-bus width, pixel format, commands, delays and named power/GPIO/backlight resources | Physical resource bindings and the authenticated session's module selection |
| USB device classes | Descriptor discovery, class negotiation, asynchronous completion, disconnect and explicit release | HID usage descriptors; UVC formats/intervals; CDC or bridge protocol tables | TYPEC24 controller, Type-C I2C bus/address, VBUS/interrupt pins and supply references |

[`camera_sensor.h`](../../include/camera_sensor.h) defines sensor modes and bus
operations. Frame interval derives from the mode's pixel clock and horizontal
and vertical totals. The generic manager probes only board-listed drivers at
the admitted address. An ID mismatch permits the next candidate; a transport
failure retains a fault without retrying the bus.

OV5640 is one adapter around its existing driver and tables. The capture core
uses a sensor-neutral callback immediately before packet emission to arm its
DMA status interval. It retains the receiver, route, DMA and IOMMU ownership
checks. Driver operations are selected during setup; frame acquisition adds no
allocation, task hop, tick wait or pixel copy.

[`sunstone_display.h`](../../include/sunstone_display.h) defines pixel clocks,
active size, porches, sync widths, polarities and rational refresh calculation.
DE and TCON planners consume that timing. HDMI retains an enum as a compatibility
key for its frozen session and exact analog profiles. Its constructor also
accepts generic timing and derives that key after validation.

The connector parser checks complete EDID extension chains, block checksums and
DisplayID section checksums. Catalogues carry the connector identity and
attachment generation. A sampled HDMI disconnect invalidates its cached EDID;
a subsequent sink needs a newly admitted owner. Discovery does not authorize
another clock or voltage point.

[`orangepi_zero3w.c`](../../drivers/board/orangepi_zero3w.c) holds the shared board
description. It records CAM1/CAM2 wiring, mini-HDMI, TYPEC24 and the unpopulated
DSI1 connector. The camera entries select the reference two-lane OV5640 module
profile on connectors wired for four lanes. They do not establish attachment.
Unspecified module supplies use `UINT16_MAX`; the authenticated camera session
provides actual supply and optional reset bindings. The DSI application overlays
its authenticated panel description on the board's connector route.

USB HID, serial and UVC already discover peripherals rather than selecting one
product. Their native controller and owner-task model is retained. The UVC
catalogue preserves every discrete interval or continuous interval range from
each frame descriptor. The shared camera backend negotiates advertised values
and exposes the original retained payload spans. Its catalogue remains immutable
until all frame leases are released. The OS USB publication boundary uses the
shared mode type; legacy `ss_usb_camera_*` calls still publish to applications.

## Findings and remaining restrictions

Line references identify the current implementation or restriction.

| Path | Finding and current behavior |
| --- | --- |
| `include/a733_camera_capture.h:4` | Capture depends on the generic sensor class; its stored mode, operations and sensor state contain no OV5640 type. |
| `drivers/camera/a733_camera_capture.c:38` | The pre-transmit callback replaces knowledge of a sensor register while preserving the DMA timing boundary. |
| `drivers/camera/a733_camera_native.c:401` | Native setup selects board buses and registered drivers. Sensor addresses, CSI/MCLK/I2C pins and GPIO polarity come from that description. |
| `include/a733_camera_profile.h:45` | Image allocation uses requested width/height; mode selection searches each driver's inventory by geometry. Legacy size presets remain build conveniences. |
| `include/a733_camera_common.h:39` | Geometry limits follow the VIPP field definition rather than a sensor's maximum image size. |
| `drivers/camera/sensor_registry.c:5` | Built-in driver registration is data. Adding a driver does not change the generic manager. |
| `drivers/camera/a733_camera_application.c:28` | CSI implements shared camera operations using held NV16 planes. EL0 publication and independent drain remain open. |
| `drivers/display/a733_hdmi.c:35` | HDMI and TV setup accept generic timing; the existing 720p60/1080p60 analog profiles remain the admitted set. |
| `drivers/display/a733_dp.c:69` | DP validates generic timing against its existing 720p60 profile. Its 256-byte copied-result ABI admits at most one EDID extension. |
| `include/sunstone_display.h:41` | The common parser handles up to eight EDID blocks; HDMI transport holds four blocks. Other timing encodings and exhausted mode capacity are reported. |
| `apps/dsi-scanout/native.c:20` | The DSI application binds its session-selected panel to the board route before native construction. Low-level callers retain their external resource-authority contract. |
| `drivers/display/a733_display_native.c:135` | Native admission checks panel identity, lane/format settings and exact timing when a port description is supplied. Existing scanout ownership is retained. |
| `drivers/usb/host/hid.c:102` | HID decodes report descriptors and usage capabilities; no keyboard or mouse product is selected in the class. |
| `drivers/usb/host/serial.c:41` | Serial leases use discovered endpoints and bridge protocol flags. TinyUSB keeps per-bridge recognition and configuration tables. |
| `drivers/usb/host/uvc.c:18` | UVC discovers formats, dimensions, intervals and endpoint alternatives. The current formats are YUYV and MJPEG. |
| `platform/a733/typec_native.c:222` | Type-C setup resolves board pins and supplies once. This controller adapter still requires FUSB302 on S_TWI1; another controller requires its own adapter. |

The native CSI workload still requires YUV422 input and NV16 output. OV5647
provides RAW10, so the current capture route refuses it. Its host checks prove
driver selection and lifecycle behavior. RAW10-to-NV16 processing remains open. Exposure,
gain and white-balance control IDs have query/dispatch support; the current
sensor adapters explicitly report them unsupported. Test-pattern selection is
implemented before streaming.

The panel class includes two Linux-derived examples, Innolux P079ZCA and
LG LH500WX1-SD03. Their data and sequences require no subsystem changes. The
current PLL planner refuses the Innolux 56.9 MHz mode; the LCD planner refuses
the LG panel's 1280-line height. These descriptions remain host examples.
Parallel LCD is representable but has no native board route. The fixed native
clock, geometry and transport limits remain separate from device identification.

Adding a sensor requires a small driver/table, an entry in the driver registry
and build sources, and a board module entry. Existing controller-compatible
formats then negotiate without core edits. Adding a panel requires a description
or small driver and a board/session selection with physical resource bindings.
Unsupported RAW processing, clock profiles or transport features require their
own controller work and validation.

## Evidence and qualification

Raw host evidence is under `~/.cache/sunstoneos/build/peripheral-classes-1101/`.
Task-owned downloaded references reside in its `reference-cache/` directory.
Repeat the sensor check with `--source reference-cache/camera-sensors-linux-v6.12/ov5647.c`
and the display check with `--source-cache reference-cache/display-class`, using
absolute paths from that build directory. The relocation record binds earlier
evidence paths to the same SHA-256 identities.
The sensor source, revision, hash, license and execution role are recorded in
[the sensor manifest](../../drivers/camera/sensor-sources.json). Panel, EDID and
DisplayID references are in [the display manifest](../display-class-sources.json).
USB source and recording identities remain in the existing
[UVC](../usb-uvc-sources.json), [HID](../usb-hid-sources.json) and
[serial](../usb-serial-sources.json) records.

Sensor lifecycle, chip-ID selection, mode negotiation, native allocation
layouts, capture/stream timing and retained CSI frame checks pass. Display
checks cover public EDID dumps, DisplayID, every implemented CTA timing against
Linux, attachment generations, panel sequences and actual DSI session binding
for 720p/1080p with retained failures. Independent sanitizer testing covers
20,000 mutated EDIDs.

USB checks cover the shared camera lifecycle, all advertised frame intervals,
original spans, stale release tickets and detach. Existing UVC protocol, HID
recording replay, serial protocol/lifetime and composed xHCI/TinyUSB class checks
pass. Type-C tests inject PMIC failure through the published power-authority
state and cover attachment, fault shutdown, retained resources and monitoring.

The OS-only, hello-compute and external mimus baseline builds pass. The selected
integration gate passes OS linkage, kernel boot and check-index verification;
its selection and logs are in `gate/gate-peripheral-classes-final/`. The USB-class
native objects also compile with class services and interrupt completion enabled.
No host check establishes board timing or power measurements.

The standalone DSI application configure encounters an existing rejected hunk in
`apps/storage-read-single-core/platform.patch`. The composed host fixture and
AArch64 compilation check the changed DSI application/provider branches. The
unrelated boot-patch rejection remains open.

The current [camera](camera-isp.md) and [display](display-g2d.md) status records
confirm no attached CSI sensor or selected panel. [USB status](usb.md) records
unqualified host peripherals and missing source-power/runner authority. No
eligible peripheral regression is submitted; there are no new board entry IDs.
Physical latency percentiles, throughput, CPU cost, idle power and visible pixels
remain unmeasured for this refactor.

The ISP owner must connect sensor-negotiated RAW media-bus formats and dimensions
to ISP0 input configuration, add the required RAW conversion and tuning, and
preserve its existing completion/retained-buffer contract. This change does not
edit `a733_isp0.c`, `isp0-*`, `apps/isp0/` or `sunstone_probe.h`.
