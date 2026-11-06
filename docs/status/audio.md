# Audio

**Status: implemented.**

I2S0 capture, playback, cyclic cancellation and drain run through the FreeRTOS DMA service, the cyclic stream driver and the native cache adapter.

Every OS image links I2S capture, and it starts in a session that grants the I2S device with a valid audio record. The headset capture service, with the Bluetooth stack and mSBC, is a library linked into an image whose application declares the headset device ([design](../designs/one-os-image.md)).

**Board-qualified scope:** Internal I2S0 loopback capture and playback, cyclic
cancellation and drain, and raw-ring overrun pass a returning board diagnostic
on port 3-3. External microphone and headset operation remain unqualified.

## Capabilities

| Capability | Status | Key figure | Evidence or design |
| --- | --- | --- | --- |
| I2S0 loopback capture and playback, cyclic cancellation and drain, raw-ring overrun | qualified on the board | 10.5 to 11.7 s per returning diagnostic | [audio.jsonl](../evidence/audio.jsonl) |
| HDMI audio streaming through I2S3, four 4,096-byte periods, up to 576,000 frames | host-tested | 50,177 frames and 50 periods in the modeled run | [design](../designs/hdmi-audio-streaming.md), `hdmi-audio-streaming-software` |
| Bluetooth headset voice-recognition capture (CVSD and mSBC segments) | host-tested | 2,090,169 owner instructions per 48,000-byte segment | [design](../designs/bluetooth-voice-recognition.md), `bluetooth-voice-recognition-measure` |
| External microphone and headset operation | open | none | open work 2 |

## Open work

1. Measure the unguarded DMA ordering margin.
2. Qualify external microphone and headset operation, including physical pairing, interoperability and safe shutdown.
3. Keep the generic capture and paced or recorded input services independent of the external recognition application.
4. Allocate the headset state and the capture buffer from the unified ledger at the grant, sized from the session's declared sizes. Both are static today: the headset library holds 948,008 bytes of `.bss`, and the capture buffer of the headset fixture application (160,000 mono 16-bit frames) holds 323,648 bytes.

## Evidence

Designs: [HDMI audio streaming](../designs/hdmi-audio-streaming.md) (records in `docs/evidence/hdmi-audio-streaming.jsonl`) and [headset voice recognition](../designs/bluetooth-voice-recognition.md) (records in `docs/evidence/bluetooth-voice-recognition.jsonl`).

Board results are records in `docs/evidence/audio.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-audio-20261101.md).

<!-- evidence-table:begin -->
3 records in `docs/evidence/audio.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793561347028719264-5a2bab37` | 2026-11-01 | 3-3 | `63840ec870d4` | returned | passed | run_total 10.522 s |
| `1793561377196205560-f9163edf` | 2026-11-01 | 3-3 | `63840ec870d4` | returned | passed | run_total 11.743 s |
| `1793561406349236252-f1275fd8` | 2026-11-01 | 3-3 | `63840ec870d4` | returned | passed | run_total 10.963 s |
<!-- evidence-table:end -->
