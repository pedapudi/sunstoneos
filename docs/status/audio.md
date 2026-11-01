# Audio

**Status: implemented.**

I2S0 finite/repeated capture, playback, duplex,
hardware TX rings, guarded cyclic RX and DMA0 interrupt notification have
software evidence. USB periods have copied ownership; PCM files supply playback.
The OS capture adapter consumes cyclic staging through exact tickets without
exposing raw DMA pages. Recorded and paced PCM also use the application kit's
capture buffer and headroom without a device grant or recognizer dependency.

**Board-qualified scope:** None.

No microphone or headset is attached. Capture, playback,
continuity, IRQs, DMA visibility, serializer drain and sound remain unqualified.

Cyclic RX needs a physical ordering bound: the package counter has no
source-established ordering against destination writes or cursor updates.
Both uncertainties must measure below one period. Four-period rings give a
128 ms application copy deadline; no qualifying record exists.
`i2s-hardware-cyclic-20261019/root-integration.json` and
`i2s-hardware-cyclic-20261019/identity-reuse-clarification.json` under `~/.cache/sunstoneos/evidence/`
define that modeled scope. Retained-allocation exclusion has focused evidence
in [DMA status](memory-dma.md#dma).

Evidence at `~/.cache/sunstoneos/build/audio-1031/frozen/README.md` binds the
current host/model images, hashes and reviews. Actual FreeRTOS/provider cases
keep a busy canceled writer in DRAINING, preserve a held copy through overrun,
and refuse stale tickets after both duplex directions close and reopen.
Sanitizer, AArch64 and linked-image checks cover device-free PCM replay with an
empty `MIMUS_DIR`. Registers, DMA effects and cache visibility are modeled.

I2S and OS audio images retain their legacy supervised lifetime. Returning-probe
conversion must retire workers, client leases, DMA, both audio directions and
USB, then join monitor secondary shutdown and interrupt restoration. No audio
board run is queued.

PCM EOF establishes declared DMA consumption and FIFO empty. It does not
establish serializer drain, so TX/GEN, clocks, pins and buffers stay owned;
REAP/CLOSE are refused. `i2s-file-playback-20261019/root-integration.json`
and `i2s-runtime-pcm-20261019/` bind the playback checks.

HDMI audio implements finite and bounded stereo S16LE/48 kHz streaming through
DMA0, I2S3 and the HDMI sampler with a TV0 pattern. Streaming lasts up to twelve
seconds within a twenty-second session. Evidence is in
`hdmi-audio-nc-20261020/root-integration.json`,
`hdmi-audio-usb-20261020/root-integration.json` and
`hdmi-stream-1028/frozen-submission/`. Final provider corrections have narrower
evidence than the nominal streaming image. FIFO empty does not prove wire drain
or transmitted audio; current retained applications require power removal.

Bluetooth HFP/CVSD, optional mSBC and A2DP are implemented with modeled
completion. Credits do not establish headset rendering or wire drain.
Physical pairing, microphone/speaker interoperability and safe shutdown remain
open. Recognition belongs to the external application.

See [I2S stream](../i2s-stream.md), [native application](../i2s-application.md),
[platform](../i2s-platform.md), [HDMI audio](../hdmi-audio.md),
[HDMI preparation](../hdmi-audio-preparation.md) and [headset](../bluetooth-hfp.md).
