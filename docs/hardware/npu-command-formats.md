# NPU command formats of compiled graphs

Status: **work in progress; host evidence only.** This document records what the SunstoneOS loader needs to
know about the records inside a compiled network binary (directory format `0x20000`): what each field of
the stream-processor, activation and two-input records means and which memory it addresses. It is written
in terms of the format, not of any application's graph. The vendor kernel driver submits command buffers
that user-space code builds and never interprets them, so the facts here come from the compiled artifacts.

## How a field is classified

| Class | Meaning |
| --- | --- |
| **Confirmed** | A mutation of the field in a copy of a graph, with the checksums repaired by `tools/npu-nbg-mutate.py` and the copy run in the SDK simulator, changed the output as the stated meaning predicts |
| **Named** | The compiler's own patch table names the field as an address (destination offset, source region and index) |
| **Inferred** | Differential compilation (one change per variant, `tools/npu-graph-variants.py`) changed the field consistently with the stated meaning; no mutation has confirmed it |
| **Unknown** | Seen in graphs; meaning not established. The loader admits it without interpreting it; the page tables confine what it can reach |

Variants are compiled by the pinned compiler offline, from model descriptions that this repository holds
(`tools/npu-graph-variants.py`). The compiled graphs are not committed.

## Prior art

The register database of `etna_viv` (`rnndb/state_3d.xml`, 368 registers) names the older neural-network
engine's registers such as `NN_INST_ADDR` at `0x010A0`. It defines none of `0x0e4c`, `0x0e5a` to `0x0e5d`,
`0x0428`, `0x0429`, `0x5580` or `0x4d800`. The pinned Mesa tree holds only `etnaviv_ml_nn.c` and its header,
which build the 192-byte neural-network descriptor and say nothing of stream-processor programs, activation
records or shader state. The vendor driver in `vip2/` and the SDK headers name the operation kinds
(`tools/inspect-nbg.py`) and patch sources, and nothing else. The shader instruction set of Mesa
(`src/etnaviv/isa`) is not yet compared with the shader instruction section.

## Operation record

Each operation of kinds 2, 6, 7 and 8 has a 192-byte descriptor (48 words; word `n` is at byte `4n`) in the
neural-network section and a load-state stream in the load-states section. The command stream the engine
runs is the concatenation of the operations' load-state streams. A patch of kind 1 relocates a descriptor
word; a patch of kind 0 relocates a word of the load-state stream (the operand of `0x08010428`, which holds
the address of the descriptor).

### Load-state stream

All words are pairs of a register header and an operand.

| Item | Kinds | Class | Meaning |
| --- | --- | --- | --- |
| `0x08010e4c`, operand | 2, 6, 7, 8 | Unknown (bit 12 inferred) | Observed operands `0x0493e040`, `0x04b3e040`, `0x04b3f040`. Bit 12 is set exactly when the descriptor has a second input (every two-input variant and operation observed). The other bits vary between graphs without a stated cause |
| `0x08010e5a`, `0x00451440` | all | Unknown | Constant in every graph |
| `0x08010428`, operand | all | Named | Address of the operation's descriptor (device address of the neural-network allocation plus 192 times the index), low six bits kept |
| `0x08010429`, operand | all | Unknown | Zero in every graph |
| `0x08010e5b`, operand | 6 | Inferred | Position of the operation's program in a 256-entry program memory: the number of instructions of the preceding stream-processor operations modulo 256 (2903 of 2903 operations of twelve graphs, including programs that wrap past 255). The loader verifies it |
| `0x08010e5c` operand, `0x08010e5d` operand, repeated | 6 | Inferred structure; contents Unknown | One 64-bit instruction of the stream-processor program per pair, low word then high word. Zeroing every operand makes the simulator abort, so the program is consumed. The effect of one bit is known for none (the bit that differs between an addition and a subtraction program has no effect in the simulator) |

### Descriptor

| Word | Kinds | Class | Meaning |
| --- | --- | --- | --- |
| 5 (byte 20) | 2, 6, 7 | Named | Coefficient or table address (patch source 5): weights (kind 2), activation table (kind 7), constants (kind 6) |
| 6 (byte 24) | all | Named | First input address |
| 7 (byte 28) | all | Named | Output address |
| 19 (byte 76) | all | Named | End of the output ring when the output is a ring |
| 21 (byte 84) | all | Named | End of the first input ring; the disabled sentinel `0x03ffffff` in the low 26 bits when unpatched |
| 34 (byte 136) | 2, 6 | Named | Second input address |
| 35 (byte 140) | 2, 6 | Named | End of the second input when it is in VIP SRAM; the low 26 bits are `0x03ffffff` (disabled) when it is not patched |
| 27, bit 29 | 6 (add/subtract) | **Confirmed** | Set: the stream-processor operation subtracts the second input from the first. Clear: it adds. Setting the bit in an addition graph made the simulator output equal the subtraction graph's output to 0.003, as predicted; the zero-to-one change of the program bit alone did not change the output |
| 1, 3 | all | Inferred | `(width / 4) << 8` ORed with a small code, where width is the number of elements in a row (0x2c for 176, 0x10 for 64) |
| 16 | all | Inferred | `(row bytes times rows) << 16` ORed with the row bytes (`0x160` for 176 sixteen-bit elements) |
| 17 | all | Inferred | Row bytes of a 32-bit row (`0x2c0` for 176 elements) |
| 18 | all | Inferred | Low byte: the row width in elements (`0xb0` for 176, `0x40` for 64) |
| 29, 30 | 6, 7 | Inferred | Output range limits as IEEE binary32: the largest and smallest value of the 16-bit fixed-point output (`0x407ffe00` is 3.9998 at 13 fractional bits, `0x417ffe00` is 15.999 at 11) |
| 2 (top nibble) | all | Inferred | Changes with the input fixed-point position (`0x9` to `0xa` when the input scale is multiplied by four) |
| 15 | 7 | Inferred | Activation class: `0x30200000` for a sigmoid, `0x30000000` for a hyperbolic tangent, `0x30380000` for a rectifier and for the swish (these two share the header; the function is the table) |
| 0, 4, 8, 13, 14, 20, 22, 23, 28, 31, 32, 33 and the rest of the nonzero words | all | Unknown | Constants or values that depend on shape and placement without a stated rule |
| Remaining words | all | Unknown | Zero in every graph examined |

### Record counts at this revision

| Record | Confirmed | Named | Inferred | Unknown |
| --- | ---: | ---: | ---: | ---: |
| Load-state stream, kind 6 (six item types) | 0 | 1 | 3 | 2 plus the program contents |
| Load-state stream, kind 7 (four item types) | 0 | 1 | 0 | 3 |
| Descriptor, 48 words | 1 (a bit) | 7 | 9 | 14 nonzero, 17 always zero |
| Second input (words 32 to 35 and the `0x0e4c` bit) | 0 | 2 | 2 | 1 |
| Shader state (576 to 816 bytes) | 0 | 0 | 0 | all |

## Second input

An operation with a second input has patches at descriptor word 34 and, when the input is in VIP SRAM, word
35, and `0x0e4c` bit 12 set. Observed sources of the second input: a graph input (patch source 1; an
element-wise multiplication compiles to a neural-network operation, an addition or subtraction to a
stream-processor operation, a batched matrix product to neural-network operations that take both operands from
graph inputs), VIP SRAM (source 8), and the dynamic-input section (source 6, stream-processor operations).

Rules that hold in all 441 operations of five layer graphs and the single-operation graphs, and that the loader
verifies (class **Inferred**; they only refuse):

| Case | Word 33 | Word 35 (low 26 bits) | Pointer words |
| --- | --- | --- | --- |
| No second input | 0 | not examined | word 34 below `0x1000` |
| Second input from a graph input or the dynamic-input section | 1, or 0 for a batched matrix product | `0x03ffffff` (disabled) | word 34 patched |
| Second input in VIP SRAM | ring size in 32-byte units (**confirmed**, below) | the ring limit, patched as an end address | word 34 patched; the start lies in [limit - size, limit) |

**Ring unit, confirmed by mutation.** In a graph whose second operand is a result in VIP SRAM (an addition of a graph input and
the product of two others, 16 rows of 176 elements), word 33 was set to 88 and the ring limit moved to 2816 bytes after the
start, half the tensor. The simulator output was unchanged in the first half and, in the second half, equal to the
prediction that the read wraps to `limit - 88 x 32`, the start, and so reads the first half again (largest difference to
the prediction 0.002, the error of the unmutated graph; the unwrapped values differ by 6.9). With 64-byte units the
wrap would have gone to another place. So the unit is 32 bytes, and the limit is the wrap point.

Words 32 and 33 describe the second input as words 16 and 17 describe the first (for graph inputs the upper half
of the byte-count word never exceeds the tensor's size in bytes, in 58 of 58 operations). An operation reads its
inputs from rings and writes its own: no input range overlaps the output range in VIP SRAM for kinds 6, 7 and 8
(534 of 534 first inputs) or for any second input (54 of 54); a neural-network operation of the published
ShuffleNetV2 graph overlaps its first input with its output in 7 of 535 operations, which the loader still admits.
Two graph inputs of one operation have the same type and size (fixed-point positions may differ).

Findings from variants that bear on the loader:

- The epsilon of a layer normalization (`1e-5`, `1e-3`, `1e-7`) changes no byte of the compiled graph, not the
  program, the descriptor or the dynamic-input section. The compiler fixes it.
- The weights and bias of a layer normalization are in the dynamic-input section.

## Not yet examined

Shader state and the shader instruction section; the batched matrix product, the gated-linear-unit gate and the
other two-input forms; the extents of the second input; the meaning of the program words. Each needs
variants and mutations as above before the loader interprets it.
