# Audio

**Status: implemented.**

I2S0 capture, playback, cyclic cancellation and drain run through the FreeRTOS DMA service, the cyclic stream driver and the native cache adapter.

**Board-qualified scope:** Internal I2S0 loopback capture and playback, cyclic
cancellation and drain, and raw-ring overrun pass a returning board diagnostic
on port 3-3. External microphone and headset operation remain unqualified.

## Open work

1. Measure the unguarded DMA ordering margin.
2. Qualify external microphone and headset operation, including physical pairing, interoperability and safe shutdown.
3. Keep the generic capture and paced or recorded input services independent of the external recognition application.

## Evidence

Board results are records in `docs/evidence/audio.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-audio-20261101.md).

<!-- evidence-table:begin -->
3 records in `docs/evidence/audio.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793561347028719264-5a2bab37` | 2026-11-01 | 3-3 | `63840ec870d4` | returned | passed | run_total 10.522 s |
| `1793561377196205560-f9163edf` | 2026-11-01 | 3-3 | `63840ec870d4` | returned | passed | run_total 11.743 s |
| `1793561406349236252-f1275fd8` | 2026-11-01 | 3-3 | `63840ec870d4` | returned | passed | run_total 10.963 s |
<!-- evidence-table:end -->
