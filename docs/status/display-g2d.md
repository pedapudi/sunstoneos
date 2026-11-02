# Display and G2D

**Status: implemented.**

Native HDMI, DSI, DisplayPort, immutable scanout and retained G2D paths have host and model evidence. Native images need a reviewed watchdog return path. No MIPI panel is attached or selected.

**Software evidence:** host, source.

**Board-qualified scope:** None.

## Open work

1. Show visible sink output and physical pixels; qualify DMA visibility, IRQ delivery, electrical timing and recovery.
2. Establish DisplayPort negotiation: analog and SERDES authority, AUX clock, board revision, orientation, link training and sink output.
3. Obtain panel-bound electrical, PLL, rail and wiring evidence, and the DSI read transactions.
4. Add per-access ownership checks to the G2D command path.
5. Add G2D rotation, mirroring, YUV, other pixel sizes and translucent solid backgrounds.

## Evidence

Board results are records in `docs/evidence/display-g2d.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-display-g2d-20261101.md).

<!-- evidence-table:begin -->
No board result is recorded in `docs/evidence/display-g2d.jsonl`.
<!-- evidence-table:end -->
