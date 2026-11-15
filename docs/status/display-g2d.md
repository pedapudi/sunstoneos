# Display and G2D

**Status: implemented.**

Native HDMI, DSI, DisplayPort, immutable scanout and retained G2D paths have host and model evidence. Native images need a reviewed watchdog return path. No MIPI panel is attached or selected.

**Software evidence:** host, source.

**Board-qualified scope:** the OS-image G2D service on port 3-1: open and close (run `1794705812820143004-c987f8e7`), one ARGB8888 fill through IOMMU master 10 with INTID 83 completion (run `1794714427314630502-a44050c7`), two copies in one session, one converting ARGB8888 to BGRA8888 (run `1794725199336568290-5c9e9da6`), exact ARGB8888 rotates at 0, 90, 180 and 270 degrees through the rotate module (run `1794743298175050163-4ed14c80`), and NV16-to-ARGB8888 conversion of a 640 by 480 frame with the BT.709 full table plus scaling to 320 by 240 and 224 by 224 (run `1794752244516494656-bd8feb22`).

## Capabilities

| Capability | Status | Key figure | Evidence or design |
| --- | --- | --- | --- |
| G2D command builder and display-ring CPU span validation | host-tested | accepted pages byte-identical to the earlier builder | [design](../designs/display-command-admission.md), `display-command-admission-host` |
| DisplayPort, four lanes at 5.4 Gb/s, 1280 by 720 at 60 Hz | host-tested | 22 matching PHY traces, 1483 modeled preparation operations | [design](../designs/displayport-top-rate.md), `displayport-top-rate-host` |
| OS-image G2D fill through IOMMU master 10, INTID 83 completion ([g2d-os.md](../g2d-os.md#operations)) | board-qualified on 3-1 | 420 by 271 surface, CRC as predicted; launch to interrupt 11.5 us | `1794714427314630502-a44050c7` |
| OS-image G2D copy, unconverted and ARGB8888 to BGRA8888, two operations in one session ([g2d-os.md](../g2d-os.md#operations)) | board-qualified on 3-1 | 395 by 257 surfaces, CRC and 12 samples as predicted; alpha-0 pixels written as 0; launch to interrupt 130.5 us for 32,148 pixels | `1794725199336568290-5c9e9da6` |
| OS-image G2D copy of alpha-0 pixels on four alternative blender configurations ([g2d-os.md](../g2d-os.md#copy-paths)) | board-measured on 3-1 | none copies exactly: three write 0x00000000 like the vendor path, and the premultiplied words multiply colour by alpha | `1794732202569746479-c9b5e324` |
| OS-image G2D rotate of ARGB8888 at 0, 90, 180 and 270 degrees through the rotate module ([g2d-os.md](../g2d-os.md#first-rotate-stage)) | board-qualified on 3-1 | 96 by 40 rectangle, every CRC as predicted, alpha-0 pixels copied exactly; 90 turns clockwise; launch to interrupt 16 to 23 us | `1794743298175050163-4ed14c80` |
| OS-image G2D convert of NV16 camera frames to RGB with scaling, four YUV-to-RGB tables ([g2d-os.md](../g2d-os.md#second-conversion-stage)) | board-qualified on 3-1 | 640 by 480 in 1050 us (292 Mpixel/s), CRC as predicted; luma below 16 raised to 16 by the YUV blender (csc0 at pipe 0's input keeps it), rounding down; scales to 320 by 240 and 224 by 224 in 1039 us; one A76 in scalar C without NEON 2646 us | `1794752244516494656-bd8feb22`, `1794749218366895381-6f184f31` |
| HDMI, DSI, immutable scanout and retained G2D paths | host-tested | none | [g2d.md](../g2d.md), [displayport-native.md](../displayport-native.md) |
| Visible sink output and physical pixels | open | none | open work 1 and 2 |

## Display outputs

**Board-qualified scope:** None.

**Software evidence:** host, source.

The display engine's scanout and composition, HDMI, MIPI DSI and DisplayPort have host and model evidence only. No sink output, panel or physical pixel is qualified on either board. See [g2d.md](../g2d.md) and [displayport-native.md](../displayport-native.md).

## G2D

**Board-qualified scope:** the OS-image G2D service on port 3-1: open and close (run `1794705812820143004-c987f8e7`), one ARGB8888 fill through IOMMU master 10 with INTID 83 completion (run `1794714427314630502-a44050c7`), two copies in one session, one converting ARGB8888 to BGRA8888 (run `1794725199336568290-5c9e9da6`), exact ARGB8888 rotates at 0, 90, 180 and 270 degrees through the rotate module (run `1794743298175050163-4ed14c80`), and NV16-to-ARGB8888 conversion of a 640 by 480 frame with the BT.709 full table plus scaling to 320 by 240 and 224 by 224 (run `1794752244516494656-bd8feb22`).

**Software evidence:** host, source.

The OS-image service is described in [g2d-os.md](../g2d-os.md). Exact copies of camera frames and packed sensor data use a rotate at 0 degrees; the blit treats alpha as coverage. NV16-to-RGB conversion and scaling are board-qualified: the vendor's page raises luma below 16 to 16 because the blender works in YUV, and both converters round down (run `1794752244516494656-bd8feb22`). Blending, flipped rotates, YUV rotates, the X-byte formats and the retained native paths are host-tested or open.

## Open work

1. Show visible sink output and physical pixels; qualify DMA visibility, IRQ delivery, electrical timing and recovery.
2. Establish DisplayPort negotiation: analog and SERDES authority, AUX clock, board revision, orientation, link training and sink output.
3. Obtain panel-bound electrical, PLL, rail and wiring evidence, and the DSI read transactions.
4. Add per-access ownership checks to the G2D command path.
5. Keep full-range luma 0 to 15: convert at pipe 0's input with csc0 and blend in RGB, the order that kept it on the board ([second conversion stage](../g2d-os.md#second-conversion-stage)), then remove the diagnostic variants; measure a NEON conversion on one A76 once applications may build with FP and SIMD registers, since the engine is 2.5 times faster than scalar C only; then RGB888 output, NV12 input, NV12 and YUV420 camera rotation, flips on the board, a mixer job and a rotate in one session (the stray masking), other pixel sizes and translucent solid backgrounds.

## Evidence

Designs: [command span validation](../designs/display-command-admission.md) (`docs/evidence/display-command-admission.jsonl`) and [DisplayPort top rate](../designs/displayport-top-rate.md) (`docs/evidence/displayport-top-rate.jsonl`).

Board results are records in `docs/evidence/display-g2d.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-display-g2d-20261101.md).

<!-- evidence-table:begin -->
10 records in `docs/evidence/display-g2d.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1794711214889069759-0a9ae5d4` | 2026-11-14 | 3-1 | `46ba128398d1` | watchdog-reset | failed | run_total 22.425 s, run_total 22.42 s |
| `1794705812820143004-c987f8e7` | 2026-11-15 | 3-1 | `6708922d8244` | returned | passed | run_total 46.225 s, version 0 x10112114raw, open_time 7945 us, row_request_to_on 39 us, close_after_open 133 us, status_reads 2, pck_command 0 us, run_total 46.2 s |
| `1794714427314630502-a44050c7` | 2026-11-15 | 3-1 | `4bc97e09e7b6` | returned | passed | run_total 22.072 s, open_time 212 us, attach 197 us, submit_to_launch 688 us, launch_to_irq 11.5 us, irq_to_done 116 us, submit_to_done 815 us |
| `1794722460072814405-2b77c3eb` | 2026-11-15 | 3-1 | `b660bab97963` | returned | failed | run_total 22.382 s, attach 332 us, launch_to_irq 130.2 us, submit_to_done 1112.4 us |
| `1794723622887180087-445859c1` | 2026-11-15 | 3-1 | `58fa4183c75c` | returned | failed | run_total 22.545 s, attach 332 us, launch_to_irq 129.9 us, submit_to_done 1112.4 us |
| `1794725199336568290-5c9e9da6` | 2026-11-15 | 3-1 | `916988f84c39` | returned | passed | run_total 23.279 s, attach 332 us, launch_to_irq_op1 130.5 us, launch_to_irq_op2 36.4 us |
| `1794732202569746479-c9b5e324` | 2026-11-15 | 3-1 | `8e5cdd4c6929` | returned | passed | run_total 34.691 s, reset 12.956 s, launch 6.516 s, launch_to_irq_vendor 20.875 us, launch_to_irq_no_fill 18.833 us |
| `1794743298175050163-4ed14c80` | 2026-11-15 | 3-1 | `7a078999890f` | returned | passed | run_total 25.468 s, launch_to_irq_max 23.46 us, launch_to_irq_min 16.25 us |
| `1794749218366895381-6f184f31` | 2026-11-15 | 3-1 | `31ab76340302` | returned | failed | run_total 22.405 s, launch_to_irq_601_limited 1050.46 us, launch_to_irq_709_full 1049.08 us, engine_rate 292.4 Mpixel/s, cpu_first 2717.96 us, cpu_best 2695.96 us |
| `1794752244516494656-bd8feb22` | 2026-11-15 | 3-1 | `40ef88476734` | returned | passed | run_total 24.082 s, launch_to_irq_vendor 1050.79 us, launch_to_irq_csc0 1049.21 us, launch_to_irq_scale_320 1039.33 us, launch_to_irq_scale_224 1038.38 us, engine_rate 292.4 Mpixel/s, cpu_first 2670.5 us, cpu_best 2646.46 us |
<!-- evidence-table:end -->
