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

## Physical execution is paused

Port 3-3 is paused by the board queue after a no-return recovery failure.
Audio control entry `1793515513009252574-95a74f53` is queued and has not launched.
Its image SHA-256 is
`9492e8302500c75cdb344c4105cd63270ce03af2e5330f7b40248fc30d2e0215`.
The image, input and expected output are retained by the queue. No loopback or
overrun run is submitted. The coordinator must resolve the paused port before
physical work continues.

After successful control, the same image's loopback mode tests guarded-copy
continuity and cache visibility across three close/reopen cycles. This mode
does not measure the unguarded DMA ordering margin. That measurement, physical
retirement, interrupt delivery and broader clock transitions remain open.
No microphone or headset is attached. External capture, audible output and
Bluetooth I2S1 routing remain separate requirements.
