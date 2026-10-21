# Native A733 JPEG encoding

The FreeRTOS encoder application implements one retained VGA job: caller-supplied
640×480 NV12 pixels, stride 640, quality 75, and copied baseline JPEG output of at
most 1 MiB. Its linked native path and focused host models pass. Physical encoding,
JPEG numerical correctness, device burst confinement and memory visibility
remain unqualified. Other dimensions, formats, qualities, repeated capture and
broader H.264/H.265 encoding remain required hardware scope. A retained VGA
IDR/P mode is implemented in the [native H.264 encoder](h264-encode-native.md).

## Encoder authority comes from the encoder resource

The encoder uses MMIO 0x01c10000, TOP+0x800, ENCPP+0xa00 and JPEG+0xb00,
IOMMU master 3, IRQ 80, NSI 9 and PCK domain 3. Its VE0/VE1 clock mux order differs
from the decoder. The provider owns the exclusive VE_ENC lease and stable
parent, fabric, rail and OSC/PCK backbone leases. IOMMU preparation separately
excludes SYS/VI/DE/VE_DEC writers without reacquiring VE_ENC.

The first mode adopts already powered hardware. Before any encoder MMIO, it
checks PCK3 command/status low nibbles 8, module and shared gates, parent rate,
external reset deassertion and masked, inactive IRQ 80. Authenticated physical
admission must establish prior transactions/walks drained, all masters excluded,
skipped masters physically quiescent and no remote writers. Idle register
readback cannot create that authority. The supported compound identity is
0x0003331000021320; its board observation remains outstanding.

The reviewed encoder branch pulses TOP+4 bit 24 high, rereads, then clears it.
It selects TOP bit 6, rereads, then selects bit 7. This branch does not cycle
external reset. The effective library's creator sets decoder 0/encoder 1; both
flags govern TOP routing. A zero-filled context does not select encoder TOP.

Register authority is the pinned A733 package in
[the dependency record](../apps/jpeg-encode/sources.json), not guessed decoder
constants. Relevant anchors are libvencoder VideoEncCreate 0x2a5c–0x2a64 and libVE
VeInitialize 0x53d4–0x5428, VeReset 0x7854–0x786c and
enableEncoder 0x6c64–0x6c7c. OrangePi cedar_ve.c:507–535 describes the separate
power/external-reset/clock path. PCK600 domain offsets and domain 3 values are in
pck600_domains.c:87–118,547–569. The cached disassemblies and original notices
remain pinned outside the repository. The opaque library is used only in
isolated host instruction tracing and is not linked or loaded natively.

## One sealed input and one copied output remain owned

The encoder session is 856 bytes with the ordinary 592-byte board prefix. It binds
the executable, target/generation, resource owners, four physical evidence
digests and a nonzero SHA-256 of exactly 460,800 caller bytes. The reference field
must be zero; normal encoding does not require a numerical oracle. The private
boot preserves and publishes the full session before provider construction.
The protected contract remains 224 bytes and its synchronous board view does not
escape validation. Startup otherwise retains the existing cached multicore
path and its physical prerequisites.

The USB upload header binds length, epoch and generation. Private WB RAM receives
bounded fragments; complete hashing precedes publication and all encoder access.
Truncation cannot publish provider access. Duplicate commits, writes after
commit and changed session identities refuse. The application never starts a
second job, including after copied-output ACK.

The actual compact allocation uses 1,560,576 NC bytes for 32 KiB tables,
page-rounded input/output and two guard pages around each buffer. WB upload and
copied JPEG use 1,511,424 bytes. Actual application metadata, including its 32 KiB
task stack, occupies 36,192 bytes within the 64 KiB budget. The allocator validates
exact capacities/alignment before exposing spans. Master 3 maps input read-only
and output read/write, with no external device scratch observed in the traced
profile. These minimum extents do not establish physical DMA burst bounds.

The CPU supplies the 623-byte JPEG prefix and 128 reciprocal quantization words.
Hardware supplies entropy data. The controller clears observed stale status,
requires clean low status bits before triggering, then accepts only a fresh
low-nibble finish value 1. Stall/error bits fail. Absolute bit count must include
nonempty, byte-aligned entropy and leave two bytes for EOI without wrap.
This is a stricter completion gate than the opaque wait implementation.

The application checks status, count, guards and fresh IOMMU scans around each
copy chunk of at most 32 KiB. It appends EOI to private copied RAM and hashes the
complete JPEG before publication. The result header binds actual length,
format, dimensions, quality, input hash, copied hash and session identity.
The host fsyncs fragments, JPEG and result record before generation/digest ACK.
TinyUSB may emit intermediate zero-length packets; capture retains them and
continues under the existing deadline. Transfer completion gates ACK processing.
All buffers, mappings, leases and uncertain effects remain retained through
success, fault, deadline and ACK until whole-board power removal. No codec
status, copy or ACK establishes fabric drain or permission to reclaim memory.

## Evidence separates modeled bytes from hardware qualification

The preserved native candidate is
`~/.cache/sunstoneos/evidence/jpeg-encode-native-20261020/build/sunstone-a733-jpeg-encode.elf`,
SHA-256 `71eb42dc31533beccc53f2c73a3e182082fd91a4c650a58e1df288a7359a945c`.
Its compiled inventory is `compiled/image-input-manifest.json`: 169 linked
objects and 447 compiler inputs. The root static startup/layout review is
`root/layout-review.json`; no CPU startup or hardware execution was repeated.

Focused checks cover actual allocation, authenticated upload, real lower native
MMIO policy, encoder programming, IOMMU accesses restricted to master 3, copied output,
completion/count failures, later-copy fault retention, launcher admission,
supervisor entry and durable host capture. Device completion and entropy are
explicit model inputs. A placeholder entropy payload cannot establish JPEG
arithmetic; physical qualification needs independently decoded captured JPEG
pixels plus guard/fault, visibility and bounded completion observations.

The reference trace and source review are preserved in
`~/.cache/sunstoneos/evidence/jpeg-encode-trace-20261020`.
The native evidence retains fixture repairs and their limits. An early USB
fixture waited indefinitely while deliberately withholding transfer completion;
the bounded continuation reuses the completed checkpoint. Accepted earlier
cases and the native image remain unchanged. An inherited launcher report
summary mentioned historical VGA cases; its clarification identifies the actual
encoder version 1 and invalid version 2 cases without replay.
