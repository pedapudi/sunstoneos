# Validate encoded NPU circular-buffer ranges

Native assembly now validates complete encoded circular-buffer ranges before
writing caller buffers. The implementation is **host-tested** against the pinned
model and isolated SDK field packing. It does not establish that hardware accesses
or prefetch stay within those ranges. Physical execution remains unqualified.

This document assumes familiarity with address ranges and binary descriptors.
A circular buffer has a byte length and an exclusive end address. Its first
address is `end - length`; the end itself is outside the readable range.

## Pair tensor addresses with their encoded rings

The [assembler](nbg-assembly.md) permits descriptor patches only at the observed
weight, input, output, output-ring-end, and input-ring-end fields. Tensor pointers
use full 32-bit addresses; weights and ring ends use addresses shifted by six.
Input pointers may name the input tensor, working pool, or internal SRAM. Output
pointers may name the output tensor, working pool, or SRAM. Weight pointers must
name a coefficient-table entry.

The low 26 bits of descriptor words 18 and 20 encode output and input ring lengths
in 64-byte units. Words 19 and 21 hold the corresponding exclusive ends in the
same units. For each nonzero length, assembly requires:

- An end relocation and a tensor-address relocation for the same source allocation.
- A complete range from `end - length` through `end - 1` inside that allocation.
- The tensor address within that range.
- A representable, 64-byte-aligned exclusive end.

A zero length requires an unpatched end field containing the observed disabled
sentinel, `0x03ffffff`. A missing tensor pointer, unknown destination, duplicate
pointer field, mismatched source, underflow, or range overflow refuses the entire
assembly. Source identity is checked explicitly even during requirements planning,
when temporary allocation addresses may coincide.

The address encoder's `NBG_ADDRESS_END_SHIFT6` mode takes the start and length of
a nonempty span. After validating the span, it writes the exclusive end shifted
by six and preserves the upper six destination bits. The end may equal the
allocation end. An end of `2^32` is refused because it cannot be represented in
the descriptor, even when the preceding data fits in the address space.

The pinned model contains 105 input rings and 102 output rings. All 207 complete
ranges fit their declared sources. Twenty input descriptors and 23 output
descriptors use the disabled-ring form. Linear tensor accesses and the 77 weight
references still use one-byte address checks; their complete access extents and
prefetch requirements remain unresolved.

## SDK packing confirms field locations without proving execution semantics

The inspected library is `libVIPhal.so` from the
[pinned A733 SDK](https://github.com/ZIFENG278/ai-sdk/tree/fc90006d0f6569da2f6726c2d8395877686f5aca).
Its SHA-256 is
`3ed5357b26bd6c4fb68fbdc0b21d637227dda711d1fe67b9e28accdc48bb11f2`.
The exported transpose helper calls a leaf encoder at ELF offset `0x4620`.
That encoder converts a 184-byte parameter record into a 192-byte descriptor.

The instruction comparison invokes only offsets `0x4620` through `0x497c`.
It supplies synthetic parameters, restricts instruction execution to that range,
and allows at most 512 instructions or one second per call. No library initializer,
Linux call, allocation service, or device operation is provided.

The encoder copies tensor addresses from parameter offsets `0x20` and `0x48`.
It shifts input ring length/end at `0x88`/`0x8c` and output length/end at
`0x90`/`0x94` right by six into the fields used by native validation. This agrees
with Mesa's candidate ring layout. The synthetic records establish packing;
they are not complete executable workloads.

The HAL encoder sets bit 1 of descriptor word 31. The model's 48 descriptors
without coefficient relocations share that selector. Its 77 weight-bearing
descriptors clear it and contain `0x07ffffff` in word 35; the other descriptors
have zero there. The selector correspondence is a useful instruction-family
distinction, but word 35's meaning and full family-specific semantics remain unresolved.

## Focused checks preserve the complete saved submission

One encoder ASan/UBSan check passed 24 parameter-refusal cases, existing encoding
and flag cases, full-batch bounds, and the added exclusive-end boundaries.
One assembly sanitizer check passed the complete fixture and 120 refusal cases.
Added cases cover ring underflow, overflow, missing or disabled end fields,
source mismatch, tensor addresses outside a ring, and unsupported pointer fields.
A separate valid case places a ring end at the source allocation's end.

Fifteen isolated HAL comparisons passed across SRAM-like, ordinary, high, and
near-limit device addresses. They compare complete descriptor bytes after native
address encoding and check output guards. One integration comparison also passes
all 534 descriptor relocations, 125 command-pointer relocations, and complete
allocations against the saved SDK submission after address/event adaptation.
The full SDK loader session was not repeated.

Both affected AArch64 objects build. The address encoder contains 428 bytes of
code and has no external references. The assembler contains 5,524 bytes of text
and read-only data. Its per-operation frame is 528 bytes, reduced from 1,872 by
limiting relocation scratch to the five recognized pointer fields. Other caller
and helper frames are additional. Current object identities are recorded in
[nbg-relocation.md](nbg-relocation.md) and [nbg-assembly.md](nbg-assembly.md).

The checks are [nbg-ring-reference.py](../checks/nbg-ring-reference.py),
[nbg-relocation.c](../checks/nbg-relocation.c), and the existing assembly checks.
The Python field comparison accepts the pinned HAL library, a host shared library
containing the native encoder, and a new output directory. Raw evidence, range
inventory, disassembly, commands, and hashes are under
`~/.cache/sunstoneos/evidence/nbg-rings-20261016/`.

Unchanged kernel, controller, service, DMA, and mimus suites were not repeated.
No board request occurred. Ring geometry validation is one prerequisite for
admission; it cannot authorize submission on its own.
