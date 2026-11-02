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
(`tools/inspect-nbg.py`) and patch sources, and nothing else. The pinned Mesa tree holds no shader instruction set or state documentation, so the shader instruction section is not
compared with any open source and stays opaque.

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

### Unpatched pointer words

A pointer word that no patch sets (the output address of a stream-processor operation that keeps its result in the engine's
own scratch, the first-input address of an operation without an input) holds a small offset and not an address of a
buffer. The twelve graphs examined first held values below `0x1000` (words 6, 7, 20, 34). Compiled attention layers
(four heads over 92 positions, ACUITY 6.30.22, target `VIP9000NANODI_PLUS_PID0X1000003B`) hold larger ones: the softmax
chain of each layer has eight stream-processor operations with the first input and its ring patched and the output
pointer unpatched, and word 7 steps by `0x4220` (16,928 bytes, 92 * 92 * 2) from `0` to `0x1cee0`; the operations with
only a state patch step by `0xb8` (184 bytes, 92 * 2), up to `0x508`. Their word 8 is `0x42201000` and `0xb81000`, that is the
same step in the upper half. The simulator runs these graphs and returns the reference output, so the engine uses these
words as offsets in its own scratch; no source states this, and the offsets could also be addresses that the page tables
would have to map.

The loader therefore bounds them by the page tables and not by the first page. `NBG_ASSEMBLY_LOW_RESERVED` (`0x100000`,
1 MiB) is the start of the lowest device address that a buffer can have (`placed()` refuses any buffer below it), and an
unpatched pointer word is admitted when it is below that bound. If the engine did use such a word as an address, the
access would fault in the page tables (the MMU exception of the board runs in `docs/reference-backsolve-npu.md`) and the
job would fail, not read or write memory outside the job's buffers.

### Record counts at this revision

| Record | Confirmed | Named | Inferred | Unknown |
| --- | ---: | ---: | ---: | ---: |
| Load-state stream, kind 6 (six item types) | 0 | 1 | 3 | 2 plus the program contents |
| Load-state stream, kind 7 (four item types) | 0 | 1 | 0 | 3 |
| Descriptor, 48 words | 1 (a bit) | 7 | 9 | 14 nonzero, 17 always zero |
| Second input (words 32 to 35 and the `0x0e4c` bit) | 0 | 2 | 2 | 1 |
| Shader state stream, outside the uniform bank (44 operand words) | 5 | 0 | 8 | 31 |
| Shader tensor descriptor (8 words) | 4 | 0 | 1 | 3 |

## Second input

An operation with a second input has patches at descriptor word 34 and, when the input is in VIP SRAM, word
35, and `0x0e4c` bit 12 set. Observed sources of the second input: a graph input (patch source 1; an
element-wise multiplication compiles to a neural-network operation, an addition or subtraction to a
stream-processor operation, a batched matrix product to neural-network operations that take both operands from
graph inputs), VIP SRAM (source 8), the dynamic-input section (source 6, stream-processor operations) and a range of the
memory pool (source 3; the attention products of a layer of 184 frames or more, once the scores no longer fit VIP SRAM), and a graph
output that an earlier operation wrote (source 2; the residual stream that the entry graph of a tiled layer both uses and exports). The
first input may come from the same four sources, and from a graph output.

Rules that hold in all 441 operations of five layer graphs and the single-operation graphs, and that the loader
verifies (class **Inferred**; they only refuse):

| Case | Word 33 | Word 35 (low 26 bits) | Pointer words |
| --- | --- | --- | --- |
| No second input | 0 | not examined | word 34 below `0x100000` (see "Unpatched pointer words") |
| Second input from a graph input, the dynamic-input section or the pool | 1, or 0 for a batched matrix product | `0x03ffffff` (disabled) | word 34 patched |
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

## Shader operations (kind 1)

Status: **host evidence only; the simulator is the instrument.** The facts below come from 55 compiled graphs that
contain 66 shader operations and 31 distinct programs, compiled for the target of the board (`VIP9000NANODI_PLUS_PID0X1000003B`,
one shader core, 16-bit fixed-point quantization, half-precision graph tensors), and from 49 mutation checks in the SDK
simulator (`tools/npu-shader-probe.py`). The grammar below is checked against every one of the 66 operations by
`tools/npu-shader-state.py`, which also holds 33 single-change cases that break one rule each.

### Operations that compile to shader operations

The model operations were written with `tools/npu-shader-variants.py` (single operations, plus chains of two to four).

| Compiler output on the board target | Model operations |
| --- | --- |
| One shader operation | Exp, Sin, Floor, Reciprocal, Pow (exponent 3 and 0.5), Gather (constant indices on axis 0 and 1, indices from a graph input, constant table with input indices as an embedding lookup of 64 x 32, 128 x 64 and 4096 x 128), Resize (nearest and bilinear on NCHW), Tile, TopK, ArgMax, CumSum, LRN, Einsum (batched), Mod, OneHot, Round, ReduceProd, Pad of a rank-3 single-precision tensor (the relative-shift model) |
| Several operations | ScatterND (four shader operations), GatherND (one transpose and one shader operation) |
| Activation (kind 7) | Erf, Gelu in its erf form, Abs, Elu, LeakyRelu, Log, Sqrt, Softplus, HardSwish, Pow with exponent 2 |
| Stream processor (kind 6) | Neg, Clip, HardSigmoid, Sign, Expand, ReduceMean, ReduceMax, ReduceSum |
| Neural network (kind 2) | Pooling (max and average), global average pooling, convolution, Pad of an NCHW tensor |
| Transpose (kind 8) | Transpose (four permutations), Concat, Split, Slice, SpaceToDepth, DepthToSpace |
| Mixed | Gelu in its tanh form: two activation, three neural-network and four stream-processor operations. A constant-weight MatMul: two transposes and one neural-network operation |
| Not compiled | Where and GridSample (export fails); NonMaxSuppression, NonZero, RoiAlign and Unique (import fails) |

Gelu, which a BERT-class model needs, therefore does not reach the shader cores on this target: the erf form is one
activation operation, and the tanh form uses kinds 2, 6 and 7. The embedding lookup of such a model is a shader operation.
Gather with indices from a graph input is a shader operation with the table as a second graph input.

### Records

A shader operation has an operation-table record (32 bytes) of kind 1, an index into the shader table, a layer, a row of the
load-state table (its state stream) and a range of patches. Three tables lead to the program.

| Table | Entry | Rule |
| --- | --- | --- |
| Shader table (section index 8) | 8 bytes: row of the program table, row of the parameter table | The parameter row is 0 and the parameter section (index 15, four groups of three words: global offset, global scale, group size and group count per axis, as the vendor tool names them) is absent in all 66 operations |
| Program table (index 24) | 8 bytes: offset and length in the program section (index 18) | Offsets are multiples of 256, lengths multiples of 16 and the section length a multiple of 256, in 66 of 66 |
| Program section | 16-byte instructions, 11 to 124 per program | The instruction words are opaque (see Limits) |

**Confirmed by mutation (simulator).** In a four-shader chain, exchanging two rows of the program table without exchanging the
shader table rows broke both shaders; exchanging both, or moving a row to a second copy of the same program, left the output
unchanged. Changing the operation record's index alone broke the operation. The program that runs is therefore
`program[ shader_table[ operation.index ].program_row ]`. The `0x040a` operand of the state stream and the source offset of
the program patch are not read: setting the stream operand to a wrong value, or the patch offset to 768, 1280 or 65536, changed no
output byte when the table named the right program.

### State stream grammar

The state stream (load-state table, 472 to 1136 bytes) is a sequence of packets. The rules are numbered as in
`tools/npu-shader-state.py`, which raises the rule number when a stream breaks one.

| Rule | Statement | Evidence |
| --- | --- | --- |
| S1 | Packets are load-state (header bits 31:27 equal 1, bit 26 clear, operand count 1 to 64 in bits 25:16, register in bits 15:0), stall (`0x48000000` and one token word) and pad words. A load-state packet of header plus an odd number of operands is followed by one pad word; the stream length is a multiple of 8 bytes. The pad word is `0x18000000` or `0xdeadbeef` and is not read | 66 of 66. Overwriting the 18 pad words of a floor shader with `0xdeadbeef` changes no output byte |
| S2 | **Observed order, not an admission rule** (the admission rule is S2a below). The register sequence is: optional prologue (`0x5580`, `0x0e02`, stall); then the body `0x02a4 0x0420 0x0416 0x0409 0x021f 0x0424 0x040a 0x5580 0x021a 0x0425 0x0427 0x0402 (3 operands) 0x0228 0x028c 0x0e07 0x040c 0x020c 0x0201 0x02aa 0x0e22 0x0412 0x0e27 0x0240 0x0249 0x022e 0x0247 0x024b 0x024d 0x024f 0x0256 0x0257 0x0258 0x0250 (6 operands) 0x0248`; then the epilogue `0x0e03`, `0x0e02`, stall. Uniform packets (S4) may stand anywhere after the prologue | 66 of 66. The prologue is on the first shader operation of a graph and absent on the later ones (also after neural-network operations) |
| S3 | **Observed values, not admission rules** except for the two flush-related registers named in S2a. Operands that equal one value in every operation: `0x5580` = 2, `0x0e02` = `0x701`, stall token `0x701`, `0x0e03` = `0x24`, `0x0248` = `0xbadabeeb`, and `0x0201`, `0x020c`, `0x021a`, `0x021f`, `0x0228`, `0x022e`, `0x0249`, `0x024b`, `0x024d`, `0x024f`, `0x02aa`, `0x0404`, `0x0409`, `0x040c`, `0x0412`, `0x0416`, `0x0427`, `0x0e22`, `0x0e27` (values in the source of the tool) | 66 of 66 |
| S2a | **Admission rule derived from S2: register-set membership.** A shader state stream may write only the registers in the observed set (the 43 operand words of the register table below, that is 36 registers) and the uniform bank `0xd800` to `0xd8ff`. Order, count and operand values of those registers are not checked, except that `0x0e02` must be `0x701`, the stall token `0x701` and `0x0e03` `0x24` (the loader-owned flush and synchronization writes). An unlisted register could control the MMU or command fetch | Set of 36 registers in 66 of 66 |
| S4 | The uniform bank is the register range `0xd800` to `0xd8ff`. Packets of 1 to 4 operands; no word is written twice | Highest word written: `0xd883` |
| S5 | `0x0424` is the instruction count, `0x0425` the count minus one, and 16 times the count equals the length of the program table entry | 66 of 66 |
| S6 | The patch list holds exactly one source-0 patch, on the `0x040a` operand with index -1, source offset 0 and transformed field 0; every other patch is a kind-0 patch on an operand word of a uniform packet; no word is patched twice | 66 of 66 |
| S7 | A patched word that opens a 4-operand uniform packet at a register that is a multiple of 4 from `0xd800`, followed by a 4-operand packet at the next register, starts a tensor descriptor (below): channel stride = row stride x height, with height = channel stride / row stride a whole number, depth not zero, and word 2 equal to height x 65,536 plus the width, for a width in elements that does not exceed the row stride in bytes. **A width or a height of 65,536 or more is refused** (reason `NBG_REFUSED_DIMENSION`): the engine's shader reads 16-bit fields (next paragraph) | 623 of 652 descriptors of 337 graphs satisfy the former rule `word 2 >> 16 = height`; the 18 that need the carry are the whole-length attention layers of 184 frames and more, which are now refused; the other 11 are listed below |
| S8 | Offset plus the descriptor extent (channel stride x depth) is at most the size of the region the patch names | 160 of 160 |
| S9 | The shader table entry exists, names a program row, the parameter row is 0, and the program lies inside the program section | 66 of 66 |

### Registers outside the uniform bank

**Registers a graph must never write.** The MMU configuration, the cache flush and synchronization registers and the
command-fetch registers belong to the driver. From the vendor driver (`vip2/vip_drv_hardware.c`, byte addresses of the
same register space, load-state register = byte address / 4): MMU control and status `0x388` (state-stream register `0x00e2`) and
its neighbours `0x38c` to `0x394` (`0x00e3` to `0x00e5`), command-fetch size and control `0x3a4`, `0x3a8`, `0x3b4` (`0x00e9`, `0x00ea`,
`0x00ed`), command-fetch address `0x654` (`0x0195`), and the control, idle, interrupt and reset registers `0x000` to `0x03c`
(`0x0000` to `0x000f`). From the etnaviv register database (recalled, not in the pinned sources): the MMU registers near byte `0x180`
(`0x0060` to `0x006c`), the front-end registers from byte `0x600` (`0x0180` to `0x01ff`), and the flush and semaphore registers
`0x0e03` and `0x0e02`. Checked against the observed set: none of the MMU, command-fetch, control or front-end registers is in it.
Two members of the flush family are: `0x0e03` (cache flush, always `0x24`) and `0x0e02` (semaphore, always `0x701`); S2a pins their values,
so a graph cannot select other flush bits or tokens. The loader must reject any other write to these ranges.

The table names each operand of the state stream once (43 operand words and the stall token). "Sensitive" means that
zeroing the operand or flipping its low bit changed the simulator output (`tools/npu-shader-probe.py`, experiment
`registers`, run on a floor shader). A register to which the simulator is blind may still matter on the board.

| Register | Observed values | Class | Meaning and evidence |
| --- | --- | --- | --- |
| `0x040a` | 0 (patched) | Confirmed | Address of the program. The loader writes the program section address plus the program table offset. The simulator does not read the operand; it takes the program from the tables (above) |
| `0x0424` | 11 to 124 | Confirmed | Instruction count. One fewer breaks the program (696 of 1024 outputs change); one more changes nothing |
| `0x0425` | count - 1 | Confirmed | End program counter, same behavior as `0x0424` |
| `0x0250` | 0 to `0x3ff` | Confirmed | Work groups in x minus one. In a flat element-wise operation the work is (`0x0250` + 1) x (`0x0253` + 1) x `0x0256` elements, 32 x 4 x 8 = 1024 for 16 x 64 elements. Halving `0x0250` leaves the second half of the output unwritten; more groups than needed change nothing |
| `0x0253` | 0 to 3 | Confirmed | Elements per thread minus one. Setting 0 leaves everything past element 256 unwritten |
| `0x0251`, `0x0252`, `0x0254`, `0x0255` | 0 in one-dimensional work; small numbers and `0xffffffff` otherwise | Inferred | The other components of the six-operand work description at `0x0250` |
| `0x0256`, `0x0257`, `0x0258` | `0x0256` 1, 4, 8; `0x0257` 0, 1, 4; `0x0258` 0, 1, 4, 8 | Inferred | Work-group size per axis (the vendor parameter section has such a field); `0x0256` is 8 in element-wise operations |
| `0x0403` | 2 to 14 | Inferred | Number of temporary registers of the program; it grows with the program, and 0 makes the simulator fail |
| `0x021f`, `0x0240`, `0x0402`, `0x024d` | `0x021f` and `0x024d` 0 in 66 of 66; `0x0240` `0x03000001` to `0x03000003` or `0x05000002`; `0x0402` `0x1f01` (65) or `0x1f03` (1) | Unknown, sensitive | Changing them changes the output; the effect is not established |
| `0x5580`, `0x0e02`, stall token, `0x02a4`, `0x0420`, `0x0416`, `0x0409`, `0x021a`, `0x0427`, `0x0404`, `0x0228`, `0x028c`, `0x0e07`, `0x040c`, `0x020c`, `0x0201`, `0x02aa`, `0x0e22`, `0x0e27`, `0x0412`, `0x0249`, `0x022e`, `0x0247`, `0x024b`, `0x024f`, `0x0248`, `0x0e03` | Constant, or small (`0x0247` 1 to 16, `0x0e07` 0 or 4, `0x028c` `0x100` or `0x300`) | Unknown | No simulator effect when zeroed or flipped. `0x0248` holds `0xbadabeeb`, the last write before the flush `0x0e03` = `0x24` (cache flush) and the semaphore and stall pair, and is therefore taken to start the work |

### Uniform bank and tensor descriptors

The uniform bank carries the kernel arguments and holds 20 to 130 words per operation. Each tensor argument is two
consecutive 4-operand packets, 8 words. The rest of the bank is constants of the program: fixed-point and half-precision
literals (`0x3c00`, `0x3f800000`), index tables (`0x06040200`) and masks; their meaning is Unknown, and they are admitted
with the program.

| Word | Value | Class | Evidence |
| --- | --- | --- | --- |
| 0 | Address (patched) | Confirmed | A plain 32-bit byte address; moving the patch offset by 64 bytes moves reads and writes by 32 half-precision elements (every source, below) |
| 1 | Row stride in bytes = row elements x element bytes | Confirmed | Doubling it makes row y read from byte 32 y (resize, exact) |
| 2 | Height << 16 plus width, modulo 2^32 | Confirmed (short tensors); long tensors by the layer graphs of the encoder, below | Rows or columns past the bound read as the last row or column (clamped). A 16 x 64 flat tensor has height 1 and width 1024, or height 2 and width 32768 when the width does not fit. The compiler adds the width to the height shifted by 16 without masking: a width of 65,536 or more carries into the height, and a height of 65,536 or more is cut at 32 bits |
| 3 | Element format | Inferred | Bit 12 is set exactly when height is greater than 1 (160 of 160); the low byte is `0x71` for half precision (121), `0xf1` (14), `0x41` (3), `0xc1` (1), `0xb2` for 32-bit integer indices (23), `0x32` for single precision (2), `0x72` (1); the high half is `0x4440` or `0x3210` for integers |
| 4 | Channel stride in bytes = row stride x height | Confirmed | Halving it makes channel c read from byte 64 c (resize, exact); the output descriptor behaves the same |
| 5 | Depth (channels) | Unknown | Doubling or halving it changes nothing: the work description fixes the channel count |
| 6 | `0x10dd10b0` (114), `0x10d910b0` (21), `0x10d810b0` (14), `0x10dc10b0` (1), 0 (10) | Unknown | Varies with the element type; no mutation has been run |
| 7 | 0 | Unknown | Zero in 160 of 160 descriptors |

The two patched words that do not open a tensor descriptor: the reciprocal's packet at `0xd818` holds a pointer (source 6) in
word 0 followed by single-precision 1.0 twice and 0; a TopK shader holds a pointer to a 64 KiB dynamic scratch table (source 6) in word 2 of its
packet at `0xd81c`, with a transformed field of 14 on that patch (transformed field 0 gives the same output, 1 raises a
memory-management exception; the field is otherwise 0).

### Address fields and extents

A patched word receives `device address of the region + source offset`, with no shift. Regions follow the patch source as for
neural-network operations (`nbg_assembly.c`).

| Source | Region | Word patched | Extent | Confirmed by |
| --- | --- | --- | --- | --- |
| 0 | Program section plus table offset | `0x040a` | 16 x instruction count | Table indirection above |
| 1, index i | Graph input i | Descriptor word 0 | Descriptor channel stride x depth; equals the input tensor size | Offset +64 moves the read by 32 elements (floor, gather, embedding); 128 MiB raises an MMU exception |
| 2, index i | Graph output i | Descriptor word 0 | Same for the output | Offset +64 moves the write (floor, gather, embedding) |
| 3 | Memory pool | Descriptor word 0 | Descriptor extent (TopK scratch, 256 bytes at offset 256 of a 1536-byte pool) | 128 MiB raises an MMU exception. Moving it within the pool changes no output, because the shader writes and reads it |
| 5, row r | Constant-data table row r | Descriptor word 0 | Descriptor extent; equals the row length | Gather indices, embedding table, reciprocal numerator: +4 bytes reads the next index, +1 row reads the next table row, an edit of the data changes the output as predicted |
| 6, row r | Dynamic-input table row r | Word 0 (reciprocal) or word 2 (TopK) | Not stated by the stream | Offset +4 reads a marked constant; 128 MiB raises an MMU exception |
| 8 | VIP SRAM, base `0x400000` | Descriptor word 0 | Descriptor extent | Six moves of 64 bytes between four chained shaders match the exact prediction (one exception of one half-precision step in the preserved head) |

In the 55 distinct graphs the 162 patches other than program patches name 58 graph inputs, 52 graph outputs, 16 constant-data
rows, 29 VIP SRAM ranges, 5 pool ranges and 2 dynamic-input rows. The descriptor extent equals the region size in all 126 patches of
sources 1, 2 and 5. The descriptor extent is not an enforced bound: the shader
reads and writes at channel stride x channel + row stride x row + element, with the coordinates clamped inside the height
and width of its own descriptor, and the channel count comes from the work description. A graph whose descriptor words
disagree with each other (S7) or with the region (S8) is therefore malformed, and a graph that satisfies them stays inside the
declared extents only if the program does what the compiler wrote. The page tables contain the rest (below).

### Command buffer placement

The load-state stream of each operation is copied without change, apart from patched words and pad words, in operation
order. The stream ends with its own cache flush, semaphore and stall, so nothing is inserted between a shader operation and
the next operation. The first shader operation of a graph carries the prologue; the loader does not add or remove it. The
neural-network and transpose operations that surround a shader keep their own rules. Class: Inferred from the vendor
concatenation rule and from the existing assembly of the relative-shift model (`nbg_sh_nt.c`); no board run qualifies it.

**Long tensors (S7): the hardware cannot run a dimension of 65,536 or more.** The relative-shift gather of an attention layer reads
the position-score table, `rows` x (2T - 1) 16-bit elements per head, as one flat tensor, and its constant index table holds
`rows` x T 32-bit indices, where `rows` is T for whole-length attention or the number of query rows in a block. The compiler
writes word 2 as width + (height << 16) in 32 bits without masking:

| Graph | Descriptor | Row stride | Word 2 | Width x height | Low and high 16 bits of word 2 |
| --- | --- | --- | --- | --- | --- |
| 184 frames, whole length | scores | 135,056 | `0x000507c8` | 67,528 x 4 | width 1,992, height 5 |
| 276 frames, whole length | scores | 304,152 | `0x0006520c` | 152,076 x 4 | width 21,004, height 6 |
| 276 frames, whole length | indices | 4 | `0x29900001` | 1 x 76,176 | width 1, height 10,640 |

The board separates the graphs by this dimension (one layer, 300 jobs or three consecutive jobs, one slot). The mechanism, that the
shader reads the 16-bit fields, is the inference that fits this table; no board run has varied the field alone:

| Graph | Widest flat dimension | Board output |
| --- | --- | --- |
| 92 frames, whole length | 16,836 | byte-exact with the simulator |
| 184 frames, 92-row blocks | 33,764 | byte-exact |
| 276 frames, 92-row blocks | 50,692 (index rows 25,392) | byte-exact |
| 184 frames, whole length | 67,528 | different in every run |
| 368 frames, 92-row blocks | 67,620 | different in every run |
| 460 and 512 frames, 92-row blocks | 84,548 and 94,116 | different in every run |
| 276 frames, whole length | 152,076 (index rows 76,176) | wrong by up to 305 steps |

The SDK simulator computes with the 32-bit values and returned the compiler's intended result for every one of these graphs, so
agreement with the simulator does not cover this limit. The loader therefore refuses a descriptor whose width or height, taken
from the strides, is 65,536 or more; a layer must be compiled with every flat shader dimension below that (for the
relative shift, query blocks of at most 65,535 / (2T - 1) rows: 64 rows up to 512 frames). The 11 descriptors of the corpus that
satisfy neither the old nor the new rule are mutated shader experiments (`rn_a`: the height halved or doubled, the stride
doubled) and two compiled graphs (an einsum and a scatter) whose channel stride is not the product of the row stride and the
height field; the loader refuses them, as before.

### Maximum of negative values

A maximum over values that are all negative is kept. A probe graph took `Max(a, b)` and `ReduceMax(a)` of 16-bit fixed-point tensors
(four rows of 64: maxima -2.35, a mix, -30 and +10.8) in four forms: `Max`, `ReduceMax`, `ReduceMax(a + 40) - 40` and
`-ReduceMin(-a)`. The simulator returned -2.35, 10.20, -30.0 and 10.77 in every form that reduces, and the board returned the same bytes
as the simulator (20 jobs; entry in the board queue for 2026-11-02). An earlier suspicion that the reduction starts at zero came from
feeding half-precision inputs to the simulator as 16-bit codes instead of decimal values (`npu_encoder_simulate.sh` now writes an
input the compiler left in half precision as decimal text, which the program converts), and a lift of the scores is not needed.

### Command segments

The doorbell word `+0x3a4` carries the fetch size in 16 bits of 8-byte units, so one fetch is at most 524,280 bytes
(`vipdMAX_CMD_SIZE`). The vendor's stream carries a LINK command for a longer buffer: `cmd_link` (`vip2/vip_drv_hardware.c`) writes
`(8 << 27) | (vipdMAX_CMD_SIZE >> 3)` and then the target address, and the vendor's linker library `libNBGlinker.so` names the
operation in its messages ("fail to insert LINK command for big command buffer, split command size=0x%x, max support size=0x%x",
`nbglk_insert_link`, `split_node`). The vendor driver itself uses the same command to join the MMU-flush states to a command
buffer and to loop the wait-link table. The command is two words: opcode 8 in bits 31:27 with the size of the next fetch in
8-byte units in the low bits, then the device address of the next segment. The NBG file holds one load-state stream per
operation and no link; the compiler never splits a graph, the linker does, when it places the buffer.

The loader does the same. `nbg_assembly.c` places the operation streams in order; when the next stream and a link would pass
`NBG_SEGMENT_BYTES` (524,280) from the start of the current segment, it writes a LINK at the end of the segment (aligned to 8 bytes
because every stream is), starts the next segment on a 64-byte boundary of the same command allocation, fills the bytes between with
END words, and gives the LINK the address of the segment and a fetch size of the rest of the allocation (at most 524,280). The
footer starts a segment of its own when it does not fit. The first fetch is the whole allocation up to one fetch, as before. The
segments are inside the allocation, which the page tables map read-only, so a link names no address the stream could not already
fetch. An operation stream longer than a segment (524,208 bytes) is refused. The layer graphs of the encoder do not need a link:
the stream of one layer is 33,856 bytes at T = 184, 42,464 at 276, 78,184 at 368, 239,128 at 460 and 501,208 at 512 (the
loader checks the link on the board with a build whose segment limit is 16,384 bytes; see the status document), so only a graph of
two layers at T = 512, or of three at T = 460, would join segments in production.

Host evidence: `checks/nbg-command-link.py` builds the assembler with segment limits of 192 to 1,100 bytes, assembles eight
compiled graphs in the one-call and the stepwise way, follows the links as the front end does, and requires the executed states
to equal the unsplit stream's, every segment to be within the limit, every link to name an aligned address inside the allocation
with the fetch size above, and the two ways to write the same bytes. Two controls (a link to the wrong address, a fetch size past
the allocation) are caught.

### Loader specification (shader operations)

1. Admit kind 1 only for graphs whose digest is in the reviewed catalogue, as for the other kinds.
2. Parse the shader table, program table and state stream by rules S1, S2a and S4 to S9. S2 and S3 are observations: do not refuse a
   graph for register order or for the value of an operand other than those S2a pins, for a constant of the uniform bank, or for an
   element format word. Refuse any write to a register outside the S2a set.
3. Resolve the program as `program[ shader_table[ operation.index ].program_row ]`; write `0x040a` with the program section
   device address plus the table offset (a multiple of 256) and ignore the patch offset. The program section must be
   readable by the engine and 256-byte aligned.
4. Resolve every other patch to a region: source 1 and 2 to the tensor of that index, 3 to the pool, 5 to the constant-data
   table row, 6 to the dynamic-input table row, 8 to VIP SRAM at `0x400000`. Refuse source 4, 7, 9 and above, a negative index
   on sources 1, 2, 5 and 6, and an index other than -1 on 3 and 8. Write the 32-bit sum of the region address and the patch
   offset into the patched word.
5. For each patched word that starts a tensor descriptor, require offset plus extent not greater than the region size. For
   source 6 require only that the offset lies inside the table row.
6. Write the pad word `0x18000000` into every pad position.
7. Copy the stream into the command buffer at an 8-byte aligned position. The state length is at most 1136 bytes in the
   graphs examined and has no fixed limit: a stream is refused only when it cannot share a command segment with its link
   (see "Command segments" below), which is a stream of more than 524,208 bytes.
8. Do not edit, reorder or drop registers other than pad words and patched words. Do not interpret the program.
9. Keep the programs and the uniform constants inside the digest of rule 1. The 31 program digests of the graphs examined
   can seed a program catalogue as for kind 6.
10. A graph with a parameter section, a nonzero parameter row, a source-6 patch with a transformed field other than 0 or 14,
    or a patch of kind 1 on a shader operation is refused until a graph presents it.

Reference check for the rules: `tools/npu-shader-state.py --check GRAPH...`; its self-test lists 33 cases (30 rule breaks and 3 controls) around the
pre-change stream.

### Limits

- **The shader path honouring the page tables is untested.** Everything above is host evidence from the simulator, which models
  an MMU, but no board run has shown that the engine's shader, program-fetch and descriptor paths go through the page tables.
  Two board cases would test it: (1) a data descriptor (S7) whose address word, patched at load, points at an unmapped canary
  address, expecting an MMU exception and a healthy recovery with the canary untouched; (2) a program address (source 0,
  register `0x040a`) pointing at an unmapped canary address, expecting a fetch fault. Both need the fault-recovery approval named in
  [the loader design](../designs/npu-loader-admission.md).

- The program words are opaque. The pinned Mesa tree holds only the four neural-network source files named in its manifest, so
  no instruction encoding is available to decode the 16-byte instructions, and the vendor toolkit exposes none. The loader
  cannot prove that a program reads and writes only through the descriptors it declares.
- The descriptors are consumed by the program as strides and clamps, not as bounds of the allocation. Confinement rests on the
  page tables and on the simulator's behavior that every patched source raises a memory-management exception at 128 MiB
  past its region. Whether the engine's shader path honors the page tables is the question of the board negative test in
  [the loader design](../designs/npu-loader-admission.md).
- The simulator ignores most configuration registers, so a register class of Unknown with no simulator effect may still be
  required by the engine.
- The patch field `transformed` has values 0 and 14 on shader patches; only the effect of 1 is known (an exception).

## Tensor processor (kind 3) and software (kind 4) operations

**Status: not emitted for the board target; both kinds stay refused.**

Conditions: 23 models (max and average pooling with kernels 2 and 3, global pooling, local response normalization, space to depth
and depth to space, Pad, four transposes, concatenation on two axes, split, slice, convolution, depthwise convolution, MatMul,
Tile, Expand, Gather and nearest Resize) were compiled for the board target (`VIP9000NANODI_PLUS_PID0X1000003B`) and for
eight other targets of the toolkit: `0x1000003F`, `0x1000004C`, `0x10000023` and `0x10000036` (all four emit no kind 3 or 4),
and `0x1000000B`, `0x10000016`, `0x1000001D` and `0xA0`. The target `0x10000025` fails to export.

Result: the board target and the four targets that behave like it never emit a tensor-processor or software operation; the compiler
assigns the same models to kinds 8, 2, 1 and 6. The other four targets emit kind 3 for 13 to 17 of the 23 models: all
transposes, concatenations, splits, slices, Pad, local response normalization, space to depth and depth to space, the
transposes around a MatMul, and max pooling (three of the four targets; `0xA0` also average pooling). No target emitted kind 4. Thirteen less common operators (OneHot,
ScatterND, GatherND, Mod, ReduceProd, Einsum, Round, Sign, NonMaxSuppression, NonZero, RoiAlign, Unique, GridSample) did not produce one
either: eight compile to kinds 1, 6 and 8 on the board target and five fail to import or export.

The board's own SDK query reports zero tensor-processor engines for target `0x1000003B` ([relative shift](../npu-relative-shift.md)),
so a graph of kind 3 would address an engine that the NPU does not have, and the graph header target would not match.
The vendor structure of a software operation, read from the debug information of the vendor `nbinfo` tool, is a type word and
a 64-byte name with no load-state stream; it names work for the vendor runtime on the CPU, which SunstoneOS does not have.

Conclusion: kinds 3 and 4 cannot be tested on this hardware and no graph for it contains them. The loader keeps refusing both
until a graph for the board target presents one. No decoder is written. For reference, a kind-3 operation of the older
targets has a 192-byte descriptor in its own section, five state writes (`0x0e53`, `0x0e4c`, `0x0e54`, `0x042e`, `0x0429`)
and patches at descriptor words 10 (input) and 13 (output).

## Evidence

`tools/npu-shader-probe.py` rebuilds every shader check from compiled graphs; its experiments are `elementwise`, `gather`,
`embedding`, `reciprocal`, `vip-sram`, `program`, `mmu`, `descriptor` and `registers`. Each experiment changes one field of a
graph, repairs the checksums with `tools/npu-nbg-mutate.py`, runs the simulator and compares the output with a NumPy
prediction. The simulator needs about 4.3 GB per run (measured peak), and a compile 4.6 to 5.2 GB.

Result of the last full run: 49 checks, 0 failed (`elementwise` 8, `gather` 6, `embedding` 5, `reciprocal` 4, `vip-sram` 7, `program` 5, `mmu` 6, `descriptor` 7, `registers` 1, plus the per-register table printed by `registers`).

## Not yet examined

The program words and the constants of the uniform bank; the effect on the engine of the registers to which the simulator is
blind; the parameter section of shaders with dynamic shapes; the batched matrix product, the gated-linear-unit gate and the
other two-input forms; the extents of the second input of neural-network operations. Each needs variants and mutations as above
before the loader interprets it.
