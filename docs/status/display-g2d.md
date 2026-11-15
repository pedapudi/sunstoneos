# Display and G2D

**Status: implemented.**

Native HDMI, DSI, DisplayPort, immutable scanout and retained G2D paths have host and model evidence. Native images need a reviewed watchdog return path. No MIPI panel is attached or selected.

**Software evidence:** host, source.

**Board-qualified scope:** the OS-image G2D service on port 3-1: open and close (run `1794705812820143004-c987f8e7`), and one ARGB8888 fill through IOMMU master 10 with INTID 83 completion (run `1794714427314630502-a44050c7`).

## Capabilities

| Capability | Status | Key figure | Evidence or design |
| --- | --- | --- | --- |
| G2D command builder and display-ring CPU span validation | host-tested | accepted pages byte-identical to the earlier builder | [design](../designs/display-command-admission.md), `display-command-admission-host` |
| DisplayPort, four lanes at 5.4 Gb/s, 1280 by 720 at 60 Hz | host-tested | 22 matching PHY traces, 1483 modeled preparation operations | [design](../designs/displayport-top-rate.md), `displayport-top-rate-host` |
| OS-image G2D fill through IOMMU master 10, INTID 83 completion ([g2d-os.md](../g2d-os.md#operations)) | board-qualified on 3-1 | 420 by 271 surface, CRC as predicted; launch to interrupt 11.5 us | `1794714427314630502-a44050c7` |
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
5 records in `docs/evidence/display-g2d.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1794711214889069759-0a9ae5d4` | 2026-11-14 | 3-1 | `46ba128398d1` | watchdog-reset | failed | run_total 22.425 s, run_total 22.42 s |
| `1794705812820143004-c987f8e7` | 2026-11-15 | 3-1 | `6708922d8244` | returned | passed | run_total 46.225 s, version 0 x10112114raw, open_time 7945 us, row_request_to_on 39 us, close_after_open 133 us, status_reads 2, pck_command 0 us, run_total 46.2 s |
| `1794714427314630502-a44050c7` | 2026-11-15 | 3-1 | `4bc97e09e7b6` | returned | passed | run_total 22.072 s, open_time 212 us, attach 197 us, submit_to_launch 688 us, launch_to_irq 11.5 us, irq_to_done 116 us, submit_to_done 815 us |
| `1794722460072814405-2b77c3eb` | 2026-11-15 | 3-1 | `b660bab97963` | returned | failed | run_total 22.382 s, attach 332 us, launch_to_irq 130.2 us, submit_to_done 1112.4 us |
| `1794723622887180087-445859c1` | 2026-11-15 | 3-1 | `58fa4183c75c` | returned | failed | run_total 22.545 s, attach 332 us, launch_to_irq 129.9 us, submit_to_done 1112.4 us |
<!-- evidence-table:end -->
