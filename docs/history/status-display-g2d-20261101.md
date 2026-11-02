# Display and G2D: historical status record 2026-11-01

This log holds the prose evidence of [the current status](../status/display-g2d.md) as it stood on the date above. It is not updated. Board results are in the records `docs/evidence/display-g2d.jsonl`.

**Status: implemented.**

Native HDMI, DSI, DisplayPort, immutable scanout and
retained G2D paths have host/model evidence. Native images need a reviewed
watchdog return path. No MIPI panel is attached or selected.

**Board-qualified scope:** None.

Visible sink output, physical pixels, DMA
visibility, IRQ delivery, electrical timing and recovery remain unqualified.

HDMI supports colorbar/framebuffer 720p60 and 1080p60 plans. Raw-26 MHz reference
checks calculate 73.9375 and 147.875 MHz pixel clocks; these are not timing
compliance measurements. DSI supports bounded low-power writes and four-lane
RGB888/loose RGB666 video. Read transactions and panel-bound electrical,
PLL, rail and wiring evidence remain open.

DisplayPort uses documented TYPEC24 routing, four lanes at 5.4 Gb/s, 720p60
RGB8 and assignments C/E. Its profile excludes USB3/PCIe sharing. Matching
analog/SERDES and AUX-clock authority, board revision, negotiation, orientation,
link training and sink output remain unqualified.

Scanout supports immutable selection and a two-to-four-frame replay ring.
RCQ completion, address readback and later frame-end do not establish retirement
of old pixel reads. Every slot stays immutable and retained; no application
produces new frames into the ring. GPU-to-HDMI capture converts two immutable
frames and closes GPU before HDMI start. Samples and hashes do not prove every
pixel or visible output.

The native scanout binding validates immutable frame/table mappings once per
prepare, request or replay. Host checks cover replay admission across phase
changes and refusal of changed mappings/table words at the next operation.
Per-access ownership checks remain. The [real-time audit](../status/driver-realtime-audit.md)
records tick-paced replay, G2D copy/guard work and unmeasured physical timing.

HDMI framebuffer, DisplayPort and GPU display share DE1 handling and DE_SYS
domain-1 preparation. DSI requires domains 0,1,2,3,8,9 ON at entry. VO0/VO1
provider code is not called by DSI or deinterlace. Deinterlace setup still
fails when DE_SYS is unpowered. [HDMI audio](../status/audio.md) has separate retained
streaming and drain limitations.

G2D supports fill, copy, source-over-solid, exact 2x and general scaling,
image-over-image blending and eight 32-bit RGB orders through 1280x720 with
5120-byte strides. Authenticated inputs, guarded command/output allocations
and exclusive IOMMU master 10 are implemented. Command construction rejects
wraparound and overlap; admitted command pages remain unchanged. Source-defined
task-end and mixer AHB reset precede output reads. These observations do not
prove physical drain or reusable memory; all resources stay retained.
Rotation/mirroring, YUV, other pixel sizes and translucent solid backgrounds
remain incomplete.

Evidence is under `~/.cache/sunstoneos/evidence/`:

- `hdmi-binding-20261018/` and `hdmi-ref26-20261020/root-integration.json`.
- `display-native-20261018/` and `display-frame-present-20261019/root-integration.json`.
- `displayport-native-20261020/root-integration.json` and
  [top-rate admission](../designs/displayport-top-rate.md).
- `gpu-display-capture-20261020/root-integration.json`.
- `g2d-native-20261020/root-integration.json`, `g2d-power-20261020/root-integration.json`,
  `g2d-success-20261020/root-integration.json` and `g2d-scale-20261020/root-integration.json`.

Current builds, command-span checks and pinned comparisons pass under
`~/.cache/sunstoneos/build/integrate-streams-1031/followup/`.
See [HDMI](../hdmi-native.md), [DSI](../dsi-video.md),
[DisplayPort](../displayport-native.md), [scanout](../display-scanout.md),
[G2D](../g2d.md) and [command admission](../designs/display-command-admission.md).
