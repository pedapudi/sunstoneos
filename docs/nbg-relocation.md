# Checked address relocation for compiled NPU models

The native address encoder is **implemented and host-tested**. It patches
resolved 32-bit device addresses into an offline command buffer. It is a
component of model loading; complete NBG loading and physical inference remain
unimplemented or untested. This document assumes familiarity with binary formats,
device addresses, and memory bounds.

The interface is [nbg_relocation.h](../include/nbg_relocation.h), implemented in
[nbg_relocation.c](../drivers/npu/nbg_relocation.c). A relocation supplies a
destination byte offset, source allocation address and length, source offset,
referenced access length, and encoding. These are resolved loader inputs, not
serialized NBG records. The loader must derive access lengths from the device
operation, including padding and prefetch. Logical tensor dimensions alone are
insufficient.

## Validate the entire batch before changing the buffer

The encoder accepts one through 4,096 relocations. Every destination is a
four-byte little-endian word at a four-byte offset. The CPU buffer may be
unaligned. The supported encodings are:

| Encoding | Written value | Required address alignment |
| --- | --- | --- |
| Full word | Resolved device address | One byte |
| Shifted address | Existing bits 31:26 followed by address bits 31:6 | 64 bytes |
| Address with flags | Aligned address with existing bits 5:0 | 64 bytes |

The complete source allocation must lie in the 32-bit device address space,
with a nonzero base and length. The requested access must have nonzero length
and fit within that allocation. Destination words must fit within the CPU
buffer. Unknown encodings, duplicate destinations, wrapping CPU ranges, and
overlap between metadata and the destination buffer are refused.

Validation precedes all writes, so any refusal leaves the buffer unchanged.
The caller must keep the buffer and relocation array stable and exclusively
owned throughout the call. Duplicate detection uses at most 8,386,560 pair
comparisons at the maximum batch size. No heap allocation, cache operation,
device access, or locking occurs. AArch64 stack use is limited to compiler
register spills; the implementation has no temporary batch array.

The encoder does not resolve NBG source kinds, establish device mappings,
interpret command semantics, or authorize submission. It cannot prove that a
caller-supplied access length covers the actual device operation. Split high
address fields and wider device addresses are unsupported. Device addresses
remain distinct from the 40-bit physical addresses supported by the NPU MMU.

## Reference instructions establish the supported encodings

The inspected SDK library is
`viplite-tina/lib/aarch64-none-linux-gnu/v2.0/libNBGlinker.so` from
[the pinned A733 SDK](https://github.com/ZIFENG278/ai-sdk/tree/fc90006d0f6569da2f6726c2d8395877686f5aca).
Its SHA-256 is
`82f049b0ed0065dd4d443e37eeb1edfcbef24457c9c24e36170d64d5b748ca66`.
The memory-pool relocation routine starts at ELF virtual address `0xb830`.
The SRAM routine starts at `0xb920`; its source-kind-eight branch supplies the
internal-SRAM base. Both use a source offset directly for format `0x1001e`.
Encoding one preserves the upper six destination bits and shifts the resolved
address right by six; encoding zero writes the full low word.

The reference check maps the pinned ELF in Unicorn and invokes only these
routines with synthetic memory. An instruction hook rejects execution outside
their allowed ranges and the mask-bit-position helper. Each call has a
512-instruction limit. There is no dynamic loader, HAL implementation, system
call interface, or physical device connection. The library remains an external
research input and is not linked into the OS.

The SDK also has branches for older format versions and high address fields.
Those branches do not establish support in the native encoder. The comparison
uses only format `0x1001e` and 32-bit device addresses.

## Focused evidence and remaining loader work

One updated ASan/UBSan host check passed encoding examples, all low-six-bit flag
patterns, source and destination
bounds, guards, a full 4,096-entry batch, and atomic refusal with an invalid
last entry. Twenty parameter-refusal cases check unchanged destination bytes;
additional cases cover null pointers, wrapping CPU ranges, and metadata aliases.

The initial isolated reference comparison passed all 241 memory-pool and 215 internal-SRAM
records in the pinned ShuffleNetV2 fixture. It also passed 264 constructed
cases covering address carry, the final device byte, address alignment, and
all upper-six-bit patterns. Each comparison checks the entire destination
buffer against the SDK instruction result.

These comparisons use synthetic destination contents and allocation bases.
They establish address encoding for those records, not operation placement,
access sizes, tensor binding, or a runnable model. The subsequent
[complete loader trace](nbg-loader-trace.md) resolves all 659 active relocations,
including command pointers, weights, and deferred tensor bindings. Its full-buffer
comparison passes at two allocation bases. The model's one initialization
relocation is omitted by the SDK and remains explicit in the report.

One AArch64 object build passed. The current object contains 400 bytes of code, no
static data, and no undefined symbols. Its SHA-256 is
`beb793b3f796b45e12af05fd9997012aa268cc150a6424a0ad8c4dedaed744f4`.
Current native and complete-loader evidence is under
`~/.cache/sunstoneos/evidence/nbg-loader-20261016/`.
The initial isolated comparison remains under
`~/.cache/sunstoneos/evidence/npu-relocation-20261016/`.
Existing kernel, service, DMA, and application evidence was reused; no board
request or kernel rebuild occurred.

The focused checks are [nbg-relocation.c](../checks/nbg-relocation.c) and
[nbg-relocation-reference.py](../checks/nbg-relocation-reference.py). The Python
check takes the pinned SDK library, pinned model, and a host shared library
built from the native encoder. Its host dependencies are pinned in
[host-requirements.txt](../checks/host-requirements.txt).
The isolated AArch64 CMake target is `nbg-relocation`.

The next loader work must map every serialized patch to an owned command or
state allocation, resolve source kinds and allocation lengths, and validate
every command before publication. Internal-SRAM mapping, output
padding, dynamic command registration, and numerical inference remain required.
