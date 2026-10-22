# Design: exact integer sums from the NPU for mimus linear layers

Status: **design; no code.** All work runs on the host: the pinned compiler,
the SDK simulator, the native NBG decoder and assembler, and Unicorn
instruction emulation. No board or NPU executes anything. Each result ends at
implemented and host-modeled, with physical behavior pending.

## What a bit-exact layer needs

A mimus linear layer multiplies int16 activation codes `x` (|x| ≤ 32,767,
one float32 scale `s` per row) by int8 weight codes `w` (|w| ≤ 127 in the
benchmark model, one fp16 scale `ws[n]` per output channel). It forms the
exact integer sum `S[n] = Σ_k x[k]·w[k,n]` over the reduction length `K`, then
computes `(float(S[n]) * ws[n]) * s + bias[n]` in float32
([npu-mimus-linear.md](../npu-mimus-linear.md)). The NPU can match this only
by returning `S` exactly to the CPU, which then applies that read-out
unchanged.

Two constraints follow from the model file:

- **Range.** |S| ≤ 4,161,409·K. A signed 32-bit output holds that bound only
  for K ≤ 516. The benchmark model has 92 int8 layers; 74 have K of 9 or 176,
  and 18 have K of 704, 1,584 or 3,520. mimus itself accumulates in 64 bits
  and converts through int32 only for layers it marks `fits32`.
- **Scale.** A compiled NBG has one static input scale, while mimus scales each
  row separately. A layer that takes codes at unit scale and leaves `s` to the
  CPU read-out avoids this conflict.

## Determination in three steps

**1. Probe for an INT32 output encoding.** Two observations already point to a
refusal. The SDK's graph verification returns status −3 ("cannot be
supported") for the INT32 form of the query projection and the second
feed-forward linear. The pinned SDK's NN configuration counts NN cores for
int8, int16, uint16, float16 and bfloat16 data, and no 32-bit type. The probe
settles the question from the descriptor itself:

- Compile the query projection three times, with UINT8, INT8 and INT16 outputs
  and everything else equal. Diff the 192-byte NN records and the
  neural-transpose records to locate the output data-type, post-multiplier and
  post-shift fields.
- Write each unused data-type code into a copy of the INT16 graph, recompute
  the checksums with `checks/nbg-checksum-reference.py`, and run the copy in
  the simulator.
- Accept a code only if the simulator returns every sum of the three studied
  layers exactly. An accepted code still covers only the 74 layers with
  K ≤ 516.

A refusal is recorded in [npu-mimus-linear.md](../npu-mimus-linear.md) with
the field offsets and simulator logs. The simulator models the revision-9.2
engine, so an accepted code would still need board confirmation.

**2. Exact partial sums at unit scale.** The NN engine computes integer sums
exactly and rounds once at output ([npu-mimus-linear.md](../npu-mimus-linear.md)).
At unit output scale with no bias, an INT16 output is therefore exact whenever
its sum lies within ±32,767. The layer is compiled so that every output stays
within that range:

- The CPU splits each code into four balanced base-16 digits,
  `x = Σ_{j=0..3} 16^j·d_j` with |d_j| ≤ 8. Four digits reach ±34,952.
- The reduction is cut into C chunks of at most 32 terms. Each partial sum
  `P[j,c,n] = Σ_{k in chunk c} d_j[k]·w[k,n]` is then bounded by
  32·8·127 = 32,512.
- The graph has one INT8 input and one fully connected layer per chunk, each
  with unit scales, zero points of 0, no bias and an INT16 output. The digits
  of a row become four rows of the batch, so each weight is stored once.
- The CPU forms `S[n] = Σ_j 16^j·Σ_c P[j,c,n]` in 64-bit integers and applies
  the mimus read-out with the row's own `s`.

For the query projection (K = N = 176), six chunks of 29 or 30 terms give
six inputs, six outputs and 24 partial sums per output channel. That fits the existing
general assembler (`nbg_assemble_layout`: at most eight inputs and eight
outputs, NN and neural-transpose operations). The NPU performs four times the
multiply-accumulates of the INT16 layer. Base-4 digits would give 16 partial
sums instead of 24 but double the NPU work, so base 16 is the default and the
tool takes the base as a parameter. Layers with K of 704 or more need
more than eight chunks and are not built in this package.

The tool `tools/npu-mimus-linear.py` gains a `--digits` form that writes this
graph, the digit input and the recombination reference. `checks/npu-mimus-kernel.cc`
recombines the simulator's partials and compares the read-out bytes with the
unmodified mimus kernel `MimusGemmS16FC`. A range fixture drives every
partial to its largest magnitude for this chunking, +30,480 and −30,480, to
show that the output stage neither saturates nor rounds there.

**3. Fallback: measure the INT16 path.** If step 2 is not bit-exact in the
simulator, or misses a budget below, the INT16 graph becomes a separate
recorded variant, and the shipped model and CPU kernel stay unchanged. A host
fake-quantization model of the NPU arithmetic replaces every linear layer in
the mimus PyTorch simulation. It models a static per-layer input scale with
saturation, an int32 bias code, one rounding to an INT16 output step, and the
CPU dequantization. Calibration of the static scales uses a slice of
train-clean-100. The comparison applies the mimus accuracy bar from
`docs/int8_activations.md`: a paired bootstrap with `train/compare_wer.py`
over 10,000 resamples on dev-other, dev-clean and replay. The variant passes a
set when the 95% interval of its relative WER change contains 0 and its
point estimate is at most +1% relative, against fp32. mimus forbids running
test-clean and test-other except at a milestone the owner calls, so the
evaluation uses those three dev sets as its held-out data.

## Data flow, ownership and cache policy

The runtime form of the exact layer reuses the NPU session runtime
([npu-session.md](../npu-session.md)) without change. One job runs one layer
for the rows of one encoder chunk.

1. The mimus client task quantizes each row as it does today, then writes the
   digits straight into the session's input buffers (READ, write-back DDR in
   the extended cached profile). This is the only copy of input data: 704
   bytes per row in place of 352 bytes of INT16 codes.
2. The client cleans by virtual address only the bytes it wrote, then submits.
   The buffers are device-owned until completion.
3. The NPU reads the input and the coefficients (CONSTANT, verified once when
   the session is admitted) and writes partials to the output buffers (WRITE).
   Intermediates stay in internal SRAM, as in the compiled INT16 layer.
4. On completion the client invalidates only the output bytes the job wrote,
   recombines the partials in place, and writes the float32 result into the
   mimus activation buffer. The output buffers return to CPU ownership with
   no output copy.

Completion uses the session's interrupt binding (`npu_session_bind_irq`) and
the held `NPU_SERVICE_WAIT` reply; this design adds no tick wait or status
poll. The client task runs on core 6 or 7 once the multicore work admits the
A76 cores; the NPU service worker stays on core 0. The two cores exchange only
the existing service queue messages and the buffers whose ownership the
submit and completion hand over. This design adds no lock and no state
written by more than one core.

Failure handling is unchanged from the session runtime. Within this package,
a compiler refusal, a simulator mismatch or a missed budget is a recorded
result and touches no device.

## Reuse and removal

- **Reused:** the fixture, quantize-file and comparison stages of
  `tools/npu-mimus-linear.py` and `tools/npu-mimus-linear.sh`; the host build
  of the unmodified mimus kernel in `checks/npu-mimus-kernel.cc`;
  `nbg_model_open` and `nbg_assemble_layout` to confirm that the new graph
  needs no assembler change; the checksum reference for the probe; the mimus
  Unicorn harness `tools/aarch64_kernel_check.py` for instruction counts.
- **Removed:** the `--raw-sums` INT32 form of the tool. The descriptor probe
  replaces it as the evidence for INT32 output, and its refusal logs stay in
  the build cache.
- **Assembler changes:** none expected. If the compiler emits an operation the
  assembler refuses, the package records the refusal and stops; extending the
  assembler needs its own design.

## Performance budgets

Figures are per activation row for the query projection, the layer this
package builds. Rows per job scale them linearly.

| Metric | Baseline | Budget | Measured by |
| --- | --- | --- | --- |
| Read-out bytes equal to `MimusGemmS16FC` | No studied layer bit-exact; largest error half an output step | All 8,448 outputs of the three studied layers, plus the range fixture | Simulator output recombined in `checks/npu-mimus-kernel.cc` |
| AArch64 instructions for digit split and recombination | NEON packed kernel for the same layer: not yet counted, estimated 15,000 at about 0.5 instructions per multiply-accumulate | At most 25% of the counted kernel figure | Unicorn instruction hook in the mimus harness; both functions compiled with the backend's flags |
| NN multiply-accumulates | 30,976 (counted, INT16 layer) | At most 123,904 | Tensor shapes and NN operation count of the compiled graph |
| Input bytes written by the CPU | 352 | At most 704, one copy | Graph input sizes |
| Output bytes read by the CPU | 352 | At most 8,448, read in place, no copy | Graph output sizes |
| Cache maintenance per job | Whole READ and WRITE buffers (section 3 of the [performance architecture](../performance-architecture.md)) | Clean 704 and invalidate 8,448 bytes per row | Stated here; counted when the session host model registers the layer |
| Queue round trips per job | Up to three | Two: submit and completion | Session host model, unchanged |

If the instruction budget fails, the exact path offloads too little CPU work
to justify four times the NPU work, and step 3 runs instead. The read-out
itself is identical in both paths and is excluded from the count.

## Left out

- **A shader kernel that accumulates in 32 or 64 bits.** The SDK has no such
  kernel for this layer, the target has one shader core, and the project
  treats shader instructions as opaque compiler output. No throughput
  advantage over the A76 NEON kernel is established.
- **A two-pass high-part and residual scheme.** It needs the per-row high part
  as a bias. Bias is per output channel, and rewriting coefficients per job
  would undo verification at admission.
- **FP16 output.** Its 11-bit significand cannot hold the sums.
- **Layers with K of 704 or more, session catalogue registration and the
  mimus engine change.** The catalogue and the mimus engine belong to other
  owners; they follow once one layer passes here.
- **Board runs.** Physical confirmation needs one scheduled NPU session that
  runs the registered exact layer on the fixed fixture and the range fixture.
  It passes when the output partials are byte-equal to the simulator's and
  completion arrives by interrupt.
