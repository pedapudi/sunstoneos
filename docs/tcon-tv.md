# TV0 timing and diagnostic pixels

The native TV0 controller supplies an eight-bit hardware colorbar to the
[HDMI output path](hdmi.md). It accepts 720p60 and 1080p60 with exact pixel
clocks of 74.25 and 148.5 MHz. Framebuffer input and physical output remain
unqualified. The interface is `include/a733_tcon_tv.h`; the implementation is
`drivers/display/a733_tcon_tv.c`.

The A733 node named TCON3 in the published device tree is physical TV0 at
`0x05730000`. TV1 serves the eDP route. VO1 TOP selects TV0-to-HDMI with bit 1
clear and the HDMI PHY clock with bit 3 clear at `0x05510000`. Gate bits 20 and
28 at TOP offset 0x20 enable TV0 and HDMI. All shared fields outside these masks
are preserved under the complete HDMI reservation.

| Profile | Active pixels | Total pixels | Horizontal front/sync/back | Vertical front/sync/back |
| --- | --- | --- | --- | --- |
| 720p60 | 1280 × 720 | 1650 × 750 | 110 / 40 / 220 | 5 / 5 / 20 |
| 1080p60 | 1920 × 1080 | 2200 × 1125 | 88 / 44 / 148 | 4 / 5 / 36 |

The runtime programs strict timing, one pixel per clock, positive horizontal
and vertical sync, the published pixel-clock polarity, and the color-check
source. It writes vertical total in half-lines, horizontal total minus one,
and the source-defined back-porch and sync widths. Start delay is vertical
blanking minus five, capped at 31 as in the published implementation.

Preparation leaves TV_EN clear. The HDMI coordinator enables TV0 only after
controller and PHY preparation, then observes its startup wait before unmuting.
Stop clears TV_EN. Underflow and lost-data flags at offset 0xfc are observed
without clearing their R/W0C evidence. Disable and frame status cannot establish
that downstream output has drained.

The A733 manual and published C structures disagree on several legacy fields.
The implementation follows the manual's writable masks:

- Global-control bit 31 is reserved and preserved; the legacy global-enable
  write is omitted.
- Vertical total is written at BASIC1. The reserved high half of BASIC4 is
  preserved instead of receiving the legacy duplicate vertical total.
- Removed interlace, color-enhancement and filled-data register writes are
  omitted. The documented fill-enable bit is cleared.
- Documented I/O polarity and tristate fields use bits 27:24. Their values
  match the source branch without `HAVE_DEVICE_COMMON_MODULE`.
- Pixel depth is eight bits for the full-range diagnostic pattern. The
  source's six-bit initialization does not define this selected profile.

The focused TV assertions and primary-reference comparison run inside the
single composed HDMI batch. They compare the timing words, reserved-bit
preservation, TOP route and deferred enable with manually transcribed expected
fields. The comparison also checks that the pinned manual contains the cited
field names; it does not extract every field definition from the manual.
No separate TV workload or older DSI suite is needed. Actual DE producer
routing, further TV modes, eDP, clock/reset acquisition, monitor output and
independent output-drain proof remain required.
