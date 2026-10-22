# Exact and rounded mimus linear layers on the NPU compiler and simulator

This is a **research result** from the pinned host compiler, the SDK
simulator, native host checks and instruction emulation; no board or NPU
executed anything. The NPU can return exact integer sums for a mimus int8
linear layer when the CPU splits each activation into four base-16 digits:
the simulator returns every partial sum exactly, and the CPU read-out of the
recombined sums is bit-identical to the mimus kernel. The NBG descriptor has
no 32-bit output code that the simulator accepts. On the session runtime as
it is written today, one exact layer costs about 13 times the CPU cycles of
the A76 NEON kernel it would replace, so it is not used. The INT16 layer,
which rounds each output once, is recorded as a separate variant with
a word error rate that meets the float32 pass criterion on three held-out
sets in simulation. Neither graph is in the session catalogue, and physical behavior
is untested.

## mimus computes an exact integer sum, then a float read-out

The P4 build of mimus, the speech recognizer this project runs, stores each
linear layer's weights as int8 codes with one fp16 scale `ws[n]` per output
row. Its int16 activation mode quantizes each input row `x` with a float32
scale `s = max|x| / 32767`, multiplies by `1 / s` in float32 and rounds half to
even, clamped to ±32767 (mimus `train/quant.py`). The portable kernel
`MimusGemmS16FC` then forms the exact integer sum `S[n]` over the codes and
computes, in float32 and in this order, `(float(S[n]) * ws[n]) * s + bias[n]`.

A signed 32-bit sum holds this layer for a reduction length K of at most 516,
because |S| ≤ 32,767 · 127 · K. A compiled graph has one static input scale,
while mimus scales every row separately; an NPU layer that works on the codes
at unit scale leaves each row's scale to the CPU read-out.

## The layers are built from the mimus codes and scales

[npu-mimus-linear.py](../tools/npu-mimus-linear.py) reads a mimus model file
directly, using the layout documented in mimus `train/model_format.py`, and
checks every tensor CRC. It generates eight test rows from a fixed seed,
quantizes them with the mimus rule and computes the mimus result.
[npu-mimus-linear.sh](../tools/npu-mimus-linear.sh) runs inside the pinned
compiler image: it imports the generated ONNX graph, replaces every entry of
the compiler's quantize file with mimus values, exports the network binary
graph (NBG), runs it in the SDK simulator on the exact codes and compares the
result. Three forms exist:

- **INT16 layer.** An ONNX MatMul plus Add whose float weights are the int8
  codes times their scales. INT16 input at the rows' shared activation scale,
  per-channel int8 weights with the mimus `ws[n]`, int32 bias in units of the
  activation scale times `ws[n]`, and an INT16 output scaled to the result
  range. The rows share one maximum so that one static input scale fits them.
- **Digit layer** (`--digits`). Each code is written as
  `x = d0 + 16·d1 + 256·d2 + 4096·d3`, with `d0`, `d1`, `d2` in [−8, 7] and
  `d3` in [−8, 8]. The reduction is cut into C = ⌈K / 32⌉ chunks whose lengths
  L differ by at most one. Chunk c has one INT8 input of shape [4R, L] for R
  rows, whose row 4r + j holds digit j of row r. Its fully connected layer
  applies the int8 codes at unit scale with no bias and writes one INT16
  output of shape [4R, N] at unit scale. Every partial
  `P[j,c,n] = Σ d_j[k]·w[k,n]` over the chunk lies within ±L·8·max|w|. The tool
  refuses a weight code of −128, a bound above 32,767 and more than eight
  chunks, the limit of the native assembler. The rows use different maxima.
- **Unit-scale INT16 layer** (`--unit`). The INT16 layer with codes at unit
  scale and no bias, so its outputs are the sums clamped to INT16. It is the
  base graph for the descriptor probe below.

[npu-mimus-kernel.cc](../checks/npu-mimus-kernel.cc) compiles the unmodified
mimus portable kernel on the host and runs it on the same codes, weights,
scales and bias; for every study its float32 output is bit-identical to the
study's reference. For a digit study it also splits the codes with
[npu-mimus-digits.cc](../checks/npu-mimus-digits.cc), requires the digits to
equal the simulator's input files, recombines the simulator's partials and
counts read-out floats that differ from `MimusGemmS16FC`.

## The INT16 layer rounds each output once

The input is mimus model file SHA-256
`f73ccd024645387d2679eea97ae11d0c33f5e4be81b39a9430d1f5d285e8089d`, the
benchmark's P4 base model, with test seed 20261021 and row maximum 4.0. Three
layers of encoder block 0 were compiled:

| Layer | K × N | Largest &#124;sum&#124; | Codes equal to one rounding of the exact result | Equal with int32 bias | Read-out floats that differ from `MimusGemmS16FC` | Largest error / output step |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| Attention query projection | 176 × 176 | 22,917,137 | 1,408 of 1,408 | 1,408 of 1,408 | 1,407 of 1,408 | 0.000233 / 0.000467 |
| First feed-forward, first linear | 176 × 704 | 25,538,357 | 5,630 of 5,632 | 5,632 of 5,632 | 5,632 of 5,632 | 0.00211 / 0.00421 |
| First feed-forward, second linear | 704 × 176 | 30,775,865 | 1,406 of 1,408 | 1,408 of 1,408 | 1,406 of 1,408 | 0.00276 / 0.00552 |

The fourth column counts output codes equal to rounding the exact real value
`sum · ws[n] · s + bias[n]` once to the output step. The fifth uses the bias as
the NPU holds it: an int32 code in accumulator units, the product of the
activation scale and `ws[n]`. With that bias every simulator code equals one
rounding of the exact accumulator, so the NPU loses no precision in the sum
itself. The compiler emits a neural-transpose step, one fully connected
operation and a second transpose step, with intermediates in internal SRAM.
Recompiling the query projection reproduces the catalogued graph
`4872524cf2bd0681e9b17c8a8bcaa61615dc925e31313d6d1f0dba2fea4eacb0` byte for
byte.

## No 32-bit output code exists in the descriptor

The compiler refuses an INT32 output for the query projection and the second
feed-forward linear: network-binary generation stops with "Fatal model
generation error: 64768", exit status 253 inside a wait status, after the
SDK's graph verification returns −3. The descriptor probe tested whether a
compiled graph can be changed to a 32-bit output afterwards.

[npu-mimus-probe.py](../tools/npu-mimus-probe.py) compares compiled query
projections whose outputs differ only in type (INT16, INT8 and UINT8 at one
step) or only in step. In the 192-byte fully connected record:

- The output data type is three bits: bits 4–5 of word 1 and bit 21 of
  word 15. INT16 output uses code 4; INT8 and UINT8 both use code 0, and the
  INT8 form adds an output zero point of 128 in bits 8–15 of word 22.
- The output strides are the high half of word 8 and the low half of word 17,
  and the output buffer size in 64-byte units is the low 26 bits of word 18;
  each halves for an 8-bit output.
- The post-shift is bits 27–31 of word 2. Doubling the output step lowers it
  by one and changes the per-channel multipliers stored in the coefficients.
- In the following transpose record, word 5 is 2 for 16-bit and 1 for 8-bit
  elements, and words 8, 10, 16, 17 and 20 hold its strides and buffer size.

The simulator's NN model names twelve data types, including INT32, in its
assertion strings. The probe wrote every three-bit output code into a copy
of the unit-scale query projection, repaired the section and primary
checksums, and ran each copy in the simulator. The repair reproduces each
compiled graph's stored checksums, the native verifier `nbg_checksum_verify`
accepts all 29 patched copies, and rewriting code 4 unchanged reproduces the
compiled output bytes.

- **Type field only.** Codes 0, 1, 2, 5, 6 and 7 run and return none of the
  1,408 sums. Code 3 stops the simulator with the assertion "Don't support
  U16 in NN", which identifies it as UINT16.
- **Type with a 32-bit layout.** Codes 1 to 7, each with the record's output
  strides and buffer doubled, the transpose element word set to 2, 3 or 4
  with its strides and buffer doubled, and the output tensor declared with
  twice the INT16 elements. No combination returns the sums read as INT32;
  code 3 again stops the simulator.

No three-bit code yields exact sums, so the descriptor offers no INT32
output. The probe did not locate a fourth type bit; if the v9.2 engine has
one, its codes 8–15 remain untested. Patched graphs are simulator-only probe
artifacts. They are not in the session catalogue, whose admission rests on
reviewed compiler output ([npu-session.md](npu-session.md)), and never run
on a board.

## Base-16 digit partial sums are exact

Every digit layer compiled, and every simulator partial equals the exact
partial:

| Study | Chunks | Partials equal | Partial range | Recombined sums equal | Read-out floats that differ from `MimusGemmS16FC` |
| --- | --- | ---: | --- | ---: | ---: |
| Query projection, 176 × 176 | 6 (30, 30, 29, 29, 29, 29) | 33,792 of 33,792 | −5,006 … 4,283 | 1,408 of 1,408 | 0 of 1,408 |
| Range fixture, 176 × 176 | 6 | 33,792 of 33,792 | −30,480 … 30,480 | 1,408 of 1,408 | 0 of 1,408 |
| First feed-forward, first linear, 176 × 704 | 6 | 135,168 of 135,168 | −4,889 … 5,064 | 5,632 of 5,632 | 0 of 5,632 |

The range fixture keeps the query projection's scales and bias and replaces
its codes: weight columns of all +127, all −127, alternating ±127 and random
codes; rows of +32,767, −32,767, 30,584 (digits −8, −8, −8, 8), −2,184
(digits −8, −8, −8, 0), alternating ±32,767 and random codes. It reaches the
bound 30 · 8 · 127 = 30,480 with both signs. The second feed-forward linear,
K = 704, needs 22 chunks and is refused by the tool; layers with K of 704 or
more stay on the CPU.

The digit split and recombination in
[npu-mimus-digits.cc](../checks/npu-mimus-digits.cc) use int32 lanes: any
INT16 partial has |P| ≤ 32,768, so eight chunks give
|S| ≤ 8 · 32,768 · 4,369 < 2^31. Compiled with the backend's AArch64 flags,
the split executes 12,230 instructions and the recombination with read-out
41,341 instructions for 8 rows of the query projection, under Unicorn. Both
compiled functions reproduce the simulator inputs and the reference bytes.

## On today's runtime the exact layer costs more CPU than the A76 kernel

[npu-mimus-cost.c](../checks/npu-mimus-cost.c) assembles each graph with the
native assembler at the session placement and derives one job's CPU events
from the code paths of the [session runtime](npu-session.md): the SUBMIT copy,
the per-job integrity checks in publication (`model_intact`, `live_mappings`,
`canonical`), whole-buffer cache maintenance, completion, the output copy and
the SUBMIT, WAIT and REAP round trips. The counts are derived from the code;
the session host model does not yet measure them. One job holds 8 rows of the
query projection, the live stream's chunk of eight encoder frames.

| Event per job | INT16 layer | Digit layer |
| --- | ---: | ---: |
| NN multiply-accumulates | 247,808 | 991,232 |
| Bytes copied in by SUBMIT | 2,816 | 5,632 |
| Bytes copied out | 2,816 | 67,584 |
| Bytes cleaned / cleaned and invalidated / invalidated | 4,096 / 4,096 / 4,096 | 24,576 / 73,728 / 73,728 |
| Bytes hashed with SHA-256 | 37,064 | 54,120 |
| Command words checked for END | 65,511 | 65,427 |
| Page translations | 556 | 642 |
| Page-table words read | 5,120 | 5,120 |
| Queue round trips | 3 | 3 |
| CPU DDR bytes read / written | 587,264 / 5,632 | 671,232 / 73,216 |
| Client instructions for digit split / recombination | none | 12,230 / 41,341 |
| NPU cycles, compiler estimate | 11,193 | 51,104 |

[npu-mimus-cost.py](../tools/npu-mimus-cost.py) charges these events with
estimates chosen at the expensive end: one cycle per instruction of the exact
path, 25 cycles per hashed byte, 4 and 12 instructions per checked command
and page-table word, 100 cycles per translation, 20 cycles per cache line
maintained, half a cycle per CPU DDR byte and 2,000 cycles per queue message.
NPU cycles are the compiler architecture model's `CycleCount` estimates
(`NN_EXT_SHOW_PERF=1` at export), converted at 492 MHz for the NPU and 494 MHz
for the CPU; the SDK simulator reports no cycle count. The A76 kernel is the
packed NEON `MimusGemmS16FC` from the mimus `port/sunstoneos` branch: 332,354
instructions for 8 rows under Unicorn, charged as 166,177 cycles by crediting
two NEON pipes. The only measured kernel figure, the board NEON run of
0.28 billion multiply-accumulates per second on one core at 494 MHz,
corresponds to about 440,000 cycles for these rows; the A76 has not been
measured.

| Path, 8 rows | CPU cycles, client + service worker | Client time from SUBMIT to copied output, cycles | Against the A76 kernel |
| --- | ---: | ---: | --- |
| Digit layer, today's runtime | 2,231,903 | 2,283,215 | 13.4 and 13.7 times |
| INT16 layer, today's runtime | 1,619,380 | 1,630,618 | 9.7 and 9.8 times |
| Digit layer with the runtime changes below | 105,571 | 156,883 | 0.64 and 0.94 times |

The exact layer passes only when both figures are below the kernel's
166,177 cycles, so it fails on today's runtime. The per-job SHA-256 of the
descriptor and coefficient allocations costs 1.35 million cycles, and the
scan of the 512 KiB command allocation reads 523,416 bytes. No job size
recovers the cost: the exact path's per-row work, the split, recombination,
both copies and the cache maintenance of the tensor bytes, is 21,348 cycles
against the kernel's 20,772, so it never breaks even with today's runtime.

The last row applies four changes that belong to separate runtime designs:

- an accessor that lets the client write READ buffers and read WRITE buffers
  directly, removing both copies;
- cache maintenance over the tensor bytes only;
- read-only mappings that replace the per-job digest, command scan and page
  translations;
- a completion reply that also performs REAP.

With all of them the exact layer would use 0.64 times the kernel's CPU cycles and
0.94 times its time. This figure is input to those designs and does not
change the result here.

## The INT16 variant keeps recognition accuracy in simulation

Because the exact layer does not pay off on today's runtime, the INT16 layer
is recorded as a separate variant; the shipped model and CPU kernel stay
unchanged. [npu-mimus-accuracy.py](../tools/npu-mimus-accuracy.py) reuses the
model, training-set calibration, decoding and scoring of mimus
`train/static_scales.py` and replaces the arithmetic of every 16-bit site with
int8 weights by the INT16 layer's:

- one static input scale per site, from the calibrated 99.999th percentile
  of |x|, with codes clamped to ±32,767;
- the exact integer sum over the int8 weight codes;
- the bias as one int32 code per output channel in units of the input scale
  times `ws[n]`;
- one rounding of each output to the INT16 step of the site's calibrated
  output range, max |y| / 32,767.

The run covers all 179 such sites of the P4 int8-weight, 16-bit-activation
simulation model (`int8_a16_punct/sim.pt`, SHA-256
`6c46d6f23f656e2fac0262c6baeaa84831d948e5af2f3f5b098c70269547a091`), with the
study's calibration statistics (SHA-256
`7e3c701c3dd7025cde3fe78ce21292f099c0cac63a1ec88269ac934d7f682fa6`). The
compiled layers above come from the benchmark model file, a different build,
so this run measures the INT16 arithmetic rather than those weights. Each set is compared with the float32
model and with the deployed per-row arithmetic using 10,000 paired bootstrap
resamples over utterances. A set passes when the 95% interval of the relative
WER change against float32 contains 0 and the point estimate is at most +1%
(mimus `docs/int8_activations.md`). mimus reserves test-clean and test-other
for milestones its owner calls, so the development sets serve as held-out
data.

| Set | Utterances | WER, INT16 variant | Float32 | Change against float32, 95% interval | Deployed arithmetic | Change against deployed |
| --- | ---: | ---: | ---: | --- | ---: | --- |
| dev-other | 2,864 | 8.067% | 8.073% | −0.07% [−0.64, +0.50] | 8.042% | +0.32% [−0.05, +0.69] |
| dev-clean | 2,703 | 3.630% | 3.619% | +0.30% [−0.54, +1.12] | 3.638% | −0.20% [−0.85, +0.42] |
| Device replay of dev-other | 600 | 10.456% | 10.466% | −0.09% [−1.27, +1.09] | 10.513% | −0.53% [−1.56, +0.55] |

All three sets pass. The simulation does not model the NPU's own rounding
beyond the integer path that the INT16 study confirmed, or the transfer of
rows that do not share one input scale; it does not establish recognition on
the board.

## Consequences for running mimus on the NPU

- Exact sums need four times the NPU multiply-accumulates of the INT16 layer
  and return 24 partials per output channel, 24 times the INT16 output bytes.
  They pay off only after the session runtime drops its per-job verification
  and copies.
- The INT16 layer rounds each output once and quantizes inputs with one
  static scale per site. It changes recognition results and does not replace
  the shipped model or the CPU kernel.
- The [session runtime](npu-session.md) can already run the compiled INT16
  query projection beside other models, with modeled device output. The digit
  graph would be added to the catalogue only after a runtime that passes the
  comparison above.

Board evidence would confirm the simulator result. One scheduled NPU session
would run the catalogued digit graph on the study rows and the range fixture.
It passes when the partials are byte-equal to the simulator's and completion
arrives by interrupt. The same session would record the job time from SUBMIT
to the copied output beside the A76 kernel's time for the same 8 rows.

## Reproduce

Inside the pinned compiler image, with this repository at `/source`, the
mimus benchmark inputs at `/mimus-bench` and a writable `/work`:

```sh
L=encoder.layers.0.self_attn.linear_q
bash /source/tools/npu-mimus-linear.sh /mimus-bench/base.mimus /work/q-int16 $L
bash /source/tools/npu-mimus-linear.sh /mimus-bench/base.mimus /work/q-digits $L --digits
bash /source/tools/npu-mimus-linear.sh /mimus-bench/base.mimus /work/q-range $L --digits --range
bash /source/tools/npu-mimus-linear.sh /mimus-bench/base.mimus /work/q-unit $L --unit
python3 /source/tools/npu-mimus-probe.py patch /work/q-unit/export_nbg_unify/network_binary.nb \
  /work/patched.nb --code 5 --wide 4
```

Then, on the host, against a mimus checkout:

```sh
clang++-21 -std=c++20 -O1 -ffp-contract=off -I MIMUS/firmware/components/asr/include \
  checks/npu-mimus-kernel.cc checks/npu-mimus-digits.cc \
  MIMUS/firmware/components/asr/src/gemm_combine_portable.cc -o kernel
./kernel STUDY_DIRECTORY
clang-21 -std=c11 -O1 -Iinclude checks/npu-mimus-cost.c drivers/npu/nbg_model.c \
  drivers/npu/nbg_assembly.c drivers/npu/nbg_relocation.c drivers/npu/nbg_checksum.c -o cost
./cost INT16.nb DIGIT.nb > events.jsonl
```

The instruction counts use an ELF linked with `ld.lld` from
`checks/npu-mimus-digits.cc` and the mimus `port/sunstoneos` branch's
`gemm_combine_portable.cc`, both compiled with the backend's flags (the
shared C++ options, then `-march=armv8.2-a+nolse -mtune=cortex-a55
-mno-strict-align -fbuiltin -DMIMUS_AARCH64_NEON=1`):

```sh
python tools/npu-mimus-cost.py --elf counts.elf --study q-digits --events events.jsonl \
  --int16-log q-int16/export.log --digit-log q-digits/export.log --report cost.json
```

The study used mimus revision `46d8e64aa5eb841f19c4205113b030e501b72c7a` for the
portable kernel and `3e05158e31cb3de952f77526794f6772281e3668` of its `port/sunstoneos`
branch for the AArch64 kernel. Results, graphs, probe copies and simulator logs are under
`~/.cache/sunstoneos/evidence/npu-exact-sum-20261022/`. The simulator models
the target's revision-9.2 engine; physical NPU arithmetic, timing and
confinement are untested.
