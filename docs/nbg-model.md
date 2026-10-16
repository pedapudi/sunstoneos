# Native decoding of compiled NPU containers

The native C decoder is **implemented and host-tested** for NBG format
`0x1001e` and the 32-bit-address form of `0x20000`. It exposes bounded views
without allocating memory or invoking drivers. The older format reproduces
the saved SDK command-buffer reference. The compiled convolution's directory
records match the saved SDK layout evidence. The
[native assembler](nbg-assembly.md) now consumes these views for the observed
neural-operation profile. Command admission and physical inference remain outstanding.

This document assumes familiarity with binary formats and borrowed memory.
A borrowed view refers to caller-owned model bytes; the decoder does not copy
the complete file. The interface is [nbg_model.h](../include/nbg_model.h),
implemented in [nbg_model.c](../drivers/npu/nbg_model.c).

## Validate structure before publishing a view

The decoder limits files to 256 MiB. Fixed-header files require at least 592
bytes; directory files require their complete header, directory, and declared
payloads. It reads little-endian fields explicitly and permits unaligned input.
A successful view has 40 section slots; absent sections have zero length.
The older format populates its original 27 slots. Validation covers:

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
compatibility. The separate [checksum verifier](nbg-checksum.md) checks
complete-file integrity for `0x1001e` and payload/section integrity for the
supported `0x20000` layout. The assembler still refuses `0x20000`. Names preserve the original 64 bytes and add a terminating NUL;
they are not checked for valid UTF-8. Signed fields are decoded without
relying on an out-of-range unsigned-to-signed conversion.

## Validate directory records before decoding their payloads

Directory files require one 12-byte subheader and at most 40 unique section IDs.
The 44-byte section records must have bounded offsets and lengths, valid entry
sizes, and zero address/reserved fields. The declared total must equal the
supplied file length. Header, subheader, directory, and nonempty payload ranges
must not overlap. Directory order and file placement are independent of section IDs.

The [directory reference](nbg-directory.md) establishes the wire layout. Tensor
records occupy 112 bytes; 32-bit-address patches occupy 28 bytes. Accessors
normalize the quantization union and set absent high-address patch fields to zero.
Forty-bit-address directory files remain unsupported. Debug and customer sections
are retained as raw spans; their internal semantics are not validated.

The network-information section supplies tensor/layer counts, command sizes,
core count, and version-specific flag bits. The decoded operation count comes
from the operation section's validated size. The ambiguous count at network byte
80 remains visible through `NBG_NETWORK_INFO`; it does not control record reads.
Memory-pool and SRAM requirements are optional bounded scalar sections. Absent
requirements are zero. Directory pool alignment uses the SDK's 64-byte allocation
convention; the file does not contain an alignment field for this requirement.

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

The directory-specific ASan/UBSan check passes the compiled convolution and
84 atomic refusals. It covers truncated allocations, malformed subheaders and
sections, overlap, duplicate IDs, record widths, scalar memory requirements,
quantization, reference bounds, and accessor aliases. A relocated, reversed
directory also passes. The same check confirms that the assembler refuses the
newly decoded format without changing its result.

One comparison with the saved directory reference matches thirteen sections,
two operations, five patches, two tensors, and three payload ranges. Its initial
comparison treated the serialized source index as unsigned; converting the
`0xffffffff` sentinel to the native signed value resolved that check failure.
Only that comparison was repeated. Native code and its successful checks were unchanged.

The shared view grew, so the existing assembly sanitizer check was also run and
passed the full fixture and 120 refusal cases. One native assembly comparison
matches all six saved SDK buffers, including 534 descriptor and 125 command
relocations. It verifies the capture's hashes before reuse. No SDK loader,
compiler, simulator, kernel, or application suite was executed again.

The AArch64 decoder object contains 10,816 bytes of text and read-only data,
no writable static data, and references only `memcpy` and `memset`. Its SHA-256 is
`bc739fb8d203f99c4152b6f4ebdacce81489844ccf41872490d1caf452956cc9`.
The 64-bit view occupies 480 bytes. Disassembly shows a 736-byte open frame,
192-byte directory-header frame, and 208-byte tensor frame. The open-to-tensor
path uses 944 bytes before memory-helper stack use. The isolated CMake target
is `nbg-model`; the affected assembler target also builds.

The checks are [nbg-model.c](../checks/nbg-model.c),
[nbg-directory.c](../checks/nbg-directory.c), and
[nbg-directory-native.py](../checks/nbg-directory-native.py). The directory
comparison accepts the pinned model, native shared library, saved reference
JSON, and a new output directory. Current commands, results, artifacts, and
input identities are under
`~/.cache/sunstoneos/evidence/nbg-directory-native-20261016/`.
Earlier fixed-format evidence remains under
`~/.cache/sunstoneos/evidence/nbg-model-20261016/`.

The [native assembler](nbg-assembly.md) resolves caller-owned allocations and
assembles the observed command profile from these views. Descriptor access extents, device padding,
relocation destinations, software-operation semantics, command permissions,
and SRAM placement remain separate admission requirements. Directory checksum
verification now has host evidence. Zero-pool assembly remains required before
the numerical fixture can reach execution admission.
The assembler adapts the completion event and reserves the controller's full fetch span.
