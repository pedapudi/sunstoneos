# Native A733 HEVC decode

The native HEVC application decodes one uploaded two-picture clip on the A733
video engine (VE) decoder and returns copied NV12 pictures over USB0. It builds
as `sunstone-a733-hevc` from `apps/hevc`. Host models check its parser, storage,
controller, decoder binding and lowest native register policy. The image has not
run on a board, and host preparation still refuses to produce a launch bundle
because the physical allocation and page-table evidence it requires does not
exist yet. There is no numerical or physical decode result.

The A733 manual advertises Main and Main10 decoding at level 5.2, up to
8192×4352 at 24 fps. The supported mode below is the first useful subset; the
remaining modes stay required hardware scope.

## Supported mode

The application accepts one immutable Annex B clip: a VPS, an SPS, a PPS, one
IDR picture and one P picture that references it. The clip must be progressive
Main profile, 8-bit 4:2:0, 640×480 with a 64×64 coding tree block (CTB), one
layer, one temporal sub-layer and one independent slice per picture. The P
picture's only reference is the IDR picture at picture order count (POC) 0, and
its own POC is below half of the POC range. CABAC entropy decoding, prediction,
transforms, sample adaptive offset and deblocking run on the VE.

The parser refuses tiles, wavefront parallel processing, dependent slices,
B slices, long-term references, scaling lists, PCM, weighted prediction,
reference list modification, conformance cropping, 10-bit samples, parameter
set extensions and any additional NAL unit such as an access unit delimiter or
SEI. Each picture holds at most 256 KiB of encoded input. Every NAL unit must
contain only valid emulation-prevention sequences and end in a nonzero byte, so
the parser and the VE's own escape removal read the same bytes.

## Source provenance and execution location

[The pinned source record](../apps/hevc/sources.json) lists every third-party
input with its revision and SHA-256. The adapted sources run on the A733 CPU;
none of them runs on another processor.

- GStreamer `gsth265parser.c` and `gsth265parser.h` at revision
  d31ce8e5e1aacf8f5e5beabb5c81ce2e4da5c202, licensed LGPL version 2 or later,
  supply VPS, SPS, PPS and slice header parsing. At configure time
  `apps/hevc/gst-source.py` checks the pinned hashes and generates a standalone
  copy that keeps the original notices. The copy narrows width, height, bit
  depth, cropping, tiles and wavefront syntax to the supported mode, and records
  the slice header length before `alignment_bit_equal_to_one`. GStreamer's NAL
  reader, `nalutils.c` from the same revision, comes from the H.264 High source
  cache and its `pins.json`. The existing integer port in `apps/h264/gst-port.h`
  replaces GLib.
- Linux Cedrus `cedrus_h265.c` and `cedrus_regs.h` at revision
  70dd4ee72eb2d5793c62e71c14e3aba78e5a4e56, licensed GPL-2.0 or later, supply
  the H.265 register fields, programming order, bit-skipping procedure and
  completion status rules. `drivers/video/a733_video_hevc.c` and
  `apps/hevc/parse.c` carry the adapted code.
- The older H6 CedarC HEVC sources, licensed LGPL version 2, and the effective
  A733 CedarC runtime package are offline references only. The image does not
  link or load them. The runtime package is the 953,940-byte `libcedarc-dev`
  package with SHA-256
  ea2c62123466e233e5f358fffac10499a5ae3886d9fe340532907bba6f2f6d4e; its
  `libawh265.so` has SHA-256
  6adc1bba9ca552e17f6bc8a5d4b28b6cba1a6268feebfded0fa99ed199457df1.

Configure the application with `SUNSTONE_HEVC_SOURCE` naming the directory that
holds the GStreamer parser and Cedrus files, and `SUNSTONE_HEVC_READER_SOURCE`
naming the directory that holds the NAL reader and its `pins.json`. In the
shared cache these are `~/.cache/sunstoneos/sources/hevc-decode` and
`~/.cache/sunstoneos/sources/h264-high`. Configuration stops with an error
naming both variables when either is missing.

## Register programming

The decoder uses the VE_DEC clock, PCK power domain 2, interrupt 81, the decoder
window at `0x01c0e000` and IOMMU masters 6 and 7. Group 5 of the effective
`libVE.so` register map 3 places the H.265 engine at decoder offset `0x500`. The
HEVC binding in `drivers/video/a733_video_platform.c` reuses the codec-neutral
decoder sequencing shared with JPEG and H.264: admission, cold start from the
NS-ROM, identity, memory, and reset with default top-level registers. It selects
decoder mode 4, the H.265 engine. The controller additionally requires compound
identity `0x0003331000021320`, the identity in the effective-library trace
described below.

For each picture the controller performs the following steps, one register
access per poll:

1. Check that the H.265 status register at `0x538` shows no busy, error, data
   request or timeout bit.
2. Program the bitstream window from the start of the picture's input slot:
   offset 0, length in bits, base address with the first, last and valid flags,
   and the slot's last byte as end address. Clear the CTB origin, tile limits
   and CTB count, clear the status bits and start software decoding (trigger 7).
3. Skip the slice header with flush-bits triggers of at most 32 bits, waiting
   for the VLD busy bit to clear after each. The count is the slice header
   length in unescaped bits from the NAL header to just before
   `alignment_bit_equal_to_one`, which is where Cedrus leaves the bit pointer.
4. Write the NAL header, SPS, picture size, PCM, PPS, scaling-list default and
   slice header words, and `0x80000007` to the engine control register. Then
   write the linear NV12 output format, chroma length and strides, disabled
   scaling and secondary output, and the entry-point and neighbor buffer
   addresses. Finally write one 32-byte frame record per picture at SRAM
   `0x400`, the output index and, for the P picture, a list0 entry naming
   frame record 0.
5. Trigger slice decoding (trigger 8). Completion requires the finish bit with
   no error, data request, timeout or busy bit, and a CTB count of exactly 80.
   The controller then disables the engine interrupt bits and clears the
   observed status.

Frame record *n* belongs to picture *n* and its index is also the output index.
Each record holds the POC twice, the motion-vector address twice, and the luma
and chroma addresses, all addresses shifted right by 8. The P picture sets
slice-header bit 21, which Cedrus and CedarC both set for a P or B slice whose
references all precede it in output order. GStreamer leaves the unsignaled PCM
fields zero, so the PCM word is zero while PCM is disabled.

`checks/hevc-decode-model.py` compares the IDR picture's programming with the
effective A733 `libawh265.so` instruction trace for the same clip,
`~/.cache/sunstoneos/evidence/hevc-decode-20261020/stream-adapted-registers/result.json`.
That trace ends at the library's first decode trigger. The comparison shows:

- Equal final values for the VE mode register, registers `0x500`–`0x528`
  except `0x50c`, `0x530`, `0x53c`, `0x550`–`0x558`, `0x55c`, `0x580`, and the
  top-level output registers `0xc4`, `0xc8`, `0xcc`, `0xe8` and `0xec`.
- The same write order after trigger 7, once the library's cycle-counter
  writes, the native controller's flush triggers and its entry-point address
  are set aside.
- The same bitstream position. The library sets the bitstream offset to 718
  bits, which is 86 bytes to the IDR NAL plus 30 header bits; the native
  controller flushes those 30 bits from the start of its slot.
- The same frame record layout, with chroma 307,200 bytes after luma and equal
  motion-vector addresses.
- Different PCM words, `0x0` natively and `0x877` in the library. PCM is
  disabled in both, so only unused sample fields differ.

The native controller also writes the zero CTB origin and tile limits and the
entry-point address, as Cedrus does, and omits the library's VE cycle-counter
writes at decoder offset `0x8`. No reference trace covers the P picture; its
registers follow Cedrus and CedarC and are checked against those rules.

## Memory and ownership

All buffers come from the existing extended-memory profile, so the image needs
no change to shared CPU or memory reservations. Device buffers are in the
uncached pool at `0x78000000`; the CPU-only upload and copied pictures are in
the write-back pool at `0x48000000`.

| Uncached allocation | Mapped bytes | IOVA |
| --- | ---: | --- |
| IOMMU tables | 32,768 | not mapped |
| IDR input | 262,144 | `0x10001000`, read only |
| P input | 262,144 | `0x10043000`, read only |
| IDR output | 462,848 | `0x10085000` |
| P output | 462,848 | `0x100f8000` |
| IDR motion vectors | 16,384 | `0x1016b000` |
| P motion vectors | 16,384 | `0x10171000` |
| Neighbor metadata | 815,104 | `0x10177000` |
| Entry points | 4,096 | `0x10240000` |

Each mapped buffer sits between two unmapped guard pages filled with `0xa5`,
and unmapped IOVA gaps separate the spans inside one 4 MiB table window. With
the guard pages, the uncached allocations total 2,400,256 bytes. The IOMMU
tables use each allocation's physical address from the allocator. Each output
holds 460,800 bytes of NV12 picture: 640-byte luma rows followed by interleaved
chroma at offset 307,200. Its remaining 2,048 bytes keep the guard pattern.
Motion-vector buffers hold the Cedrus requirement of 160 bytes per CTB plus
1,024 alignment bytes, 13,824 bytes for 80 CTBs. The neighbor buffer covers the
813,056 bytes that Cedrus, CedarC and the effective library all allocate.

The write-back pool holds a 524,288-byte upload buffer and two 462,848-byte
copies, 1,449,984 bytes in total. Their leases are retained at allocation. The
parser's private 589,824-byte workspace is in the image's BSS.

Startup allocates storage and builds the tables before the scheduler starts.
After the upload is committed, the controller task checks its digest again,
copies each slice NAL to the start of its zero-padded input slot, clears
outputs and scratch, and marks every uncached allocation as device owned. A
successful picture returns no allocation. A failure retains every allocation,
mapping, clock and power lease and token until whole-board power removal, and
later polls perform no device access.

## Copied output and USB protocol

After a picture completes, the copy callback runs a fresh IOMMU fault scan and
checks the guard pages and the idle status register. It then copies the 460,800
output bytes to the picture's write-back copy in 64 KiB steps, one per task
wake, and hashes the copy. A second fault scan, guard check and status check
precede publication. The application publishes the IDR copy, waits for the
host to acknowledge it, and only then programs the P picture.

USB0 carries the protocol that `apps/hevc/host.py` and `apps/hevc/observe.py`
implement on the host:

- Upload: a 32-byte header (magic `0x55363235`, version 1, clip bytes, picture
  count 2, session epoch and generation) followed by the clip. The device hashes
  and parses the complete upload before the controller may use it.
- Query: byte 0 followed by the picture ordinal. The reply is a 160-byte header
  with status 0 (pending), 1 (copied) or 2 (failed), the session identity, the
  clip digest, the copy digest, the picture geometry, slice type and a terminal
  failure flag. Only status 1 appends the 460,800-byte copy.
- Acknowledgment: byte 1, the 32-bit ordinal, the session generation and the
  copy digest. Acknowledgments must arrive in order and name a copied picture
  with its digest; any other byte sequence stops the board.

## Session, startup and preparation

`include/a733_hevc_application.h` defines the 856-byte immutable session: the
592-byte supervised board session, identities and owners, digests of the four
physical evidence records, the clip digest, the two picture ranges, and the
clock, IOMMU gate and supply fields. The extended boot copies the 288-byte
protected contract and the session before any provider starts, and the
application starts only when the admitted board prefix and build ID match.

`apps/hevc/prepare.py` derives the launcher, contract and session for a
supervised run. It refuses at admission, before any bundle is written, because
no record yet proves the physical allocation addresses and page tables of the
mapped buffers. A run needs that record together with the initial-isolation,
memory, electrical and supervision evidence that the admission schema lists.

## Checks

- `checks/hevc-decode-model.py` builds `checks/hevc-decode-model.c` under
  AddressSanitizer and UBSan with the generated parser, `apps/hevc/parse.c`,
  `apps/hevc/storage.c`, the controller, the real decoder binding and the real
  clock and power lease managers. On the recorded clip
  `hevc-decode-20261020/two-pictures.hevc` it checks header lengths, POCs and
  slice fields against FFmpeg's `trace_headers` output, and makes the register
  comparison above. Parser cases refuse invalid escapes, forbidden bits, wrong
  layers, NAL order, ranges and workspace; a storage case maps physical
  addresses that differ from CPU addresses. Controller cases cover the nominal
  IDR then P sequence, error, data request, short CTB count, deadline,
  unsupported identity, copy refusal, flush error and a busy P entry. Every
  failure stays terminal with all leases retained.
- `checks/hevc-native-access.py` runs the AArch64 build of the lowest native VE
  access functions in Unicorn for every register write recorded by the model
  and for both status reads. It also checks refusal of neighboring offsets,
  byte writes and pages without a Device-nGnRnE identity mapping.
- `checks/hevc-image.py` checks a built image and its manifest against the ELF
  rules of `apps/hevc/prepare.py`.
- `checks/hevc-session.c` checks session range admission with synthetic
  provider identities.

These checks model MMIO, translation and completion. They do not establish
that the VE decodes the pictures correctly, that the A733 accepts every Cedrus
control bit, or that copied output implies drained device transactions.

## Remaining work

- A physical allocation and page-table record for the mapped buffers, so that
  preparation can admit a run, and the four physical evidence records.
- A board run of the two-picture clip, with copied pictures compared against an
  independent decoder's output for the same clip.
- Main10, larger pictures, multiple slices, tiles, wavefront parallel
  processing, B pictures, more references and compressed output.
