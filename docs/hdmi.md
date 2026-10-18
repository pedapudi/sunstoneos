# HDMI controller and PHY

SunstoneOS implements a bounded HDMI controller, PHY and TV0 colorbar path for
the Orange Pi Zero 3W miniHDMI connector. The implementation accepts exact
1280×720 at 60 Hz and 1920×1080 at 60 Hz, with RGB888 and one pixel per clock.
It has not produced qualified physical output. A
[native resource binding](hdmi-native.md) connects the shared providers.
Qualified image-owned cold/analog inputs and a connected monitor remain
prerequisites.

The runtime is `drivers/display/a733_hdmi.c` and `a733_hdmi_phy.c`, with the
public interface in `include/a733_hdmi.h`. The [TV0 timing controller](tcon-tv.md)
generates the diagnostic pixels. The path uses TV0 at `0x05730000`, VO1 routing
at `0x05510000`, the byte-addressed HDMI controller at `0x05520000`, and TOP PHY
at `0x05600000`. The Synopsys C422 PHY is programmed through the controller's
internal I2C port. This path does not use the COMB0 USB/DisplayPort lanes.

## Admission requires actual shared ownership

One serialized privileged task owns fixed-address, zero-initialized controller
storage. Copies, callback reentry and concurrent leaf calls are unsupported.
Requests, callback context and reservation storage remain private and live.
The clock registry and controller must share one monotonic time history.

The acquire callback must provide an authentic common clock token, stable
24 MHz oscillator and SFR clocks, VO1 power domain 10, released bus resets,
and held PMIC rail reservations. Its exclusive VO1 scope covers both TV
controllers, HDMI/eDP, shared TOP registers, PLL consumers and domain/reset
writers. The common registry assigns bit 63 to this exclusive scope. The native
binding obtains its actual token before device access.

The resistor controls at `0x03000160` and `0x03000164` require common authority
for both complete shared words. Every writer must participate in that authority.
The runtime sets bit 12 of the first word and clears bits 31:24 of the second,
preserving their other fields. Local HDMI ownership alone is insufficient.

The provider must establish inactive consumers, prior transaction drain,
independent recovery, Device memory mappings, and analog-profile provenance.
The profile contains complete TOP words at offsets 0x00, 0x04, 0x20, 0x24, 0x28, 0x2c, 0x30 and
0x34, plus both resistor words. Preflight compares all of them before programming.
The initial PLL must be disabled. No reset image or rail voltage is synthesized.
In particular, the published setup leaves the charge-pump word at offset 0x34
inherited; its safe initial value remains a physical admission requirement.

The schematic identifies VDD08_HDMI and VCC18_HDMI; board software names DCDC2
and CLDO2 supplies. DCDC2 is shared with the NPU. Connector 5 V comes through a
diode from VCC5V, with no connector power GPIO shown. The native binding obtains
rail ownership and reads programmed voltages.
Electrical compatibility still requires physical qualification.

## Preparation and output are separate operations

Preparation mutes output, verifies hotplug presence, reads the sink's Extended
Display Identification Data (EDID), programs the controller and PHY, and prepares
TV0 with its enable bit clear. EDID reads use the Display Data Channel (DDC),
including segment addressing. Each byte transaction has a 10 ms deadline;
errors stop preparation without automatic retries.

Admission requires a complete, checksummed digital EDID version 1.3 or 1.4,
up to three extension blocks, an HDMI identification block, and an advertised
selected CTA video code: 4 for 720p60 or 16 for 1080p60. Unsupported sizes,
malformed data blocks and DVI sinks are refused. A published maximum TMDS clock
below the requested clock also causes refusal. Timing-only descriptors and
user overrides are not implemented.

Limited-range RGB is the default. The actual controller color-space converter
maps the full-range TV colorbar using diagonal coefficients `0x1b7c`, offsets
`0x0020`, scale 1 and eight-bit color depth. The nominal integer endpoints are
16 and 235. Coefficients are written by column, A/B/C row, low byte before high
byte, as in the pinned Synopsys driver. AVI quantization uses the CTA default
limited range. Explicit full-range output bypasses conversion, advertises full
range, and requires the EDID RGB quantization-selectable flag. Physical color
accuracy and sub-code rounding remain unqualified.

The TOP PLL uses the published 24 MHz table rows for 74.25 and 148.5 MHz.
Both use a 2376 MHz VCO, divided by 32 or 16. The source's automatic search
limit of 2250 MHz contradicts these rows; the manual allows 2520 MHz. This
implementation admits only the exact rows and does not use the search algorithm.
It programs the documented factor and analog fields, observes lock within
20 ms, waits at least 20 microseconds, then enables the PLL output.

The internal PHY receives three MPLL words and three board-specific drive
words. Each word is read back. SVSRET stays low for at least 5 microseconds;
internal I2C operations and PHY lock observation have bounded deadlines.
Lock observations establish register-programming completion, not sink acceptance.

Start enables TV0, waits at least 50 ms, rechecks TOP PLL lock, PHY lock and
TV health, and then unmutes. Regular active service polls hotplug and those
health signals. Hotplug pulses between observations can be missed; interrupt
handling remains required. A sampled disconnect or TV data fault schedules stop.

Stop mutes, requests PHY power-down, disables transmit and TV0, and gates the
controller and PLL output. All ownership remains held. These control writes do
not prove independent output drain or permit shared rail/reset release. An I/O,
authority or deadline failure stops further device access. Retention retries
perform metadata operations only, including after an uncertain write.

## Source and validation evidence

The primary implementation reference is the Orange Pi A733 kernel revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`, under
`bsp/drivers/drm/sunxi_device/hardware/lowlevel_hdmi20`, together with the board
DTS and A733 User Manual V1.00. The CSC reference is
`drivers/gpu/drm/bridge/synopsys/dw-hdmi.c` at that revision, SHA-256
`681e2f32ed671e8f759fa84cb108416e76adae3bf94a07a11684d5bbbbfe00ae`.
The matching U-Boot setup was inspected for analog initialization; it also
leaves the charge-pump word inherited. Downloads and source manifests remain
outside Git under `~/.cache/sunstoneos/sources/hdmi-research/`.

The selected check composes the actual controller, PHY and TV0 runtime using
fabricated fixture authority and modeled registers. It covers both modes,
limited/full range, extended EDID reads, startup delays, hotplug stop, and
checkpoint-based faults without repeating preparation for each fault. A separate
comparison consumes the same trace and unchanged primary source bytes.
AArch64 compilation checks the native freestanding implementation. Evidence
belongs under `~/.cache/sunstoneos/evidence/hdmi-controller-20261016/`.
The composed sanitizer check, trace comparison and three AArch64 compilations
passed. The largest reported individual native stack frame is 96 bytes; that
figure excludes nested calls and provider callbacks. These checks do not
establish native resource acquisition or physical output.

Framebuffer scanout requires a DE-to-TV route and owned memory. A single
XRGB8888 frame needs 3,686,400 bytes at 720p or 8,294,400 bytes at 1080p,
exceeding the current 2 MiB cached pool. Audio, CEC, HDCP, hotplug interrupts,
fractional refresh, additional modes/formats, deep color, scrambling, live mode
changes, safe ownership release and eDP remain required for full hardware support.
