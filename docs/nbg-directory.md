# Decode the compiled convolution's section directory

The NBG `0x20000` directory and checksum findings provide **reference evidence**.
The [native decoder](nbg-model.md) now supports its 32-bit-address layout;
native checksum verification also passes host checks. The [native assembler](nbg-assembly.md)
now matches saved SDK buffers for the numerical fixture.
The [compiled convolution](npu-compiler.md)
provides a pinned numerical fixture for implementing support without weakening
the existing `0x1001e` checks. This document assumes familiarity with binary
formats, address relocation, and CRC checksums.

## The directory supplies offsets, sizes, and record widths

The fixture has a 48-byte format header, one 12-byte subheader at offset 48,
and thirteen 44-byte section records at offset 60. Payloads occupy bytes
632–1927 without gaps. Header byte 12 holds the checksum; bytes 16–23 hold the
64-bit total size. Bytes 24, 28, and 32 hold the subheader offset, size, and count.

Each section record contains a type, 64-bit file offset, 64-bit byte count,
64-bit address, alignment, entry size, and two reserved words. All address and
reserved fields are zero in this fixture. The coefficient section requires
64-byte alignment; other alignment fields are zero.

| Section | Type | Offset | Bytes | Entry bytes |
| --- | ---: | ---: | ---: | ---: |
| Network information | 35 | 632 | 220 | 220 |
| Internal SRAM requirement | 38 | 852 | 4 | 4 |
| Section checksums | 40 | 856 | 24 | 8 |
| Input | 1 | 880 | 112 | 112 |
| Output | 2 | 992 | 112 | 112 |
| Layer | 3 | 1104 | 76 | 76 |
| Operations | 4 | 1180 | 64 | 32 |
| Neural descriptor | 7 | 1244 | 192 | 192 |
| Address patches | 10 | 1436 | 140 | 28 |
| Coefficient ranges | 23 | 1576 | 8 | 8 |
| Command-state ranges | 27 | 1584 | 16 | 8 |
| Coefficients | 24 | 1600 | 256 | 1 |
| Command states | 21 | 1856 | 72 | 1 |

SDK debug information and reader instructions establish the changed layouts.
Tensor records use an eight-byte quantization union at byte 40 and a 64-byte
name at byte 48. Asymmetric quantization stores the scale at 40 and zero point
at 44. The older format stores separate fixed-point, scale, and zero-point fields
and starts the name at 52.

The fixture declares 32-bit addresses and uses seven-word, 28-byte patches.
The words hold patch type, destination offset, source type, source index,
source offset, transformation, and name identifier. The SDK selects a separate
40-byte patch layout for a declared 40-bit address width. Native support must
validate the selected width before reading records.

Network attributes also moved: the first word at network-information byte 92
contains the change-parameter bit, two-bit neural width selector, two-bit tensor
width selector, and two-bit DMA selector, starting at bits 0, 1, 3, and 5.
The fixture's word is `0x0a`; both command-width selectors are one, meaning
192 bytes in the inspected SDK.

One count remains ambiguous. The network-information field named
`operation_count` in SDK debug information contains five, while the operation
section contains two records and the SDK inspector reports two operations.
The five address patches do not establish that this field counts patches.
Use the checked section size for record bounds and resolve this discrepancy
before assigning a semantic invariant to that field.

## The payload checksum excludes the header and directory

The stored value `0xe5e19256` matches CRC-32/ISO-HDLC over bytes 632–1927 when
the checksum-value words at bytes 860, 868, and 876 are treated as zero.
The checksum records' section-type words remain included. Computing the CRC
over the complete file with the header checksum zeroed does not match.

The compiler's writer initializes a running CRC, writes the header and directory
without updating it, then updates it as section payloads are emitted. It stores
that accumulated value in the header before filling the section-checksum values.
This ordering explains the observed normalization. The inspected writer also
updates the CRC when emitting alignment padding. The current fixture contains
no padding, so a padded model remains necessary evidence for general support.

The section checksums independently match their payloads:

| Payload | Stored and computed CRC |
| --- | --- |
| Neural descriptor | `0x9c9826b8` |
| Coefficients | `0xf326459f` |
| Command states | `0xef85cb7a` |

A changed target ID leaves the payload checksum unchanged. Successful CRC
verification therefore cannot establish header or directory integrity. Structural
validation, target admission, and a trusted model identity remain separate
requirements. CRC also supplies no authentication for the payloads it covers.

## The focused comparison reuses the numerical fixture

[nbg-directory-reference.py](../checks/nbg-directory-reference.py) accepts the
pinned model, SDK inspector, compiler library, and a new output directory.
It extracts relevant DWARF type definitions, records directory and tensor data,
and compares Python zlib with the compiler's isolated CRC routine in Unicorn.
The check is specific to the pinned fixture; it is not a general container validator.

One invocation passed all three section checksums and the normalized payload
checksum. It also matched incremental CRC calculation across all thirteen
sections. Seventeen isolated calls used a one-second and 100,000-instruction
limit per call. The compiler, loader, simulator, and device interfaces did not
execute. Existing numerical comparisons were reused; no target rebuild was needed.

The inspected files have these SHA-256 identities:

- SDK inspector: `5ef6bb4f1590e0d1b9603506574bee09321d6159e32a6b3aaef66148085b2639`.
- Compiler `libOpenVX.so`: `0d2ee44aaf78e0f2a7e4470ad5f24ea2094c8c7d772b2b050e2f7f81361bcd12`.
- Compiled model: `db42d815100417f4b8913896462ed031beafae35fb4ee1c28569ac950a9e8fca`.

The compiler CRC routine occupies offsets `0x21e830` through `0x21e8b2`.
Its accumulator helper starts at `0x21fb80`; the section writer starts at
`0x22d210`. The SDK inspector's network and patch readers start at `0x40d82d`
and `0x40ea8c`. Original binaries, disassembly, extracted types, comparison
results, and identities remain under
`~/.cache/sunstoneos/evidence/nbg-directory-20261016/`.

Bounded native directory decoding and version-specific record accessors now
have host evidence in [nbg-model.md](nbg-model.md). The
[checksum verifier](nbg-checksum.md) now implements the primary and section rules.
The [convolution capture](nbg-conv-loader.md) supplies allocation and command bytes
for extending assembly to the legitimate empty working pool.
Model admission, cold initialization, physical execution, and
the remaining hardware capabilities are still outstanding.
