# Native decoding of compiled NPU containers

The native C decoder is **implemented and host-tested** for NBG format
`0x1001e`. It exposes bounded views of a compiled model without allocating
memory or invoking drivers. Its decoded records reproduce the saved SDK
command-buffer reference through the host reconstruction. Native model assembly,
command admission, and physical inference remain outstanding.

This document assumes familiarity with binary formats and borrowed memory.
A borrowed view refers to caller-owned model bytes; the decoder does not copy
the complete file. The interface is [nbg_model.h](../include/nbg_model.h),
implemented in [nbg_model.c](../drivers/npu/nbg_model.c).

## Validate structure before publishing a view

The decoder accepts files from 592 bytes through 256 MiB. It reads little-endian
fields explicitly and permits an unaligned input buffer. A successful open
returns the header and all 27 section descriptors. Validation covers:

- NBG magic and version, supported neural and tensor command sizes, and
  power-of-two working-pool alignment.
- Section bounds, nonempty section overlap, known record strides, and header
  counts for tensors, layers, and operations.
- Offset/length entries for coefficient headers, coefficients, programmable
  instructions, dynamic I/O, load states, and instruction data.
- Tensor dimensions, element formats, logical byte counts, and quantization.
  Size calculations refuse overflow before multiplying. Asymmetric scales
  must be positive finite binary32 values; positive subnormals are accepted.
- Operation state and relocation ranges, known neural/tensor command indices,
  layer indices for known operation kinds, hardware-initialization references,
  and input/output tensor indices in relocations.

Complete validation precedes publication of the view. Invalid input and
unsupported layouts leave the caller's result unchanged. The result cannot
overlap the input file. Validation time is linear in the record count, plus
a fixed comparison of section ranges; there is no heap allocation.

Unknown operation kinds, relocation sources, and relocation kinds remain
available as raw values. Undecoded sections remain byte spans. Later loader
policy must reject unsupported execution modes explicitly. Successful decoding
does not establish that an unknown operation can execute.

The header retains target, feature flags, core count, memory requirements,
and stored checksum. The decoder does not verify checksum or hardware
compatibility. Names preserve the original 64 bytes and add a terminating NUL;
they are not checked for valid UTF-8. Signed fields are decoded without
relying on an out-of-range unsigned-to-signed conversion.

## Keep the file and view immutable while using accessors

The caller must retain an immutable file and an unchanged successful view until
all accessors and returned spans are finished. Concurrent mutation is outside
the interface contract. The decoder performs no locking, cache maintenance,
device mapping, or memory protection.

Accessors return sections, decoded tensors, operations, relocations, and table
entries resolved to their payload spans. They check requested indices and
ranges before reading. Refusal leaves output unchanged. Output cannot overlap
the view or its file. These checks cannot validate an arbitrary CPU pointer
or prevent a caller from violating the immutability contract.

Tensor results distinguish logical bytes from device allocation size. They
preserve scale bits directly, so decoding requires no floating-point runtime.
Operation auxiliary words and relocation transformation fields remain visible
for contextual interpretation. A transformed neural command pointer differs
from a transformed data address, as shown by the
[complete loader trace](nbg-loader-trace.md).

## Focused evidence includes recorded SDK output

One ASan/UBSan check passed the pinned ShuffleNetV2 fixture, an unaligned input,
atomic refusal, accessor output preservation, name termination, signed fields,
logical-size boundaries, all six table types, and empty payload spans.
It exercises 635 refusal cases, including every shorter-than-header prefix.
A deterministic campaign of 512 mutations changes header, operation, or patch
bytes and selected file lengths. Accepted mutations are traversed through the
accessors; refused mutations preserve the output view. This is bounded mutation
coverage, not exhaustive fuzzing or command-safety verification.

One integration check uses the native decoder for all sections, operation and
patch records, tensor metadata, and 186 table entries. The host reconstruction
then applies the native address encoder and matches the previously captured
SDK descriptors, command buffer, and coefficient payload. The capture uses
device addresses starting at `0xe0000000` and includes 659 active relocations.
The SDK is not executed during this check. Its earlier reference evidence is
reused because the SDK, model, feature responses, and capture contents are unchanged.
The check verifies capture files against the existing hash manifest before reuse.

One AArch64 object build passed. The object contains 6,699 bytes of text and
read-only data, no writable static data, and references only `memcpy` and
`memset`. Its SHA-256 is
`611e1dc51e1b5d93b792192fbb3c2483f6d4026b6b9ab6bf9f81e3aaaa1ab60d`.
The 64-bit view occupies 376 bytes. Disassembly shows a 768-byte open frame
and a 240-byte tensor-decoding frame, for 1,008 bytes along that call path,
excluding memory-helper stack use. The isolated CMake target is `nbg-model`.

The checks are [nbg-model.c](../checks/nbg-model.c) and
[nbg-model.py](../checks/nbg-model.py). The Python check accepts the pinned model,
a host shared library containing the decoder and address encoder, an existing
successful SDK capture directory, and a new output directory. The capture's
existing `manifest.json` must be in its parent directory. Results
and input hashes are under `~/.cache/sunstoneos/evidence/nbg-model-20261016/`.
No board request, kernel rebuild, or unchanged integration suite was run.

The next native loader step is to resolve owned allocations and assemble
commands from these views. Descriptor access extents, device padding,
relocation destinations, software-operation semantics, command permissions,
checksum verification, and SRAM placement remain separate admission requirements.
The controller's completion event and fetch reservation must also be respected.
