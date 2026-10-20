# Native A733 HEVC decode proposal

HEVC decoding remains incomplete. Partial parser, input, storage, controller and
provider-bridge source resides in the separate
`/home/sunil/git/sunstoneos-worktrees/hevc-decode` worktree at commit `69c4eac`.
It is not integrated into bringup's shared providers, has not completed review
and has no native build. Application tasks, USB integration, allocation/table
proof and hardware qualification remain unfinished.

Bringup contains the typed session/startup/CMake foundations and source-only host
preparation, producer, observer and coordinator. Runtime preparation explicitly
refuses until an actual native allocation/page-table proof is available. The
recorded C session-validator evidence covers two positive and eleven invalid
range cases with UBSan only, using synthetic identities and no providers or
pixels (`hevc-decode-20261020/root-session/result.json`). Host fixtures, launcher
checks and native execution have not run. The source-only host audit verifies
24 frozen files under `hevc-host-source-20261020/root-integration.json`; it does
not establish runtime behavior. There is no native HEVC image.

The A733 manual advertises Main/Main10, level 5.2, up to 8192×4352 at 24 fps.
Published source and isolated instruction traces establish several A733 routing,
register and buffer-layout details below. The completed traces stop before picture
decoding. They establish neither numerical decode nor physical operation.

## Reusable source and actual A733 routing

[The pinned source record](../apps/hevc/sources.json) identifies Linux Cedrus,
GStreamer's standalone HEVC parser, older CedarC source and the effective A733
runtime package. Cedrus control code is GPL-2.0-or-later; the GStreamer parser is
LGPL version 2-or-later, and the CedarC reference source is LGPL version 2. Original notices and
license records must remain. The opaque library is an offline instruction
reference; the FreeRTOS image will not link or load it.

Linux revision 70dd4ee72eb2d5793c62e71c14e3aba78e5a4e56 supplies
`cedrus_h265.c` and register definitions. GStreamer revision
d31ce8e5e1aacf8f5e5beabb5c81ce2e4da5c202 supplies VPS/SPS/PPS/slice parsing.
The existing H.264 integer/bitreader adaptation provides the standalone port;
no GStreamer framework or Linux runtime is needed. CedarC revision
d92ffcb082595f0fffda45ea481cec40fe5a3a8d supplies corroborating register,
allocation and reference semantics, subject to effective A733 comparison.

The effective package is the existing 953940-byte CedarC package with SHA-256
ea2c62123466e233e5f358fffac10499a5ae3886d9fe340532907bba6f2f6d4e.
Its `libawh265.so` has SHA-256
6adc1bba9ca552e17f6bc8a5d4b28b6cba1a6268feebfded0fa99ed199457df1.
The library's HevcDecInitContext at 0x31a4–0x31e8 requests register groups 5 and 0.
The actual libVE map 3 table at 0x9130 routes group 5 to decoder+0x500 and decoder
TOP to +0. VeGetGroupRegAddr 0x31e0 and its map function 0x2f10 select that table for
creator decoder 1/encoder 0. The encoder branch that returns +0x100 is a separate
routing path. These facts establish the window; they do not establish every
Cedrus control bit for A733.

The application should reuse VE_DEC, PCK 2, IRQ 81, decoder MMIO 0x01c0e000 and the
existing dual-master 6/7 IOMMU admission. No evidence yet permits narrowing that
master mask. Decoder authority does not authorize the overlapping encoder window.

## Initial complete path and ownership

The proposed first mode accepts one immutable AnnexB clip containing a VPS, SPS,
PPS, one IDR picture and one following P picture. It supports progressive Main,
8-bit 4:2:0, 640×480, one layer, CTB64, one short-term reference and one independent
slice per picture. CABAC, ordinary prediction/transform content, SAO and deblocking
remain real codec work. Tiles, wavefront parallel processing, dependent slices,
B pictures, long-term references, custom scaling matrices, 10-bit output and
compressed framebuffer output are initially refused. Broader HEVC modes remain
required hardware scope.

Both pictures' syntax, geometry, reference relationships and input ranges must
validate before the first device write. Each picture has at most 256 KiB of encoded
input. Whole-clip digest and immutable per-picture descriptors bind the supplied
bytes. The application decodes the IDR, copies visible NV12 pixels into owned WB
RAM, publishes indexed output and waits for ACK0. It then decodes the P picture
using the retained IDR reference and a separate destination. ACK does not retire
a reference, refill input or release storage. A later failure preserves the first
copied picture and retains every device allocation and provider lease.

The native controller requires finish without error/data request/timeout,
source-defined CTB completion and source-defined idle conditions before copied
output. Effective HevcVeStatusInfo 0x1a4e4–0x1a534 reads status+0x38 and CTB count
+0x3c, handles fault bits and acknowledges observed status. The unbounded library
wait at 0x1a480 is unsuitable for native use; the existing admitted deadline must
bound every wait. Neither that wait nor copied output establishes fabric drain.
All allocations remain retained through board power removal.

## Provisional fixed memory and interfaces

The existing extended memory profile is sufficient without shared CPU or memory
reservation changes. The proposed allocation budget is 3 MiB NC and 2 MiB WB, plus
at most 1 MiB of CPU parser/controller metadata within the existing application
BSS range. Actual linked allocation, metadata and stack bounds must be checked
before composed cases.

| Device allocation | Proposed retained capacity |
| --- | ---: |
| Two encoded-picture inputs |2×262,144 bytes |
| Two decoded NV12 destinations |2×462,848 bytes |
| Two motion-vector buffers |2×16,384 bytes |
| Neighbor metadata |815,104 bytes |
| Entry-point buffer |4,096 bytes |
| Eight pairs of outer guard pages |65,536 bytes |
| IOMMU tables |32,768 bytes |
| Total NC allocation |2,400,256 bytes |
| WB upload and two visible copied pictures |1,449,984 bytes |

The effective framebuffer allocator requests 460,800 bytes for NV12, rounded
to 462,848 bytes per native retained extent. Its clean pre-overflow records have
640-byte stride, 640×480 luma and chroma at offset 307,200. Both 10-bit and AFBC
flags are zero. Explicit frame SRAM addresses must separate those planes. The source-derived motion-vector requirement is 160 bytes per CTB plus
1024 alignment bytes: 80 CTBs require 13,824 bytes, rounded to 16,384. The actual
A733 instruction sequence 0x9e0c–0x9e18 computes that formula. Init 0x3224–0x3230
allocates 0xc6800 neighbor bytes, matching the older source's 813,056 bytes;
0x3250 allocates 4096 entry-point bytes. Eight IOMMU spans cover the two inputs,
two destinations, two motion-vector buffers, neighbor metadata and entry points.
These are planning bounds, not an executed allocator proof.

Proposed new leaves are `a733_video_hevc` parser/planner/controller interfaces,
a distinct typed decoder-provider bridge, `apps/hevc` input/copy/USB and host
preparation/capture. The bridge should reuse the actual decoder coordinator,
lock and VE_DEC lease; JPEG/H.264 structures and callbacks retain their types.
The root owner approved that narrow typed bridge. Generic IOMMU policy changes
are unnecessary.

The integrated 856-byte private-session foundation keeps the 592-byte board prefix. Its extension
contains 48 bytes of identity/owners, 128 bytes of evidence digests, a 32-byte
stream digest, two 16-byte input descriptors and 24 bytes of clock/rail fields.
An immutable compiled marker binds the fixed dimensions and capacities.
The integrated 288-byte protected contract uses the existing extended-memory
pattern. `include/a733_hevc_application.h` defines these foundations; they do not
establish a native allocator or complete application. Source-only host framing
defines indexed copied NV12 with validated geometry, strides, generation and digest. Independent reference pixels are an
optional qualification input to the host, separate from normal decoder output.

## Required source comparison before native programming

The next prerequisite is a bounded isolated instruction trace using contexts
constructed through actual initialization operations. It must resolve:

- A733-specific SPS/PPS/slice field layouts, output-format/stride controls and
  frame SRAM/reference-list ordering against Cedrus, including bitstream
  offsets and emulation-prevention handling.
- Linear NV12 output with scaling, AFBC and LBC disabled, including every extra
  A733 register write or allocation. A default from the older chip is insufficient.
- Trigger, status, CTB count and all relevant busy fields for one completed
  IDR/P sequence. Publication may require more than the interrupt finish bit.
- Actual reconstruction/reference plane addresses, motion-vector extents and
  initial table contents for both pictures.

A focused source review confirms the SRAM frame records at 0x1b858–0x1b958
contain progressive POC twice, motion-vector address twice, and Y/C addresses.
Each record occupies 0x20 bytes at SRAM 0x400; addresses are shifted by eight.
The source review also identifies effective TOP+0xe8 bit 30 and identity-dependent
CTRL bit 31 differences. GStreamer consumes slice byte-alignment bits before
reporting header size, while Cedrus positions hardware before that padding. The
adaptation must preserve the earlier bit position, validate padding and reconcile
emulation-prevention bytes. Copying the reported header size directly is unsafe.

Unresolved fields must remain explicit blockers to native register code. Proven
parser/source-port and private input/output preparation can proceed independently.
After the source bridge is concrete, focused review should cover bitstream bounds,
reference lifetime, speculative state commit, lowest native MMIO policy and copied
output publication. Selected checks should use actual producer-built clips and an
independent pixel/syntax reference, preserve successful checkpoints and avoid CPU
startup replay or performance measurement.

## Effective package metadata mismatch limits the picture trace

All seven runtime libraries match the same pinned published package. Its public
320-byte `VideoPicture` layout agrees with the effective framebuffer accesses,
including the metadata pointer at offset 192. The effective framebuffer allocator
requests 64 bytes for that metadata at `libfbm.so` 0x56e8–0x5754. HEVC clears and
copies 24,856 bytes through the same pointer at `libawh265.so` 0x9724–0x974c.
The libraries therefore disagree about this metadata allocation.

The isolated model now rejects that write before mutation. Its preserved
`stream-bounds/refused.pkl.gz` snapshot and `pre-overflow-framebuffer.json` record
frame allocation and format before the mismatch. An earlier trace reached the
first HEVC trigger after crossing the allocation boundary; that trace is
provisional and provides no later output-layout evidence. Both records remain in
`~/.cache/sunstoneos/evidence/hevc-decode-20261020/`.

The native two-picture interface does not expose HDR or AFBC display metadata.
A reviewed reference-model adjustment provided 24,856 bytes of backing only for
the exact 64-byte metadata request. All other allocation bounds remained strict.
The resulting trace, `stream-adapted-registers/result.json`, stops before the first
decode trigger. It preserves the original failure and does not establish that
the published library combination executes correctly without modification.

An automated content-safety check stopped the analysis worker. A subsequent
attempt to continue open-source native integration was also stopped. No further
reference execution was attempted. Partial source and existing evidence are
preserved on the local HEVC development branch. A complete IDR/P implementation,
review, native build and selected validation remain outstanding.

The final source review identified two outstanding corrections: retain the
allocator's physical addresses in the page tables, or explicitly require identity
mapping; and validate emulation-prevention bytes throughout each encoded unit.
The partial controller already contains the requested integer-overflow, stable
acquisition-token and copied-result lifetime corrections. None of these partial
native paths has completed execution checks.
