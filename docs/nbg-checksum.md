# Verify compiled-model integrity before assembly

Native checksum verification is **implemented and host-tested** for NBG formats
`0x1001e` and the supported `0x20000` directory layout. Coverage differs by format:
the older format covers the complete file; the directory format excludes its
header and directory. Successful verification detects corruption within that
coverage. It does not authenticate a model or establish command safety.

This document assumes familiarity with binary formats and checksums. The interface
is [nbg_checksum.h](../include/nbg_checksum.h), implemented in
[nbg_checksum.c](../drivers/npu/nbg_checksum.c).

## Fixed-header files cover every byte with the checksum field zeroed

The algorithm is CRC-32/ISO-HDLC: reflected polynomial `0xedb88320`, initial state
`0xffffffff`, and final complement. For `0x1001e`, the field at bytes 108–111 is treated
as four zero bytes during computation. The implementation substitutes those bytes
while reading; it never modifies the input file.

The whole-file coverage rule is inferred from two independently published models
with matching stored checksums:

| Published model | Format | Bytes | Stored and computed CRC |
| --- | --- | ---: | --- |
| A733 SDK ShuffleNetV2 UINT8 | `0x1001e` | 964,224 | `0xdd476ad1` |
| TIM-VX convolution/ReLU/max-pool FP32 | `0x1001e` | 9,152 | `0x48739b09` |

The [NPU source record](npu.md) pins both repositories. The model SHA-256 values are
`fc0d50bdc863e4dfc74eef6dad9d34655c560e759a2f426979a01c505fa9b95f` and
`e898680018e90837ec6941c9f0fa5566bde8876ac29a366d9ee53469342cb276`, respectively.
The TIM-VX model targets `0xba`; it supplies checksum evidence and is not admitted
by the A733 assembler. The cached older SDK model uses format `0x10014` and has
a zero field. This verifier refuses that format; its checksum semantics remain unresolved.

The pinned SDK routine at offset `0x12b70` implements the same reflected CRC
primitive. An isolated instruction check computes both normalized model checksums
and matches native C and Python zlib. This does not establish that the SDK loader
enforces whole-file integrity. Its separate section-checksum routine has a
version-bit guard that excludes the inspected `0x1001e` files.

## Directory files require payload and listed section checksums

For `0x20000`, the verifier first calls [nbg_model_open](nbg-model.md). It requires
32-bit addresses, the 48-byte header, a 12-byte subheader immediately afterward,
and consecutive 44-byte directory records starting at byte 60. Other placements
can pass structural decoding but have no established checksum coverage and are
refused by the verifier.

The primary CRC covers the concatenation of declared section payloads in emitted
directory order. Alignment gaps are excluded. Each checksum-value word in the
section-checksum table is treated as zero; section-type words remain included.
The verifier then checks every listed section against its stored CRC. The table
must be nonempty, name distinct present sections, and exclude itself. Repairing
the primary CRC cannot conceal a failure in a listed section checksum.

The [directory reference](nbg-directory.md) establishes the coverage from the
pinned compiler writer and two emitted artifacts, including the padded
[two-stage graph](npu-two-stage.md). Native admission additionally requires
ascending nonempty payload offsets, zero uncovered gaps and no trailing bytes.
These are conservative native layout restrictions; the CRC does not protect
excluded bytes. The decoder independently bounds nonoverlapping sections and
section-relative tables. The assembler obtains descriptor and coefficient bytes
only through those declared, checked sections, so gap bytes cannot become loaded
payload through a file-relative offset.

Header and directory bytes are **outside checksum coverage**. A structurally valid
changed target ID still passes checksum verification. Target compatibility,
trusted model identity, and command admission must be checked separately.

## Preserve input bytes and distinguish invalid input from corruption

`nbg_checksum_verify` accepts stable readable input up to 256 MiB. Fixed-header
files require at least 592 bytes. Directory files require successful structural
decoding before checksum reads. Input may be unaligned. The writable result
must not overlap the file.

A match returns the primary stored/computed pair. A mismatch returns the first
failing pair: the primary checksum first, then section checksums in table order.
Invalid input leaves the result unchanged. These checks cannot establish whether
an arbitrary pointer names readable memory. A stored zero is compared normally;
zero never disables verification. Generated zero-CRC cases cover both formats.
The directory zero-CRC fixture changes bytes within the final declared payload
and repairs its listed section CRC; it does not append untracked bytes.

The implementation processes each byte with two four-bit table steps. It reserves
64 bytes of read-only table storage and uses no heap, mutable static state,
initialization lock, or device interface. Work is linear in file size. Distinct,
nonoverlapping section ranges bound the additional section-CRC pass.

The assembler requires a matching checksum on each requirements or assembly call.
The [assembler](nbg-assembly.md) supports the observed fixed-header and directory
profiles; checksum success alone does not establish profile compatibility.
Callers keep the file and decoded view immutable during use.

## Focused checks cover both formats and section integrity

One ASan/UBSan check passed both published fixed-header models, the saved zero-CRC
file, unaligned input, 64 corruption cases, and 18 invalid-input cases. This check
was repeated because the byte-update helper and version dispatch changed. The
existing isolated SDK CRC comparison was reused.

The original directory ASan/UBSan result covers corruption, repaired-primary
section corruption, duplicate references, metadata bounds and output aliasing.
Its synthetic whole-tail gap and appended zero-CRC assumptions were superseded
by the pinned writer's declared-payload coverage. The fixture producer now uses
that coverage and creates zero CRC inside a declared payload.

[nbg-directory-padding.c](../checks/nbg-directory-padding.c) passes fifteen
focused ASan/UBSan cases: the actual padded graph, prior emitted model, corrected
synthetic gap and zero CRC, nonzero gap endpoints, trailer, reordered payloads,
overlap, coefficient-relative escape, independently corrupted payloads after
primary repair, and the actual two-operation assembly requirements. Unchanged
fixed-format and controller checks were reused. Current results and exact inputs
are in `~/.cache/sunstoneos/evidence/npu-two-stage-20261019/`.

The earlier AArch64 verifier contained 1,128 bytes of text and read-only data,
no writable static data, and one external reference to `nbg_model_open`. Its
SHA-256 is `8915adbe2297ac15dc6e0b87750add313b175f965722809a7a82b86f51de2b60`.
Its recorded frame occupied 528 bytes. The directory path adds the decoder's 944-byte
open-to-tensor path, totaling 1,472 bytes before memory-helper stack use.
The isolated target is `nbg-checksum`. Host links now require `nbg_model.c` too.

Current commands, artifacts, input identities, and results are under
`~/.cache/sunstoneos/evidence/nbg-directory-checksum-20261016/`. The fixed-header
reference evidence remains under `~/.cache/sunstoneos/evidence/nbg-checksum-20261016/`.
[nbg-checksum.c](../checks/nbg-checksum.c) consumes the two published models and
saved zero-CRC fixture. [nbg-checksum-reference.py](../checks/nbg-checksum-reference.py)
records the earlier isolated SDK comparison; it need not run when those inputs
and CRC semantics remain unchanged.

The separate [convolution loader capture](nbg-conv-loader.md) records SDK preparation
for the newly verified model. It stops before device submission and supplies
allocation and command evidence now matched by native assembly. That loader capture stopped before device submission.
Descriptor access bounds, SRAM routing, command admission, and physical inference
remain outstanding.
