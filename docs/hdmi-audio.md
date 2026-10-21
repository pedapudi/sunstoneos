# Finite HDMI PCM playback

The opt-in HDMI audio image sends one immutable caller file of pulse-code modulation (PCM)
samples through DMA0 and the internal I2S3 serial audio controller to the HDMI audio sampler. It accepts interleaved signed
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
The actual calculated HDMI link (TMDS) rate is 74.25 MHz for raw 24 MHz DCXO or 73.9375 MHz
for raw 26 MHz DCXO. The sampler programs N=6144 and a hardware-generated CTS clock-recovery counter,
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
including identity translations, Normal non-cacheable attributes and descriptor
write access on every 4096-byte page. It accepts outer or inner shareability,
matching the uncached pool contract: the address-translation result reports
Normal non-cacheable memory as outer shareable whatever the descriptor holds. Its cache callback accepts only CLEAN for those immutable
ranges and uses DSB ordering; it never broadens the shared policy or accepts
invalidation, unknown ranges or cacheable aliases. The allocation becomes
DEVICE-owned before the DMA worker is created. Protected ranges exclude the remaining firmware, source
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

Playback enables I2S3 and its FIFO requests with transmission off, then sets
the transmit enable once the FIFO holds samples, as the I2S0 stream driver does.
While the DMA job runs and while the FIFO drains, each output-task poll reads
the I2S interrupt status and FIFO status together. A TX overrun, or a TX
underrun seen while the FIFO again holds samples, retains the failure. An
underrun alone is expected once the FIFO empties after the final transfer,
because transmission stays enabled until the stop.

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

## Selected native software evidence

The final audio image is SHA-256
`432b7c104a2613b4870ca9e14c0753118a60474f099a6cfb564acb62c1094882`,
with build ID
`9acc54030f35e43da614065ce126f09dcfe967fa8312c0da054fe007eba4e650`.
Its exact compiled source, headers, tools and primary references are preserved
in 618 input snapshots under
`~/.cache/sunstoneos/evidence/hdmi-audio-nc-20261020/compiled-inputs.json`.
The host supplied one sealed model session and the coordinator supplied its
exact armed contract. No physical admission records were manufactured for a
board run; these inputs are explicitly MODEL evidence.

`checks/hdmi-audio-native.py` executed the linked admission and constructor,
then resumed the three actual PMIC, HDMI-output and DMA task bodies. An immutable
2049-frame caller file crossed three source pages and ended with a partial DMA
burst. The actual native path completed its FIFO job, observed DMA completion
and drain, observed 128 free I2S FIFO slots, published the 160-byte result and
ready word, and reached normal retained HDMI stop. The result reports 8196
source bytes consumed, calculated TMDS 73,937,500 Hz, 48 kHz PCM and unknown wire
drain. Its PCM and session hashes match the supplied inputs.

The positive execution is saved in `native-run/`. Its runner source hash is
`fc9c0ca68687099021c0b6e525f4a9ab54e94eab66995bb3debad6b7d61370a7`.
A later runner fixed a Python byte-array conversion and terminal-state
bookkeeping, restored that verified terminal checkpoint, and executed only two
previously unexecuted failure continuations. `native-faults-final/` records a
sink without the required LPCM capability and interruption after DMA submission
but before FIFO requests. Both publish retained failure without claiming source
consumption or drain, performing later device writes, or releasing the allocation.
The interrupted case retains the enabled DMA channel and its held buffers.
No preparation or successful playback interval was repeated for these cases.

The model implements architectural services, queues, electrical readbacks and
lower device effects. Scheduling is serialized and cooperative, with actual task
contexts and selected SVC #105 yield sites, plus tick-relative delays. It does
not qualify SMP scheduling, preemption, priority inheritance, real interrupt
latency, MMIO stall recovery, electrical timing, transmitted packets or sound.
The [host evidence](hdmi-audio-preparation.md) separately covers sealed packaging,
offline decoding and bounded launcher cases for that original profile.

Earlier inputs and failures remain under `hdmi-audio-20261020`. The initial
provider run correctly refused an incorrectly modeled PL1 drive field. The
first integrated image later reached active video and sampler preparation but
refused non-cacheable RAM through the shared write-back-only DMA policy. The
reviewed audio-specific NC adapter required one affected successor build; no
shared DMA policy or CPU mapping was changed. The inherited-DMA-interrupt guard
cases were reused because generated boot source, object code and all 1340
linked bytes of `a733_smp_boot` are identical between those images. No old image
or result was relabeled as the successor.

The opt-in `SUNSTONE_HDMI_AUDIO_USB` profile adds copied observation of this
finite operation. Its version-2 session is 816 bytes; the original 656-byte
version-1 session remains an offline-only profile. The appended USB authority
binds a distinct owner, four exact supply voltages, shared fabric settings and
three physical-authority records. USB uses ordinary cold-reference mode 1,
with both USB controllers initially gated/reset, excluded remote PHY/reference
consumers and quiet owned interrupts 188–190. These prerequisites supplement
the existing HDMI, audio, DMA-security and supervised-entry requirements.

The output task copies its published 160-byte result into one 512-byte reply.
Each request binds the session epoch and generation; each reply also carries
the lease, build identity and full session hash. USB retains that copied slot
through actual data-transfer completion and the terminal zero-length packet.
A fully delivered reply permits another copied snapshot. There is no host ACK
or resource release. The host permits at most
200 sequential requests within 19 seconds of the original 20-second lifetime.
A partial or late transfer retains the slot and resources. Provider failure
can park USB before a fault reply is available; retained failure does not imply
that the host can retrieve it.

Evidence under `hdmi-audio-usb-20261020/native-cases/` covers the final image
`0c661c29d33fc1e17001d4c7c052ad3bc6e9b4e4c893a7fb76119b57845b4546`.
Seven affected cases execute the new constructor, shared USB preparation,
result publisher, mailbox and linked TinyUSB framing, including full-speed and
high-speed terminal packets, request ownership refusals and late completion.
The prior accepted finite-audio disposition is explicitly seeded into the new
constructor-owned result; PCM DMA and the sampler interval are not repeated.
The new publisher binds the actual session and unchanged PCM hashes. The model
supplies RTOS scheduling, architectural services, PMIC/readback effects and DCD
transfer completion. It establishes neither physical USB operation nor sound,
serializer drain or DMA visibility. The original finite-operation evidence
retains its original scope. Root's four new pre-GIC guard cases and the separate
[host checks](hdmi-audio-preparation.md) cover their respective boundaries.
