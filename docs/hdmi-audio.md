# Finite HDMI PCM playback

The opt-in HDMI audio image sends one immutable caller file through DMA0 and the
internal I2S3 route to the HDMI audio sampler. It accepts interleaved signed
16-bit little-endian stereo at 48 kHz, from one to 48,000 frames. The image uses
the existing limited-range 720p TV0 pattern while audio runs. It has no streaming
interface, reconnect, replay or resource-release path. Software execution over a
hardware model does not establish transmitted audio or rendered sound.

Build `apps/hdmi-audio` with the existing A733 toolchain and select the
`sunstone-a733-hdmi` target. This enables `SUNSTONE_HDMI_AUDIO`; the default HDMI
image remains video-only. Audio and the retained framebuffer option cannot be
combined in this image. The [host preparation guide](hdmi-audio-preparation.md)
describes the immutable file, typed admission records and offline result decoder.
There is no executable live capture transport. Never use FEL memory reads
against running native firmware.

## Source route and clocks

The A733 manual identifies I2S3 as an internal HDMI/eDP controller at
`0x02535000`. The pinned Orange Pi device tree binds it to the HDMI codec with
TX hub enabled, two 32-bit wire slots, CPU-generated BCLK/LRCK and no MCLK.
Its TX FIFO is at `0x02535020`, DMA0 request 6, with 128 entries. This route uses
no external PB audio pins. The implementation adapts the effective Linux I2S
and DesignWare HDMI setup listed in the [source manifest](hdmi-audio-sources.json).
Linux is a source reference; the code runs under FreeRTOS on A733.

The application adopts the existing normalized-reference AUDIO1 profile. It
requires the enabled, locked 614.4 MHz DIV5 output, with modulation and automatic
gating disabled according to the existing audio provider. Shared OSC, REF,
AUDIO1 and DIV5 leases prevent retuning; an exclusive I2S3 lease covers its local
clock, reset, FIFO and route. CCU source 2 with divisor 25 yields 24.576 MHz.
The existing integer format planner divides this by eight for 3.072 MHz BCLK,
and the two 32-bit slots yield 48 kHz LRCK. An incompatible inherited audio
parent is refused; this application does not retune it.

HDMI retains its existing native VO1, rail, PMIC and resistor-word authority.
The actual calculated TMDS rate is 74.25 MHz for raw 24 MHz DCXO or 73.9375 MHz
for raw 26 MHz DCXO. The sampler programs N=6144 and hardware-generated CTS,
following the source's automatic-CTS path. It does not substitute a nominal
74.25 MHz CTS table for the raw 26 MHz clock. These configured rates do not
measure clock error, qualify the non-nominal video timing or establish audio
drift. Raw 19.2 MHz remains refused by the HDMI provider.

The already-read EDID must advertise stereo LPCM at 48 kHz and 16 bits through
a CTA audio descriptor or the CTA Basic Audio guarantee. This describes the
sink's advertised capability, not a physical playback observation.

## Finite memory and task ownership

The 656-byte authenticated session appends a 216-byte audio suffix at offset
440. It binds the PCM SHA-256, memory and electrical records, authority,
frame count and the exact 104-byte DMA handoff. PCM occupies its separate fixed
upload window at `0x41f10000`, with exactly four bytes per frame. The admission
contract hashes the complete session, and the application verifies the PCM hash
before copying it.

The existing non-cacheable reservation at `0x41c00000..0x41e00000` supplies one
permanent allocation: a 4096-byte descriptor page and a source span rounded to
4096 bytes. Actual native translation checks validate the whole pool and exact
registered DMA windows. The shared DMA RAM policy remains restricted to write-back
RAM. This application validates its two exact non-cacheable ranges separately,
including identity translations, inner shareability and descriptor write access
on every 4096-byte page. Its cache callback accepts only CLEAN for those immutable
ranges and uses DSB ordering; it never broadens the shared policy or accepts
invalidation, unknown ranges or cacheable aliases. The allocation becomes DEVICE-owned before the DMA
worker is created. Protected ranges exclude the remaining firmware, source
upload, session and CPU tables. No CPU edits or remapping occur after handoff;
all memory remains reserved even after successful completion.

The HDMI output task prepares the existing supervised DMA platform provider,
activates the actual DMA service and submits one immutable FIFO job on channel
0. The DMA worker alone performs controller operations and validates its task,
platform readiness and unchanged allocation on every callback. The output task
alone operates I2S3 and the HDMI sampler through their actual native bindings.
Both use the existing shared register mutex and monotonic counter provider.
Inherited DMA interrupt delivery must be disabled, not pending and not active
before the common boot code runs. This polled path enables no DMA interrupt.

The authenticated DMA handoff requires quiet prior producers, drained earlier
transactions, owned channel routing and retained manual recovery. A record ID,
software timeout or successful constructor cannot establish those physical
facts. The accepted shared DMA clock correction admits normalized SYS24M fabric
parents on raw 24/26 MHz boards while retaining REF ownership; it does not infer
fabric rates from raw DCXO alone.

## Completion and retained failure

A successful run observes the actual DMA service's COMPLETE result after its
controller stop/drain protocol, then observes all 128 I2S TX FIFO slots free.
It mutes the HDMI sampler and disables I2S requests/transmission. Stopping at
that point may truncate a sample still inside the serializer. The result
therefore reports finite PCM source bytes consumed and FIFO empty, while
`wire_drained` remains zero. It does not claim complete HDMI packet delivery,
sink rendering, sound or acoustic drain.

The aligned 160-byte retained result contains twelve 64-bit fields, the PCM
SHA-256 and the complete immutable session SHA-256 at offset 128. The producer
publishes it with the existing coherence helper before releasing the separate
aligned ready word. The session digest binds image identity, epoch, generation
and admission records, including when the same PCM file is used again. A ready
word is only a diagnostic publication; it does not grant physical authority.

Early interruption, hotplug loss, failed authority, expired deadline, a DMA
failure or uncertain I/O retains the whole session. No timeout fabricates a
COMPLETE result or permits descriptor, PCM, clock or rail reuse. Further device
writes stop after uncertainty. Buffers and hardware reservations remain owned
until supervised whole-board power removal. The firmware provides no automatic
reset or retry after that boundary.
