# Video, image codecs and deinterlacing

**Status: implemented.**

Native JPEG, H.264, HEVC and DI301 paths have host, instruction or device-model evidence. Numerical device output is unqualified.

**Software evidence:** source.

**Board-qualified scope:** None.

## Capabilities

| Capability | Status | Key figure | Evidence or design |
| --- | --- | --- | --- |
| JPEG, H.264, HEVC decode and DI301 deinterlacing | host-tested | none | [video-codecs.md](../video-codecs.md), [video-platform.md](../video-platform.md) |
| Decoder cold start of PLL_VE0 at 624 MHz (N=78, P0=3) | host-tested | 5 writes and at most 109 reads, once per power lifetime | [design](../designs/video-pll.md), `video-pll-host` |
| Encoder cold start (PLL_VE1) | open | none | [design](../designs/video-pll.md#encoder) |
| Numerical device output, physical decode | open | none | open work 1 |

## Open work

1. Qualify physical arithmetic, cache visibility, confinement, interrupts, drain, output and recovery for every codec.
2. Allow more than one decoded frame per image lifetime; OS decode accepts one, and repeated-frame throughput, IRQ completion, CPU cost and idle power are open in the [real-time audit](driver-realtime-audit.md).
3. Add H.264 fields, wider formats, long-term B references and unrestricted streaming; add HEVC Main10, larger geometry, slices, tiles, wavefront parallel processing and B pictures.
4. Add encoder rate control, unique per-picture inputs and wider modes, quality, geometry and repetition.
5. Replace modeled entropy, completion and reference contents with device results; programming matches the pinned vendor-library reference.

## Evidence

Design: [video decoder PLL](../designs/video-pll.md) (`docs/evidence/video-pll.jsonl`).

Board results are records in `docs/evidence/video.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-video-20261101.md).

<!-- evidence-table:begin -->
No board result is recorded in `docs/evidence/video.jsonl`.
<!-- evidence-table:end -->
