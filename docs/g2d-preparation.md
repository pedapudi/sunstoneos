# Prepare and capture one retained G2D operation

The native application performs one caller-selected fill, source copy,
solid-background blend, exact twofold enlargement, general scaling or
image-over-image blend. Copy and general scaling may convert between 32-bit RGB
channel orders. It exports an immutable
CPU-owned copy. The host path uses the existing
supervised board launcher, typed admission records and coordinator claim. All
native DMA buffers, page tables and provider leases remain allocated after
completion, ACK or failure. Recovery follows the admitted whole-board
power-removal plan.

Pixels use ARGB8888 stored as little-endian B, G, R, A bytes unless a copy or
general scaling names another 32-bit order. Width is one through
1280 pixels; height is one through 720. The byte stride is a multiple of four,
at least four times width and at most 5120. The complete source covers
`stride * height` bytes, including padding, with a maximum of 3,686,400 bytes.
Source bytes are supplied for every operation and authenticated by SHA-256.

Output starts at zero, including row padding. Fill writes the selected color
inside the destination rectangle. Its source coordinates must be zero. Copy
moves the selected rectangle from the separate source surface without alpha
blending; its color field must be zero. Both rectangles must fit the declared
surface. Scaling requires a positive even destination width and height. The
source window has half those dimensions at the supplied source origin, and its
color field must be zero. A 64×64 initial fixture does not restrict the public
geometry.

## Seal the operation

`apps/g2d/producer.py` extends an existing 592-byte supervised board template to
a 904-byte G2D session. It binds the source digest, twelve operation/geometry
words, eight operation-extension words, owner, authority, clock client and four
reviewed physical evidence hashes. The existing board prefix binds epoch,
generation, lease and native build identity. The extension words are the
general-scaling source size, the image-over-image background origin and plane
alpha, the output format and two reserved words; each word an operation does not
use is zero. No reference pixels enter the native protocol.

New images require the initial evidence record's `de_sys_power_profile` to equal
`off-or-on-retained-v1`, with `de_sys_consumers_excluded` and
`de_sys_prior_transactions_drained` both true. These facts cover every consumer
of DE_SYS, including DE, deinterlace, G2D and EINK, in addition to the existing
all-master, walk, interrupt and remote-writer exclusion. The native provider
independently refuses incoherent command/status or reserved initial values.
No idle register read establishes the admission facts. Older inherited-ON
evidence alone cannot authorize this profile. The preparer checks the linked
image's separate power-profile marker; the 904-byte session layout is unchanged.

For example, a fill template can be produced from exact source bytes and the
reviewed evidence files:

```sh
python3 -B apps/g2d/producer.py \
  --board /absolute/board-template.bin --input /absolute/source.argb \
  --owner 101 --authority 102 --client 3 \
  --initial /absolute/initial.json --memory /absolute/memory.json \
  --electrical /absolute/electrical.json --supervision /absolute/supervision.json \
  --operation fill --width 640 --height 480 --stride 2560 \
  --dx 32 --dy 48 --w 128 --h 96 --color 0xff3366cc \
  --output /absolute/g2d-template.bin
```

The example owner/client values illustrate argument syntax; actual values must
match the admitted provider ownership. A copy uses `--operation copy`, zero
color and the selected `--sx`/`--sy` source coordinates.

## Prepare the exact image and authority

`apps/g2d/prepare.py` consumes the actual frozen native ELF and image manifest,
904-byte template, reviewed admission record and exact source file. It derives
fresh epoch/generation/lease values, preserves the native build identity and
creates an ordinary `a733-g2d-supervised-bundle-v1` directory. Runtime upload
contains a 32-byte typed header followed by the exact source body.

The preparer checks the ELF's immutable eleven-word geometry, executable entry,
protected session region and extended memory reservations. The authenticated
288-byte boot contract binds the complete session digest, board entry digest,
and WB `0x48000000..0x78000000` / NC `0x78000000..0x79000000` memory layout.
The SRAM launcher and native target both use the 20-second board lifetime.
Prepare the launcher once and preserve that exact artifact for its instruction
checks; do not rebuild it between host cases.

Physical memory admission requires the native provider's actual allocation and
page-table proof, including guarded source/output/RCQ extents and the private
table allocation. The admission record references `allocation` JSON and the
32,768-byte `page_table` artifact by absolute path and SHA-256. The allocation
record binds the image and template hashes, exact provider rows and three mapped
spans. The preparer checks alignment, pool bounds, aliases, guard extents, access
permissions and every descriptor, including unmapped entries.

Operational admission requires `physical_authority: true` and `modeled: false`.
The focused preparation checker substitutes only this authority predicate for
explicitly labeled `MODEL-NOT-PHYSICAL` records. It checks that production
preparation refuses those records before exercising the actual layout validator
and preparing the actual image. Recorded software models do not authorize
physical execution.

## Capture under the existing coordinator claim

`tools/check-a733-g2d-supervised.py` accepts the prepared bundle, pinned FEL
executable, existing claim/owner and a fresh evidence destination. Its prerequisites
are `supervised-ram-recovery` and `g2d-supervised-software`; it reads the existing
claim and does not create queue work. The target USB identity is VID `1f3a`, PID
`efe8`, revision `0100`, product `sunstoneos native G2D`, serial
`sunstoneos-a733-g2d`.

A single absolute 19-second host window covers enumeration, source upload,
operation, copied output and final ACK observation. Pending replies permit another
bounded query. Unexpected empty, malformed or failed transfers stop immediately; capture
never retries transport, relaunches the image or releases uncertain native memory.

The 192-byte result header binds the complete sealed session SHA-256 in addition
to epoch/generation, source/output digests and exact operation geometry. The full
session digest binds the original build identity and lease. Copied success also
requires native completion, RCQ task-end, the exact master-10 scan and guard
checks. The RCQ frame counter is diagnostic; the host makes no cumulative-counter
assumption.

The native sender bridges the header/payload boundary in 512-byte chunks and
refills its FIFO from TX completion before TinyUSB decides whether to send a ZLP
(zero-length packet). The host uses the selected endpoint’s 64-byte FS or 512-byte
HS packet size and bounded packet-aligned reads, retaining payload received with
the header. Intermediate short packets or ZLPs are refused. A short read ending
at the complete declared extent has consumed the terminal short packet or ZLP.
If the final read fills its request and the response is a packet-size multiple,
one additional read must consume the single terminal ZLP before ACK. The native
delivery gate also waits for that terminal transfer; an empty read elsewhere is
a protocol failure.

The complete source upload is already frozen in the typed bundle. Capture saves
one durable upload intent and its digest, tracks accepted bytes and the current
write offset in memory, and saves that progress on completion or failure. It does
not rewrite the journal for every 512-byte source chunk. This preserves a bounded
request history without adding thousands of fsyncs to the session window.

The host preserves raw fragments, full responses and `output.argb` durably before
ACK. For fill and copy it compares every byte with an independent CPU reference,
including zero pixels outside the rectangle and zero padding. A mismatch retains
the actual output and stops before ACK. Blending and format-converting copies
retain their ideal reference comparison as an unqualified diagnostic;
authenticated output may differ from that ideal without blocking ACK. Scaling
has no pixel reference and requires zero bytes outside the destination. After the operation checks pass, an authenticated result receives one
41-byte ACK bound to generation and output digest. One final query must return
ACKed status with unchanged identity, digests and proofs. This observation ends
capture; every native resource remains retained. Evidence failures after an
observed ACK preserve that observation without claiming completed capture.

Host models and copied-byte comparisons establish their stated software behavior.
They do not establish physical G2D execution, DMA confinement, visibility,
interrupt delivery, recovery or performance.

## Focused software checks

`checks/g2d-preparation.py` consumes the frozen native evidence directory and raw
page-table artifact from the actual allocator case. It produces typed modeled
facts, checks allocation and descriptor refusals, and compiles the SRAM launcher
once. `checks/g2d-host.py` consumes that modeled bundle and checks fill/copy/source-over-solid bytes,
FS/HS framing, durable output before ACK, identity refusal and partial failures.
Both checkers record physical authority as false. The operational caller rejects
the modeled bundle.

The supervisor entry check uses
`checks/h264-supervisor-entry.py --caller tools/check-a733-g2d-supervised.py --case actual`.
It stops at readiness before queue or device access.

The preserved host evidence is
`~/.cache/sunstoneos/evidence/g2d-host-20261020/`. Modeled preparation passed
against native ELF SHA-256
`4e043dad7e691ad50c4164dd9085ae118a0e0911d7fd8aea93e16610e7966328`.
The checks passed thirteen capture cases, four geometry refusals, five
allocation/table refusals and the actual supervisor entry readiness boundary.
Production refused modeled authority. The single preserved SRAM launcher has
SHA-256 `1f0fa59aacced824403a7d0b380b59e47629ce3c19c805bfc7a43c28dd184f84`.
These results establish software behavior only; physical admission remains absent.

The DE_SYS-capable image is separately bound in
`g2d-power-20261020/host-authority/`. Seven focused checks cover the accepted
power profile, missing or differing profiles, absent whole-domain exclusion or
prior-drain facts, and missing or differing immutable image markers. The actual
producer reseals each altered record before validation. The check substitutes
unchanged board authority and stops before DDR/allocation validation; it claims
no complete physical admission or capture. Launcher generation, request format
and every launcher source/header match the preserved launcher above. Its binary
and object artifacts are reused without recompilation or instruction-case replay.


For opaque solid-background blending, pass `--operation source-over-solid` and
an opaque `--color`, such as `0xff315a93`. Source coordinates and destination
rectangle retain the copy bounds. Input pixels use straight per-pixel alpha;
output and padding outside the rectangle remain zero. This operation adds no
second upload or buffer mapping. It requires the launcher built with the changed
geometry header; the earlier fill/copy launcher rejects operation 3.

The host compares RGB against ideal nearest-integer `/255` source-over arithmetic
and expects opaque output alpha. Hardware rounding remains unqualified, and the
capture record states that limit. The arithmetic comparison is an unqualified diagnostic and does not reject an
authenticated output or prevent its ACK. Session, digest, completion and durable
output checks remain strict. Neither the comparison nor ACK authorizes another
operation or releases retained resources.


## Exact twofold enlargement

Use `--operation scale2x` with zero color. The destination rectangle may range
from 2×2 through 1280×720, with even width and height. Its source window ranges
from 1×1 through 640×360. The destination and half-sized source windows must
both fit the declared canvas at their separate origins. Canvas dimensions and
origins need not be even. Source upload, stride, surface size and the 904-byte
session and 192-byte result layouts retain their existing meanings. No separate
scaled input or extra device mapping is introduced.

For example, a 60×64 canvas with 256-byte stride can enlarge the 16×12 source
window at (40,42) into a 32×24 destination at (9,17). The complete upload still
contains 16384 bytes, including padding. These source coordinates fit the
half-sized window even though a 32×24 source window would exceed the canvas.
The CLI arguments for this geometry are:

```text
--operation scale2x --width 60 --height 64 --stride 256 \
--dx 9 --dy 17 --w 32 --h 24 --sx 40 --sy 42 --color 0
```

The host has no qualified interpolation oracle. Capture records
`matches_cpu_reference: null`, `reference_qualified: false` and an unchecked
reference comparison. It independently requires zero bytes outside the selected
destination rectangle, including row padding. A nonzero untouched byte causes
host failure before ACK, after the actual response and copied output have been
saved durably. Source authentication, complete-session and output digests,
completion, master-10 scans, guards and ACK receipt remain strict. An accepted
copy establishes no numerical interpolation or physical pixel qualification.

The scaling host checks passed on 20 October 2026 with the actual native image,
its exact sealed session and copied response and receipt. Five geometry
boundaries include the minimum and maximum extents, asymmetric origins and
source windows ending at the canvas edge. Twelve geometry refusals cover odd
or zero destination extents, source and destination overflow, and nonzero color.
Two source-authentication refusals confirm that an unchecked interpolation
reference still requires the complete authenticated upload.

One exact native copied result completed host capture with untouched bytes zero
and the interpolation comparison unchecked. One altered row-padding byte, with
a recomputed output digest, was retained and refused before ACK. The native
output pixels are explicit model data; no interpolation model or physical
arithmetic claim follows from their capture.

Preparation derives its allocator rows and full page table from the actual
saved native checkpoint for that exact session. It does not replace the proof's
session identity or repeat construction. Production admission refuses the
modeled records. Preparation compares all launcher source and header inputs and
copies the entry's existing binary, objects and original compilation journals.
There is one launcher compilation; the prepared session, request and contract
match the entry bytes. Root's three new scaling SRAM cases pass separately.
Earlier fill, copy, blend, completion, boot and transport evidence retains its
recorded scope without matrix replay.

Host sources and selected immutable artifacts are preserved at
`~/.cache/sunstoneos/evidence/g2d-scale-host-20261020/frozen/manifest.json`.
Native results are under `~/.cache/sunstoneos/evidence/g2d-scale-20261020/`.
The preserved models do not authorize physical execution or resource reuse.


## General scaling, image-over-image blending and format conversion

These operations need an image whose geometry predicate admits operations 5
and 6, and the launcher built from the same header. The earlier launchers refuse
them. Source upload, stride, surface size and the 192-byte result layout keep
their meanings; no second upload or mapping is added.

- **General scaling.** Use `--operation scale` with `--sw` and `--sh` for the
  source crop at `--sx`/`--sy`, and `--w`/`--h` for the destination. The crop
  must fit the canvas and differ in size from the destination; color is zero.
  For example, `--operation scale --width 60 --height 64 --stride 256 --sx 2
  --sy 1 --sw 55 --sh 60 --dx 3 --dy 4 --w 13 --h 9` reduces a 55×60 crop to
  13×9. Capture records no interpolation comparison and requires zero bytes
  outside the destination rectangle, including padding.
- **Image-over-image.** Use `--operation source-over-image` with the foreground
  at `--sx`/`--sy`, the background at `--bx`/`--by` and plane alpha `--alpha`
  1 to 255. Both rectangles have the destination size and must fit the canvas.
  The host compares output with ideal straight-alpha source-over as an
  unqualified diagnostic that does not block ACK.
- **Format conversion.** Add `--source-format` and `--output-format` to a copy or
  general scaling; each accepts ARGB8888, ABGR8888, RGBA8888, BGRA8888,
  XRGB8888, XBGR8888, RGBX8888 or BGRX8888. A converting copy is compared with
  an ideal channel reordering that treats X channels as 255. That comparison is
  an unqualified diagnostic; a plain copy remains an exact, ACK-gating check.

`checks/g2d-modes-host.py` exercises these rules with sessions resealed from an
accepted earlier session and synthetic response frames. The register, linked
image and launcher checks are described in [the component document](g2d.md).
