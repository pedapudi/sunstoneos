# Video, image codecs and deinterlacing

**Status: implemented.**

Native JPEG, H.264, HEVC and DI301 paths have host, instruction or device-model evidence. One baseline JPEG decode in the OS image has matched its reference on the board; every other numerical device output is unqualified.

**Software evidence:** source.

**Board-qualified scope:** The OS decoder service's open, cold start and close on port 3-1, and one decode of the 48-by-32 baseline JPEG fixture at the CPU top points: 661 writes equal to the cold model, completion on INTID 81, both IOMMU masters fault-free and the 2,304 output bytes equal to the reference ([video-decode-os.md](../video-decode-os.md#board-stages)). The runner's record of the decode run is `host-error`, because the decoder's answer could not be saved; its measurements are host.py's reading of the run's output.

## Capabilities

| Capability | Status | Key figure | Evidence or design |
| --- | --- | --- | --- |
| JPEG, H.264, HEVC decode and DI301 deinterlacing | host-tested | none | [video-codecs.md](../video-codecs.md), [video-platform.md](../video-platform.md) |
| Decoder cold start of PLL_VE0 at 624 MHz (N=78, P0=3) | host-tested | 5 writes and at most 109 reads, once per power lifetime | [design](../designs/video-pll.md), `video-pll-host` |
| Encoder cold start (PLL_VE1) | open | none | [design](../designs/video-pll.md#encoder) |
| Numerical device output, baseline JPEG in the OS image | board-qualified, one fixture | trigger to INTID 81 13.5 us; decode request 8.06 ms | [video-decode-os.md](../video-decode-os.md#board-stages) |
| Numerical device output, H.264, HEVC and other JPEG inputs | open | none | open work 1 |

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
4 records in `docs/evidence/video.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1794709569136470404-9a7b6a30` | 2026-11-15 | 3-1 | `34e7e5471665` | returned | failed | run_total 46.175 s |
| `1794712072331160946-bd447f26` | 2026-11-15 | 3-1 | `c1a4e4fcdd0b` | watchdog-reset | failed | run_total 23.471 s |
| `1794714415147688285-6b3ec3f5` | 2026-11-15 | 3-1 | `7d63776e0424` | returned | passed | run_total 21.826 s, entry_hold 75 us, hold_a 59 us, lock_wait 12 us, hold_b 99 us, owner_row_wait 7858 us, close 248 us |
| `1794742351867366265-ef057195` | 2026-11-15 | 3-1 | `ada5dab1fd38` | host-error | passed | cpu_l 1794 MHz, cpu_b 2002 MHz, dsu 1274 MHz, clock_wait 128955 us, writes 661 count, intid81_count 1 count, trigger_to_irq 13.5 us, irq_to_drained 42.1 us, to_trigger 4338.7 us, drained_to_written 3661.4 us, decode_request 8055.7 us, lock_reads 4 count |
<!-- evidence-table:end -->
