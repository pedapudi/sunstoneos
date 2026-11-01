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

## Native entry needs secondary CPU ownership

The audio-disabled control refuses entry before cache maintenance or audio
setup. Queue entry `1793516471802901284-83b8c6a0` records a watchdog reset and
software fault site `0x15`: CPU1 power policy at `0x07053000` reads `0x100`
(dynamic OFF). ESR contains that observed word and FAR contains its address;
these values describe a software check, not an architectural exception.
The image SHA-256 is
`ea2949db2010351abcc46268ce9768243d167db57d86a8f6d3d4796ea09eb2bf`.

The shared probe contract requires acknowledged static OFF, clear start
requests and disabled GIC wake on all secondary CPUs. The intentional watchdog
qualification probe only parks and does not establish those prerequisites.
The existing owner in `boot/a733/stage2/cpu.c` is for cold boot; it has no
returning-probe policy restoration. The shared startup owner must establish and
retain the required state through cache maintenance and restore it safely for
ROM return. The audio image does not change CPU power policy.

The shared diagnostic patch is
`~/.cache/sunstoneos/build/audio-1031/shared-patches/0005-probe-entry-fault-inputs.patch`.
It preserves the entry predicates and records each refusal's input. The CMake
hook accepts it through `SUNSTONE_I2S_PROBE_ENTRY_PATCH` without editing shared
source. Actual linked-entry checks and independent review pass under
`~/.cache/sunstoneos/build/audio-1031/entry/` and `entry-review/`.
The OS-only, hello-compute and external mimus baseline builds pass, including
their image link checks. The selected integration gate, scheduler check and
check index pass. `entry/validation.json` records the image hashes, source
revision, external application checkout and build logs.

The board queue is unavailable after the host reboot. No internal-loopback or
overrun run is submitted. Queue recovery and the shared CPU ownership path
both block physical audio execution.

After successful control, the same image's loopback mode tests guarded-copy
continuity and cache visibility across three close/reopen cycles. This mode
does not measure the unguarded DMA ordering margin. That measurement, physical
retirement, interrupt delivery and broader clock transitions remain open.
No microphone or headset is attached. External capture, audible output and
Bluetooth I2S1 routing remain separate requirements.
