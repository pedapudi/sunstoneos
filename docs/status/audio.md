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
