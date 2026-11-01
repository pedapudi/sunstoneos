# Audio capture and playback

Status: implemented and host-checked. Physical audio remains unqualified.

The native diagnostic in `apps/i2s/probe.c` uses the existing FreeRTOS DMA
service, cyclic stream driver and cache adapter. I2S0 CTL bit 3 connects DOUT
to DIN internally, as specified in A733 manual section 17.1.8.1. The diagnostic
supports an audio-disabled control, guarded cyclic loopback and held-copy
overrun. It uses 16 kHz mono samples in 32-bit containers, eight 1,024-byte
periods and the existing 24.576 MHz module clock. CPU clocks and supply
voltages are unchanged.

Retirement returns exact client tickets, cancels and drains both directions,
reaps and closes each stream, and observes the global DMA MBUS FIFO empty
before reuse. Final cleanup masks the DMA interrupt, suspends its worker and
releases parent leases. The probe monitor restores the DMA routing, security,
clock and interrupt fields it changes before using the shared return contract.
The audio clock driver accepts the documented reset state with its LDO enabled
and PLL disabled, then waits for lock, settling and effective output gates.

Sanitizer, real FreeRTOS and linked-instruction checks pass under
`~/.cache/sunstoneos/build/audio-1031/return/check-1/`. They cover actual
constructors, busy drain, held-copy overrun, serializer-owned file EOF, stale
readbacks, delayed FIFO completion and failed restoration. The independent
review is in `return-review/final-review.md` under the same build root.

## Returning probe owns secondary CPU policies

The native probe acquires acknowledged static OFF before whole-hierarchy cache
maintenance and restores exact original policies after final cache cleanup and
receipt preparation. It admits matched static or dynamic OFF profiles, preserves
inherited GIC wake-disable and clear software requests, and writes only changed
secondary policy words. CPU0, cluster policy, wake controls, entry addresses,
clocks, resets and rails are unchanged.

The per-core CPU1 check writes zero then `0x100` only at `0x07053000` and verifies
byte-identical neighboring pages. A separate aggregate check acquires and restores
all seven secondaries. The returning path executes 18,944 data-cache operations
and one I-cache invalidation before dynamic policy release. Failed acquisition
or restoration retains the first failed input, keeps the watchdog armed and
prevents successful completion or normal return.

Fourteen CPU ownership cases pass under
`~/.cache/sunstoneos/build/audio-1031/policy/cpu-final/`; fourteen native entry,
restoration and constructor cases pass under `policy/native-final/`. The default
weak hooks remain inside memtest's protected recovery closure, with its link,
relocation, stack and layout checks passing. Three independent reviewers approve
the design, source and linked image; records are under `policy-review/`.
The OS-only, hello-compute and external mimus baseline builds, selected integration
gate, scheduler check and check index pass.

Shared patches `0006` through `0009` are under
`~/.cache/sunstoneos/build/audio-1031/shared-patches/` and pass `git apply --check`.
Patch 0006 replaces diagnostic patch 0005. The audio CMake hook builds patched
copies without editing shared tracked files. The source comparison and measurable
budgets are in `apps/i2s/CPU-OWNERSHIP.md`.

The reviewed image SHA-256 is
`1c822cdde6884b49e5249c58011629dfdb1419c5d71c3ee4ca8567a5355c1e43`.
Its ELF, three mode inputs, expectations, short README and exact submit commands
are under `~/.cache/sunstoneos/build/audio-1031/policy/`. The queue daemon is
stopped; no new run is submitted. Control must return normally before internal
loopback, and loopback must return normally before overrun. These transitions
and physical audio remain unqualified.

After successful control, the same image's loopback mode tests guarded-copy
continuity and cache visibility across three close/reopen cycles. This mode
does not measure the unguarded DMA ordering margin. That measurement, physical
retirement, interrupt delivery and broader clock transitions remain open.
No microphone or headset is attached. External capture, audible output and
Bluetooth I2S1 routing remain separate requirements.

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
