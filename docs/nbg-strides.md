# NPU simulator stride interpretation

This is **research evidence**. Hardware access validation remains incomplete. The pinned
compiler's simulator selects its revision-9.2 neural decoder for the A733
target. Isolated instructions from that decoder establish channel-stride
packing and limited address arithmetic. Full access extents remain unresolved.

The reader is assumed to understand binary descriptors, byte offsets, and
instruction emulation. Descriptor words below are zero-based little-endian
32-bit integers. A channel stride is the address difference between adjacent
image channels in the applicable layout.

## The target selects the revision-9.2 decoder

The [compiler image](npu-compiler.md) contains `libEmulator.so` at
`/root/Vivante_IDE/VivanteIDE5.11.0/cmdtools/vsimulator/lib/`. Its SHA-256 is
`2d102725286968ffa4b011dfb7f92ca9f4434a71fd19920beaf4045923bbac6a`.
The library was copied from a stopped container; extraction did not start it.

Its feature lookup accepts model `0x9000`, revision `0x9202`, product
`0x05090009`, customer `0x08000000`, and target `0x1000003b`. The isolated
lookup returns the matching record at ELF virtual address `0x5432e0`.
The decoder predicates return true for revision 9.2 and false for revision 9.
Static inspection of the simulator's constructor and execution dispatch
connects those predicates to `cmoNeuralNetV92`.

The relevant ELF addresses are:

| Function | Address | Evidence |
| --- | --- | --- |
| Feature lookup | `0xc99c6` | Isolated execution against the embedded table |
| Revision-9.2 predicate | `0xc9e24` | Isolated execution with the returned record |
| Revision-9 predicate | `0xc9e52` | Isolated execution with the returned record |
| Descriptor decoder | `0x1583e6` | Static inspection; selected arithmetic blocks executed |
| Input address calculation without SRAM | `0x15985c` | Selected branch executed with synthetic state; stopped before memory helper |
| Output address calculation | `0x16bb66` | Isolated execution with synthetic state |

## Channel strides combine two descriptor fields

The decoder combines these halves directly:

| Value | Low 16 bits | High 16 bits |
| --- | --- | --- |
| Input channel stride | Word 16, bits 16–31 | Word 26, bits 0–15 |
| Output channel stride | Word 8, bits 16–31 | Word 26, bits 16–31 |

The block at `0x1591ee` through `0x15921a` performs this combination without
multiplying by row stride. The older decoder multiplies the corresponding
fields, which explains why adopting its interpretation would overestimate
the first saved A733 descriptor's channel spacing.

For the saved convolution, input row/channel strides are 8 and 64;
output row/channel strides are 6 and 36. For the first saved ShuffleNet
descriptor, they are 224 and 50,176 for input and 112 and 12,544 for output.
The low row-stride fields are word 16 bits 0–15 and word 17 bits 0–15.
Additional row-stride bits exist in the extended format; their activation
conditions have not been verified by this check.

With synthetic byte-element, non-interleaved state, the exercised input
branch calculates base plus channel times channel stride, plus row times
row stride, plus column. The convolution's final input coordinate produces
offset 191. The output leaf adds a caller-supplied channel offset to the base
and multiplies the row/column contribution by an interleave factor.

These expressions do not establish complete accesses. The input branch was
selected synthetically: its controlling feature is zero in the returned target
record. The target's general input path, SRAM routing, offsets, pooling,
transpose operations, coefficient streams, burst alignment, and prefetch
still require analysis. Output callers must also be traced to establish
how they calculate channel offsets and apply circular-buffer wrapping.

## No host check preserves the interpretation

No host check covers the stride interpretation. A bounded one-time comparison pinned the simulator and
both saved model hashes and confined execution to selected instruction ranges
with time and instruction limits. It checked the target lookup and predicates,
two saved descriptor stride pairs, and two synthetic pairs with nonzero high
halves. Address samples used synthetic state for the two saved descriptors.
No inference, external-memory helper, or device access executed. The simulator
feature field is zero for the target and does not represent the board's SRAM
capacity.

Raw extraction records, disassembly, the check copy, and results are under
`~/.cache/sunstoneos/evidence/nbg-access-research-20261016/`.
The passing result is `stride-check-corrected/comparison.json`.
Native assembly, inference, and performance checks were not repeated.

Results of host checks that no longer exist are kept in the evidence record [nbg-strides.jsonl](evidence/nbg-strides.jsonl).
