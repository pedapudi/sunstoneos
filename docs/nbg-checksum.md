# Verify compiled-model integrity before assembly

Native checksum verification is **implemented and host-tested** for NBG format
`0x1001e`. The assembler requires a match before publishing allocation requirements
or modifying output buffers. The verifier covers the complete supplied file,
including gaps and trailing bytes. Successful verification detects corruption;
it does not authenticate a model or establish command safety.

This document assumes familiarity with binary formats and checksums. The interface
is [nbg_checksum.h](../include/nbg_checksum.h), implemented in
[nbg_checksum.c](../drivers/npu/nbg_checksum.c).

## Compute CRC over the file with the checksum field zeroed

The algorithm is CRC-32/ISO-HDLC: reflected polynomial `0xedb88320`, initial state
`0xffffffff`, and final complement. The checksum field at bytes 108–111 is treated
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

## Preserve input bytes and distinguish invalid input from corruption

`nbg_checksum_verify` accepts stable, readable input from 592 bytes through 256 MiB.
It checks the NBG magic and supported version, but leaves complete container
validation to [nbg_model_open](nbg-model.md). Input may be unaligned. The writable
result must not overlap the file.

A match or mismatch returns both the stored and computed checksum. Invalid
pointers, sizes, output overlap, magic, or version leave the result unchanged.
These checks cannot establish whether an arbitrary pointer names readable memory.
A stored zero is compared normally; zero never disables verification. A generated
fixture whose computed CRC is zero verifies that behavior.

The implementation processes each byte with two four-bit table steps. It reserves
64 bytes of read-only table storage and uses no heap, mutable static state,
initialization lock, or device interface. Work is linear in file size.

The assembler checks integrity on each requirements or assembly call because it
does not retain an authenticated file identity between calls. Callers keep the
file and decoded view immutable during use. A checksum mismatch prevents all
caller-buffer writes and leaves the result unchanged.

## Focused checks cover the verifier and its assembly requirement

One ASan/UBSan check passed both published models, a generated zero-CRC file,
unaligned input, 64 corruption cases, and 18 invalid-input cases. Corruption cases
cover header metadata, stored checksum bytes, every nonempty section's first and
last byte, inter-section padding, and the final file byte. The checks also cover
zeroing a nonzero stored checksum, output overlap, and invalid address ranges.

One isolated SDK comparison passed the published models and generated zero-CRC
file, with a five-second and twenty-million-instruction limit per call. The SDK
loader and device submission path were not executed. The generated file checks
the CRC's zero value; it is not a structurally valid executable model.

The affected assembly sanitizer check passed the full fixture and 107 refusal
cases. Existing malformed-profile cases receive repaired checksums so that they
continue to exercise assembly validation. Added cases corrupt the checksum,
coefficient payload, and final file byte. The earlier complete SDK-byte comparison
remains relevant: command construction and address encoding are unchanged, and
the original model passes the added integrity requirement.

The AArch64 verifier object contains 376 bytes of text and read-only data, no
writable static data, no external references, and no stack frame. Its SHA-256 is
`dbc5502536d737ebab602218ea0eaefaf320a4f201626bcd7183822663b6e069`.
The isolated target is `nbg-checksum`. The affected assembler object also builds;
its current identity and resources are in [nbg-assembly.md](nbg-assembly.md).

The checks are [nbg-checksum.c](../checks/nbg-checksum.c),
[nbg-checksum-reference.py](../checks/nbg-checksum-reference.py), and the affected
[assembly check](../checks/nbg-assembly.c). The Python check accepts the pinned SDK,
both published models, a host shared library containing the verifier, and a new
output directory. It writes the zero-CRC fixture used by the C check. Raw results,
commands, artifacts, and input identities are under
`~/.cache/sunstoneos/evidence/nbg-checksum-20261016/`.

Descriptor access bounds, SRAM mapping, command semantics, and physical inference
remain outstanding. No board request, kernel rebuild, or unchanged integration
suite was run for checksum verification.
