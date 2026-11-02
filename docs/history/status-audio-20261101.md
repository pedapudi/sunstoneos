# Audio: historical status record 2026-11-01

This log holds the prose evidence of [the current status](../status/audio.md) as it stood on the date above. It is not updated. Board results that have queue entries are in the records `docs/evidence/audio.jsonl`.

**Status: implemented.**

I2S0 capture, playback, cyclic cancellation and drain run through the FreeRTOS
DMA service, the cyclic stream driver and the native cache adapter.

**Board-qualified scope:** Internal I2S0 loopback capture and playback, cyclic
cancellation and drain, and raw-ring overrun pass a returning board diagnostic
on port 3-3. External microphone and headset operation remain unqualified.

The diagnostic in `apps/i2s/probe.c` uses the FreeRTOS DMA service, cyclic stream
driver and native cache adapter. I2S0 CTL bit 3 connects DOUT to DIN internally,
as specified in A733 manual section 17.1.8.1. The format is 16 kHz mono in
32-bit containers, with eight 1,024-byte periods and a 24.576 MHz module clock.
The audio driver preserves reserved AUDIO1 control bit 24, accepts the observed
LDO-on/PLL-off baseline and waits for lock, settling and effective output gates.

The board result for entry `1793561377196205560-f9163edf` verifies 18,419 patterned
samples in 72 capture copies across three cancel/drain/close/reopen rounds.
It records 150 DMA interrupts, three completed retirements and no retained fault.
Entry `1793561406349236252-f1275fd8` verifies the held capture copy after raw-ring
overrun, then retires both streams and returns normally. The same image's
audio-disabled control, `1793561347028719264-5a2bab37`, also returns normally.

Retirement returns exact client tickets, cancels and drains both directions,
reaps and closes each stream, and waits for the global DMA MBUS FIFO to empty
before reuse. The hardware clears its package counter during cancellation.
Canceled progress preserves the last ACTIVE observation as a lower bound;
channel completion and cache ownership establish drain. The diagnostic retains
original fault callers and operands before later service refusals can hide them.

Final cleanup masks the DMA interrupt, suspends its worker and releases parent
leases. The monitor restores its saved DMA routing, security, clock and interrupt
fields before returning through the shared probe contract. CPU operating points
and supply voltages retain their inherited settings.

## Secondary CPU ownership and host evidence

The probe acquires acknowledged static OFF before whole-hierarchy cache
maintenance. It restores the exact original policies after final cache cleanup
and receipt preparation. It accepts matched static or dynamic OFF policies and
preserves either inherited GIC wake setting. Only changed secondary policy words
are written; CPU0 and cluster policies retain their original values.

The CPU1-specific check writes only `0x07053000` and verifies neighboring pages.
The aggregate check acquires and restores all seven secondaries. Return executes
18,944 data-cache operations and one I-cache invalidation before policy release.
Acquisition and restoration failures retain their original inputs, keep the
watchdog armed and prevent normal return. Three independent reviewers approve
the design, source and linked image; their records are under
`~/.cache/sunstoneos/build/audio-1031/policy-review/`.

Sanitizer checks, twenty focused cases, seventeen CPU ownership cases
and seventeen cyclic FreeRTOS cases pass under
`~/.cache/sunstoneos/build/audio-1031/counter-drain/`. The models cover delayed
readbacks and busy drain, counter reset at idle, overrun, held copies, stale
tickets, wraps and reopening. The OS-only, hello-compute and external mimus
baseline builds, selected integration gate and check index pass there too.

The returning image SHA-256 is
`63840ec870d4045a3537c6a8fdcce95b97247cdfc4bb71d224f5147b9fa5019b`.
Its ELF, inputs, expectations, README, commands and every submission result are
under `~/.cache/sunstoneos/build/audio-1031/counter-drain/`.
Shared patches `0006` through `0011` are under `shared-patches/` beside that
directory and pass `git apply --check`. Patch 0006 replaces patch 0005; the
CMake hook compiles patched shared sources without editing their tracked copies.
Patch 0011 documents cancellation's conservative progress count in shared
headers and the stream contract.

The guarded-copy diagnostic checks continuity and cache visibility. Measuring
the unguarded DMA ordering margin remains open. No microphone or headset is
attached; external capture, audible playback and Bluetooth I2S1 routing require
physical qualification. The generic capture and paced/recorded input services
remain independent of the external recognition application.

## Other audio paths

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
