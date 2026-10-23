# A733 video and image codecs

SunstoneOS has a baseline JPEG parser, hardware-table builder and checked
register executor for the A733 video decoder. The executor compiles for native
AArch64 and matches instruction traces from an A733/T736 runtime package.
The [native JPEG application](jpeg-application.md) integrates the executor with
clock/reset providers, decoder IOMMU mappings, guarded buffers and copied USB
output, and decodes sequences of up to eight images in one session. Its central
image links and focused software checks pass. Physical
decoding, memory confinement, completion and numerical conformance remain
unqualified. The [native JPEG encoder](jpeg-encode-native.md) now implements a
one-job VGA NV12/quality75 path with authenticated upload and retained copied
JPEG output. The [native H.264 encoder](h264-encode-native.md) implements a retained
VGA NV12 Baseline sequence of one IDR and up to 29 P pictures at fixed QP 26,
with caller upload and indexed copied AnnexB output. Focused models pass; physical encoding remains unqualified.

This document is for engineers implementing FreeRTOS device drivers. A video
engine (VE) contains several codec register groups. An IOMMU master identifies a
memory requester; it does not identify an independently programmable codec.
An I/O virtual address (IOVA) is the address presented by that requester.

## Primary sources establish separate decode and encode resources

The A733 User Manual V1.00, sections 14.1–14.2, pages 637–640, specifies codec
families and limits. It does not describe the codec registers. Its SHA256 is
`cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d`.
The local copy is
`~/.cache/sunstoneos/sources/camera-research/A733_User_Manual_V1.00.pdf`.

The [OrangePi kernel device tree](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi),
clock driver and `bsp/drivers/ve/cedar-ve` platform driver establish the operating
system resources. All references use revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`.

| Resource | Decoder | Encoder |
| --- | --- | --- |
| Register base and device-tree span | `0x01c0e000`, `0x3000` bytes | `0x01c10000`, `0x1000` bytes |
| Top-level register offset | `0` | `0x800` |
| Level-high interrupt | SPI 49, GIC INTID 81 | SPI 48, GIC INTID 80 |
| IOMMU masters | 6 and 7, bank 1 local IDs 0 and 1 | 3, bank 0 local ID 3 |
| NSI memory-interconnect ports | 7 and 8 | 9 |
| Power-domain symbol | `SUN60IW2_PCK_VE_DEC` | `SUN60IW2_PCK_VE_ENC` |

The decoder's `ve1` device-tree node supplies master 7 and NSI port 8. It has no
independent register window or interrupt. Both decoder masters must remain under
one ownership contract until the selected profile's use of each master is
established. The advertised decoder span overlaps the encoder window; decoder
ownership does not authorize writes anywhere in that span.

The clock-control unit (CCU) uses the following offsets. Keys are required for
the shared AHB and memory-bus registers. A provider must preserve every unrelated
user while changing these registers.

| Control | Decoder | Encoder |
| --- | --- | --- |
| Module clock | `0xa88` | `0xa80` |
| Divider, source mux, gate | bits 4:0 plus one, bits 26:24, bit 31 | Same fields |
| Bus gate at `0xa8c` | bit 2 | bit 0 |
| Reset at `0xa8c` | bit 18 | bit 16 |
| AHB gate at `0x5c0`, key `0x010000ff` | bit 0 | bit 1 |
| MBUS master gate at `0x5e0`, key `0x41055800` | bit 14 | bit 12 |
| MBUS clock at `0x5e4`, key `0x00040302` | bit 18 | bit 1 |

Decoder clock parents, in mux order, are PLL VE1, PLL VE0, peripheral 800 MHz,
peripheral 600 MHz, peripheral 480 MHz, display-engine 3x, and PLL NPU.
Encoder parents swap the first two entries. PLL VE0 and VE1 control offsets are
`0x220` and `0x240`; NPU also uses these PLLs. The JPEG executor does not program
shared clocks or infer their current rates. The native provider needs a qualified
power/reset sequence and shared-clock ownership before using these fields.

## The published runtime provides a concrete JPEG register reference

Radxa's [A733/T736 root-filesystem recipe](https://github.com/radxa/allwinner-debian/blob/7f592b79a85cb7b5ef86dabaf2016013b85a8d4d/mk-custome-rootfs.sh)
installs the [CedarC development package](https://github.com/radxa/allwinner-debian/blob/7f592b79a85cb7b5ef86dabaf2016013b85a8d4d/packages/arm64/libcedarc/libcedarc-dev_2.0.0_arm64.deb).
The package is 953940 bytes and has SHA256
`ea2c62123466e233e5f358fffac10499a5ae3886d9fe340532907bba6f2f6d4e`.
Its filename says 2.0.0; Debian metadata identifies
`libcedarc-dev-2.0.0-arm64`, version `1.0.7`.
Extraction used `dpkg-deb -x`; package scripts were not executed.

| Extracted library | SHA256 | Reference behavior |
| --- | --- | --- |
| `libawmjpeg.so` | `c6c0fff04dcddc633544831430727b445a3f1fe4873c881a16aeaed958513e65` | JPEG parsing, quantization/Huffman SRAM packing, plane layout and trigger |
| `libVE.so` | `3af49a5eb24f6832518a858274fb206d60e490985bff02f5d5be36d053f65be4` | Compound identity, register routing, engine selection |
| `libvideoengine.so` | `abfea1fa75f6a3ca5413d16eb69fcb87692f3e3b6552d83a222a998e909cef82` | Codec selection and frame allocation geometry |

These are Linux host libraries, not native FreeRTOS drivers. The JPEG
implementation uses the executable instructions as a register-format reference;
it does not load those libraries on the board. Cached binaries retain their
original contents and notices.

The package selects the legacy `mjpeg` decoder for compound identities
`0x0003332100021321`, `0x0003331000021320` and `0x0003052000012011`.
The native executor admits only these identities. None has been observed on the
physical target. Identity admission requires top register `0xf0` to be zero or
`0x16670040`; the compound identity combines the low 20 bits of `0xe0` and `0xe4`
as high and low 32-bit halves.

For the package's split register map, the decoder context routes top-level
registers to offset zero and MPEG registers to `0x100`. Legacy JPEG uses this
MPEG group. A separate JPEG group exists at `0xe00`, and the package includes
`libawmjpegplus.so`; that layout is not the selected profile. The shipped header's
numeric group labels disagree with the binary's split-map top-level entries.
The routing check executes the binary dispatch with a decoder context and uses
its returned addresses. The decoder0/encoder1 creator flags select encoder top at
`0x800`, so that flag is part of the checked reference setup.

## The first native profile accepts bounded baseline JPEG

[The parser interface](../include/a733_video_jpeg.h) accepts baseline 8-bit
YCbCr 4:2:0 with components 1, 2 and 3 in one interleaved scan. Quantization and
Huffman tables must be explicit, use IDs zero or one, and agree between the
chroma components. It validates segment lengths, canonical Huffman codes,
symbol ranges, scan framing, restart-marker order/count and an exact terminal
end-of-image marker. Unsupported or malformed input leaves output unchanged.

Progressive JPEG, multiple scans, other sampling formats, abbreviated MJPEG
with implicit tables, and Adobe RGB are refused. The caller's input contract is
YCbCr. The parser does not decode entropy-coded coefficients, so accepted marker
framing does not establish a valid coefficient stream or bounded device access.

The table builder emits 128 quantization writes and 512 little-endian Huffman
SRAM words. Its frame size counts 16-by-16 minimum coded units (MCUs), with each
axis encoded as its count minus one. Preparation supports dimensions through
16384 by 16384; the executor restricts its first profile to 1920 by 1080.

[The executor interface](../include/a733_video_jpeg_run.h) requires fresh zeroed
storage and a single privileged caller. It performs no allocation. The caller
supplies a CPU input alias and disjoint input, luma and chroma IOVA spans.
Input addresses must be 16-byte aligned; output addresses must be 32-byte
aligned. All spans must fit below 4 GiB, and the input capacity is at most
16 MiB. The complete input file must fit that capacity.

Output is planar YU12: luma followed by separately addressed contiguous Cb and
Cr planes. Luma stride and row allocation round the width and height up to 32.
Each chroma plane occupies one quarter of the padded luma allocation.
The executor checks minimum span sizes; these arithmetic checks do not establish
hardware burst limits or physical alias exclusion.

The controller advances through these operations:

1. Acquire stable ownership of the job, frozen input, both masters, decoder
   registers, power, clocks, reset and interrupt. Before identity admission,
   permit only power/clock prerequisites for identity reads. Do not reset,
   select or program engines, or change translations in acquisition.
2. Parse the file and build tables. Read and admit the compound identity before
   invoking preparation. Unknown identities retain ownership and never prepare
   translations or program the codec.
3. Require the provider to establish cold handoff, independent recovery, both
   masters' translations, full access extents and cache publication. Select the
   MPEG engine with VCU, scaling and rotation disabled.
4. Check engine selection and absence of stale status. Program restart count,
   sampling, quantization, output addresses, MCU size, Huffman SRAM, input end,
   planar format, strides, scan bit offset and scan bit length.
5. Enable codec completion/error interrupts with `0x1fc` at `0x114`. Encode the
   input base at `0x128`; trigger with a byte write of `0x0e` at `0x118`.
6. Poll status at `0x11c`. Reject error bits before accepting completion bit zero.
   Disable codec interrupt enables with `0x180` at `0x114` and acknowledge only
   observed low status bits. Require a separate completion callback to confirm
   stopped producers, completed external transactions, no IOMMU faults and
   invalidated output caches before reporting DONE.

The register program contains 659 writes before polling. The input end at
`0x134` is inclusive. The scan length includes the end-of-image marker, matching
`JpegHwDec`. Shared output-format bits at `0xec` are preserved outside mask
`0x77`. The first profile requires no scaling, rotation or video-control-unit
(VCU) execution.

Every external operation has monotonic deadline checks before and after it.
Unexpected callback results, backward time, timeout and device errors produce a
terminal retained FAULT. Even a trigger that completes after its deadline leaves
ownership retained. DONE allows output consumption under retained ownership;
neither terminal state authorizes reset, unmap, memory reuse or lease release.
The package's unconditional successful return after waiting is not used as
completion evidence.

## Instruction comparisons verify preparation and programming

The checks run original AArch64 library instructions in Unicorn with bounded
memory and explicit service stubs. They do not emulate the codec hardware.
`a733-video-jpeg-reference.py` executes the package's DQT, DHT, SOF, SOS and DRI
parsers, `SetJpegFormat`, `InitJpegHw` and `JpegHwDec`.

The comparison passed for a 48-by-32 FFmpeg baseline JPEG and a constructed
restart/table-selection case. Quantization writes, Huffman SRAM words, geometry
and restart settings matched. Every prefix truncation of the baseline fixture
was reported refused; selected malformed metadata cases were also reported
refused. The original parser-check script was not archived, which limits
reconstruction of that historical malformed-input result.
The complete pre-trigger register byte image, table streams and byte trigger
matched for planar YU12 with 32-byte stride/row alignment.

The executor's sanitizer model passed success, pending completion, preparation
failure, unknown identity, stale interrupt status, device error, late trigger,
write failure, completion failure, unexpected read result, backward time and
invalid span cases. Unknown identity never calls preparation. Successful output
requires the completion callback; failures retain the ownership token.
Parser and executor compile with warnings treated as errors for
`aarch64-none-elf`. No board or device was accessed.

Saved source, fixture, object files, traces and SHA256 identities are under
`~/.cache/sunstoneos/video-codecs/review-20261016/`. Its `sha256.json` has SHA256
`2f103cc434bb5f7244f9906e08656787544990b495449be8e644bcc85251c4e1`.
`evidence/preparation-1/` contains table traces;
`evidence/execution-1/` contains native and reference register images;
`evidence/routing-2/routing.json` records codec selection and register routing.
The identity-before-preparation change preserves the previously compared write
trace byte for byte. The historical preparation manifest binds the native shared library
(`7d439f06fea91a77258666dc1e47da6e54b17e607b2cb606f1d05f2bfa2935f6`),
the reference JPEG library and the baseline fixture
(`6522f2719f1f05e23a95aa71a18d52f2e91b19b4b6c94381b4100de4c79fd573`).
All three artifacts remain available with matching hashes. Quantization/Huffman
outputs and reference write traces remain inspectable for both preparation cases.

The original Python runner has recorded hash
`54496026d71d4bcedbb5eeb4e270948510b343d316acf482206822d9c5402927`,
but its exact source was not retained. The synthetic input bytes and individual
malformed-case outcomes were also not archived. Consequently the original
malformed-input execution cannot be fully reconstructed from the record.
The preserved extended runner adds an optional executor-trace argument,
group/wait/alignment service stubs and a branch comparing planar programming.
That branch exits before the existing synthetic/refusal cases. The native parser
and table builder remained unchanged. The extended runner and executor model
source are preserved, along with native objects, the sanitizer executable and
register traces. This preserves the executor comparison without claiming a
byte-identical reconstruction of the earlier Python runner. `results.txt` records that limitation and the
corrected harness/model failures.

## Full codec support remains required

The manual's advertised limits define the remaining hardware inventory. They
are capability claims, not SunstoneOS qualification results.

| Direction and family | Advertised profile or limit | Runtime reference and native status |
| --- | --- | --- |
| JPEG encode | Baseline, 64-megapixel snapshots, 4K at 15 fps; 4:2:0/4:2:2/4:4:4, grayscale | Native one-job VGA NV12/quality75; modeled path tested, physical unqualified |
| H.264 encode | Baseline/Main/High, level 4.2, up to 4K at 30 fps | Bounded native VGA Baseline sequence of up to 30 IDR/P pictures at fixed QP modeled; rate control, broader profiles/rates/resolutions remain required; manual's level/resolution combination needs clarification |
| H.265 encode | Main, level 5 Main Tier, 4K at 30 fps | Encoder libraries present; native unsupported |
| MJPEG decode | Up to 16384 by 16384; 1080p at 60 fps | Baseline YCbCr420 up to 640-by-480 planes, one image or a sequence of up to eight per session; modeled, physical execution unqualified |
| H.265 decode | Main/Main10, level 5.2, 8192 by 4352 at 24 fps | [Native 640-by-480 Main IDR and P build](hevc-native.md); modeled against the effective `libawh265.so` trace, preparation awaits physical allocation evidence, physical decoding unqualified; wider modes remain required |
| VP9 decode | Profiles 0/2, level 6.1, 8192 by 8192 at 24 fps | `libawvp9HwAL.so`; native unsupported |
| AVS2 decode | 8192 by 4608 at 24 fps | `libawavs2.so`; native unsupported |
| H.264 decode | Baseline/Main/High, level 5.1, 4096 by 2304 at 30 fps | [Native Baseline 48-by-32, VGA and full-HD streaming builds and a runtime build for Baseline, Main and High I/P/B streams with up to four short- or long-term references](h264-native.md); implemented and modeled, physical decoding unqualified; long-term references in B pictures, fields, levels above 4.0 and wider formats remain required |
| H.263, MPEG-4 SP/ASP level 5, XVID, Sorenson decode | 1080p at 60 fps | MPEG-4 family libraries present; per-format paths need derivation |
| MPEG-1/2 decode | 1080p at 60 fps | `libawmpeg2.so`; native unsupported |
| VP8 decode | 1080p at 60 fps | `libawvp8.so`; native unsupported |
| AVS/AVS+ decode | 1080p at 60 fps | `libawavs.so`; native unsupported |
| WMV9/VC-1 decode | Simple/Main/Advanced, 1080p at 30 fps | No dedicated VC-1 library found in this package; native unsupported |

The manual does not advertise AV1. A generic header enumeration is insufficient
evidence of A733 AV1 hardware support. The package also contains software codec
libraries; their presence does not establish hardware implementation.

The manual describes codec framework software running on the ARM host. No
external firmware file was found in the extracted package, and the inspected
kernel VE path does not call `request_firmware`. The chosen JPEG register path
does not load a firmware image. This evidence does not establish that every
codec is firmware-free: `libvdecVcs.so` contains VCU scheduling functions whose
native requirements still need derivation.

## Native binding and physical qualification are the next dependencies

A native provider must resolve the following before the JPEG executor can run:

- Qualified independent watchdog/ROM recovery and a cold handoff that excludes
  inherited decoder transactions. A timer timeout alone does not supply this.
- Read-access power/clock prerequisites, then supported-identity admission before
  codec reset or selection. The clock provider must preserve NPU/display users
  and establish rates, reset timing, power isolation and interrupt ownership.
- Mappings for masters 6 and 7 with physical ownership, alias exclusion,
  input/output cache maintenance, fault attribution and bounded invalidations.
  The existing camera IOMMU interface does not authorize codec mappings.
- Actual input over-read and output burst extents, including prefetch at the
  inclusive input end and padded planes. Neither a minimum allocation formula
  nor the parser's marker checks proves these extents.
- A source-derived producer-stop and accepted-transaction drain sequence.
  Codec completion, interrupt acknowledgement and CPU barriers alone do not
  establish final external writes or authorize reclamation.
- Board-observed compound identity, followed by a bounded RAM-only decode with
  independent software pixel comparison and guard/fault evidence. The root
  coordinator owns that future physical test.

The remaining families need native parser/control paths, codec-specific memory
and firmware contracts, and physical qualification. Linux is a reference for
those implementations; the runtime operating system remains FreeRTOS.
