# Display and G2D

**Status: implemented.**

Native HDMI, DSI, DisplayPort, immutable scanout and retained G2D paths have host and model evidence. Native images need a reviewed watchdog return path. No MIPI panel is attached or selected.

**Software evidence:** host, source.

**Board-qualified scope:** None.

## Capabilities

| Capability | Status | Key figure | Evidence or design |
| --- | --- | --- | --- |
| G2D command builder and display-ring CPU span validation | host-tested | accepted pages byte-identical to the earlier builder | [design](../designs/display-command-admission.md), `display-command-admission-host` |
| DisplayPort, four lanes at 5.4 Gb/s, 1280 by 720 at 60 Hz | host-tested | 22 matching PHY traces, 1483 modeled preparation operations | [design](../designs/displayport-top-rate.md), `displayport-top-rate-host` |
| HDMI, DSI, immutable scanout and retained G2D paths | host-tested | none | [g2d.md](../g2d.md), [displayport-native.md](../displayport-native.md) |
| Visible sink output and physical pixels | open | none | open work 1 and 2 |

## Open work

1. Show visible sink output and physical pixels; qualify DMA visibility, IRQ delivery, electrical timing and recovery.
2. Establish DisplayPort negotiation: analog and SERDES authority, AUX clock, board revision, orientation, link training and sink output.
3. Obtain panel-bound electrical, PLL, rail and wiring evidence, and the DSI read transactions.
4. Add per-access ownership checks to the G2D command path.
5. Add G2D rotation, mirroring, YUV, other pixel sizes and translucent solid backgrounds.

## Evidence

Designs: [command span validation](../designs/display-command-admission.md) (`docs/evidence/display-command-admission.jsonl`) and [DisplayPort top rate](../designs/displayport-top-rate.md) (`docs/evidence/displayport-top-rate.jsonl`).

Board results are records in `docs/evidence/display-g2d.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-display-g2d-20261101.md).

<!-- evidence-table:begin -->
No board result is recorded in `docs/evidence/display-g2d.jsonl`.
<!-- evidence-table:end -->
