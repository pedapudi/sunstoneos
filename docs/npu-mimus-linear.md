# One mimus linear layer on the NPU compiler and simulator

This is a **research result** from the pinned host compiler and SDK simulator;
no board or NPU executed anything. The NPU path reproduces a mimus int8 linear
layer's integer arithmetic exactly, but its output is an INT16 code, so it is
not bit-exact with the mimus float32 result. Handing the exact integer sums
to the CPU would make the combination bit-exact, and the pinned compiler
refuses the INT32 output that this needs.

## mimus computes an exact integer sum, then a float read-out

The P4 build of mimus, the speech recognizer this project runs, stores each
linear layer's weights as int8 codes with one fp16 scale `ws[n]` per output
row. Its int16 activation mode quantizes each input row `x` with a float32
scale `s = max|x| / 32767`, multiplies by `1 / s` in float32 and rounds half to
even, clamped to ±32767 (mimus `train/quant.py`). The portable kernel
`MimusGemmS16FC` then forms the exact integer sum over the codes and computes,
in float32 and in this order, `(float(sum) * ws[n]) * s + bias[n]`.

## The layer is built from the mimus codes and scales

[npu-mimus-linear.py](../tools/npu-mimus-linear.py) reads a mimus model file
directly, using the layout documented in mimus `train/model_format.py`, and
checks every tensor CRC. For one layer it writes an ONNX MatMul plus Add whose
float weights are the int8 codes times their scales; these products are exact
in float32, and dividing by the scale recovers the codes. It generates eight
test rows that share one maximum, so all rows have the same activation scale,
which a static NPU input quantization can represent. It quantizes the rows
with the mimus rule and computes the mimus result.

[npu-mimus-linear.sh](../tools/npu-mimus-linear.sh) runs inside the pinned
compiler image. It imports the ONNX graph and lets the compiler create a
quantize file. It then replaces every entry with mimus values:

- INT16 input with the shared activation scale and zero point 0.
- Per-channel symmetric int8 weights with the mimus `ws[n]`.
- Int32 bias in units of the activation scale times `ws[n]`.
- An INT16 output scaled to the result range. It exports the network binary graph
and runs it in the SDK simulator on a file holding the exact mimus activation
codes; the check confirms that the simulator's input equals those codes. The
compiler emits a neural-transpose step, one neural-network operation and a
second transpose step, with intermediates in internal SRAM.

[npu-mimus-kernel.cc](../checks/npu-mimus-kernel.cc) compiles the unmodified
mimus portable kernel on the host and runs it on the same codes, weights,
scales and bias. For every studied layer its float32 output is bit-identical
to the study's reference, so the comparison below is against mimus's own
kernel arithmetic.

## The integer path is exact; the output is rounded once to INT16

The input is mimus model file SHA-256
`f73ccd024645387d2679eea97ae11d0c33f5e4be81b39a9430d1f5d285e8089d`, the
benchmark's P4 base model, with test seed 20261021 and row maximum 4.0. Three
layers of encoder block 0 were compiled:

| Layer | K × N | Largest &#124;sum&#124; | Codes equal to one rounding of the exact result | Equal with int32 bias | Largest error / output step |
| --- | --- | ---: | ---: | ---: | --- |
| Attention query projection | 176 × 176 | 22,917,137 | 1,408 of 1,408 | 1,408 of 1,408 | 0.000233 / 0.000467 |
| First feed-forward, first linear | 176 × 704 | 25,538,357 | 5,630 of 5,632 | 5,632 of 5,632 | 0.00211 / 0.00421 |
| First feed-forward, second linear | 704 × 176 | 30,775,865 | 1,406 of 1,408 | 1,408 of 1,408 | 0.00276 / 0.00552 |

The second column counts output codes equal to rounding the exact real value
`sum · ws[n] · s + bias[n]` once to the output step. The third uses the bias
as the NPU holds it: an int32 code in accumulator units, the product of the
activation scale and `ws[n]`. With that bias every simulator code equals one
rounding of the exact accumulator, so the four codes that differ by one sit
within 0.0004 of a rounding boundary and are explained by bias quantization.
The NPU does not lose precision in the sum itself. Measured against the mimus
float32 output, the largest error is half an output step, and no layer is
bit-exact.

## Exact sums for a CPU read-out are not available from the compiler

A bit-exact result would need the NPU to return each integer sum, which
exceeds the INT16 range at these sizes, so that the CPU could apply the mimus
read-out. The tool's `--raw-sums` form builds the layer from the codes with
unit scales, no bias and an INT32 output. Network-binary generation in the
pinned compiler refuses it for the query projection and the second
feed-forward linear with "Fatal model generation error: 64768". INT16 output
codes at unit scale would saturate: one product of an int16 activation and an
int8 weight can already exceed 32,767.

## Consequences for running mimus on the NPU

- A static NBG has one input scale, while mimus quantizes every row with its
  own scale. A layer on the NPU needs rows rescaled to one shared scale or a
  separate graph per scale; either changes mimus's arithmetic.
- Outputs carry INT16 rounding of half a step and int32 bias rounding, so
  NPU layers change recognition results slightly and need an accuracy
  evaluation before replacing the CPU kernel.
- The [session runtime](npu-session.md) can already run the compiled query
  projection beside other models, with modeled device output.

## Reproduce

Inside the pinned compiler image, with this repository at `/source`, the
mimus benchmark inputs at `/mimus-bench` and a writable `/work`:

```sh
bash /source/tools/npu-mimus-linear.sh /mimus-bench/base.mimus \
  /work/mimus-encoder.layers.0.self_attn.linear_q encoder.layers.0.self_attn.linear_q
```

Then, on the host, against a mimus checkout:

```sh
clang++-21 -std=c++20 -O1 -I MIMUS/firmware/components/asr/include \
  checks/npu-mimus-kernel.cc MIMUS/firmware/components/asr/src/gemm_combine_portable.cc -o kernel
./kernel STUDY_DIRECTORY
```

The study used mimus revision `46d8e64aa5eb841f19c4205113b030e501b72c7a` for the
portable kernel. Results, compiled graphs, simulator logs and the INT32
refusal logs are under `~/.cache/sunstoneos/build/audit-npu-runtime/compiler/work/`
in the `mimus-*` and `raw-*` directories. The simulator models the target's
revision-9.2 engine; physical NPU arithmetic, timing and confinement are
untested.
