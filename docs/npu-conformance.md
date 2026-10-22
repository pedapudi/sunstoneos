# NPU output conformance with the SDK simulator

The conformance harness is **implemented** and passes on the host. For every
model in the session catalogue it runs a fixed set of input vectors through
the pinned SDK simulator of the A733 NPU (the revision 9.2 engine that the
[compiler record](npu-compiler.md) pins) and compares the output bytes with an
independent reference. All four models built by the pinned compiler match
their references bit for bit on every vector. The comparison of board output
with the simulator is **pending**: no board application produces captures
yet. Physical NPU arithmetic, cache visibility and interrupt delivery remain
unqualified.

The harness is [npu-conformance.py](../tools/npu-conformance.py). It runs on
the host, verifies every pinned file, and starts one container from the
compiler image with no network and no devices. The simulator program is
[npu-conformance-runner.c](../tools/npu-conformance-runner.c), and the
references and the files they read are pinned in
[npu-conformance-references.json](../tools/npu-conformance-references.json).
[The design](designs/npu-conformance.md) records the reasoning and the budgets.
The harness never contacts USB devices or the board.

## Running it

The compiler image from [npu-compiler.lock.json](../tools/npu-compiler.lock.json)
must be loaded in Docker, and the pinned files must be present under
`~/.cache/sunstoneos` (or `SUNSTONE_CACHE`). Run, with a new output directory:

```sh
PYTHONDONTWRITEBYTECODE=1 ~/.cache/sunstoneos/venv/bin/python \
    tools/npu-conformance.py run ~/.cache/sunstoneos/build/npu-conformance/RUN
```

The command prints each model's state and exits with status 0 only when every
model passes. `RUN/report.json` holds the full result; `RUN/work/` holds the
staged inputs, every job's input, output and consumed-input files, the
reference dumps and the logs.

The container runs as its root user with every capability dropped, a 32 GiB
memory limit and six CPUs. It writes only to `RUN/work`, which the harness
makes world-writable for that purpose; the staged inputs inside it stay
read-only to the container.

## Models and references

The model set is the [session catalogue](../tools/npu-session-models.json):

| Model | Tensor types | Reference | Pinned files |
| --- | --- | --- | --- |
| 8×8 convolution (`conv8`) | UINT8 | Compiler quantized CPU inference | Imported model JSON and data, quantize file, input description |
| Two convolutions (`dual-u8`) | UINT8 | Compiler quantized CPU inference | As above |
| Same graph, INT8 (`dual-i8`) | INT8 | Compiler quantized CPU inference | As above |
| mimus query projection (`mimus-linear-q`) | INT16 | Exact integer accumulator, bias as an int32 code, one rounding to INT16 | mimus model file, weight codes and scales, bias, study metadata, quantize file, `tools/npu-mimus-linear.py` |
| Published ShuffleNetV2 | UINT8 | None | None |

- **Compiler inference.** For each base vector, the harness converts the codes
  to floats with the catalogue's scale and zero point, runs the compiler's
  quantized CPU inference on them, and requires the compiler's dump of its
  quantized input to equal the vector. The scale, zero point, type and shape
  in the pinned quantize and input-description files must equal the
  catalogue's.
- **mimus rule.** The harness applies the int32-bias rule of
  [npu-mimus-linear.py](../tools/npu-mimus-linear.py). It forms the exact
  integer sum over the INT16 input codes and int8 weight codes and adds the
  bias as an int32 code in units of the input scale times the row's weight
  scale. One rounding, half to even, gives the INT16 output. Before using the pinned
  weight, scale and bias files, it requires them to equal the layer read from
  the pinned mimus model file.
- **ShuffleNetV2.** Another compiler produced it, and no source model or
  quantize file exists. It runs in the simulator and its outputs are reported,
  but no arithmetic claim rests on it.

For the catalogued input of every model, the simulator output must also equal
the catalogue's output identity. For mimus, that input is the study's saved
simulator input, so the first base vector repeats the study's result.

## Vectors and job order

Each model gets up to five base vectors of raw tensor codes, in this order:

1. the catalogued reference input;
2. every code at the input's zero point;
3. every code at the type's minimum;
4. every code at the type's maximum;
5. seeded bytes.

A base vector equal to an earlier one runs once. ShuffleNetV2's input zero
point equals its minimum code, so it has four base vectors.

Seeded bytes are SHA-256 in counter mode. Block *i* is the digest of the model
name, a zero byte, the vector index in decimal, a zero byte, and *i* as eight
little-endian bytes. The seeded base vector uses vector index 4. For the
input-line vectors described next, the line index in decimal and a zero byte
follow the vector index, which is 5.

Each model built by the pinned compiler then gets one input-line vector for
every 64-byte line of every input. Each equals the previous job's vector
except in its line, which receives seeded bytes; the first one starts from the
seeded base vector. The session runtime aligns every allocation to 4 KiB
([npu-session.md](npu-session.md)), so lines counted from a tensor's first
byte are physical cache lines. The inputs span 3 lines for `conv8`, 3 and 1
for each two-convolution graph, and 44 for mimus: 55 input-line vectors in
all. The last job of every model repeats the seeded base vector, and the two
outputs must be equal; this detects simulator nondeterminism.

The runner fills every output tensor with the byte `0x5a` before each job, so
a job that leaves an output unwritten cannot pass with the previous job's
bytes. It reads every input back from the simulator after the run; that
consumed input must equal the vector.

The report gives each model's **board job order**: the base vectors arranged
so that the most consecutive pairs have different expected outputs, with the
seeded vector last, followed by the input-line vectors and the repeat. A
board application that runs this order can compare its captures with the
stored simulator outputs.

A line is **covered** when some consecutive pair of jobs in the board order
changes that line and has different expected outputs. Every input and output
line of the four pinned-compiler models is covered.

## Result

The run below used the files pinned at this revision. Every comparison
requires equality; no tolerance entry exists.

| Model | Jobs | Base vectors | Differing codes | Unsaturated base vectors | Input / output lines covered | Mean / maximum job time |
| --- | ---: | ---: | ---: | ---: | --- | --- |
| `conv8` | 9 | 5 | 0 | 5 | 3 of 3 / 2 of 2 | 0.4 ms / 0.8 ms |
| `dual-u8` | 10 | 5 | 0 | 5 | 4 of 4 / 3 of 3 | 0.7 ms / 1.9 ms |
| `dual-i8` | 10 | 5 | 0 | 5 | 4 of 4 / 3 of 3 | 0.2 ms / 0.8 ms |
| `mimus-linear-q` | 50 | 5 | 0 | 5 | 44 of 44 / 44 of 44 | 18 ms / 87 ms |
| ShuffleNetV2 | 5 | 4 | no reference | 1 | not planned | 8.9 s / 10.2 s |

- **mimus.** The int32-bias rule matches all 1,408 output codes of every base
  vector, including the vectors whose codes all sit at an INT16 limit.
- **ShuffleNetV2.** The catalogued input gives `[0, 255]`, every code at zero
  gives `[255, 0]`, the seeded vector gives `[255, 0]`, and every code at 255
  gives `[251, 5]`. The last output is unsaturated, so a board capture of that
  vector would compare unclamped arithmetic of ShuffleNetV2's layers. Without
  a reference, it supports no arithmetic claim in the simulator.

### Operator coverage

The SDK inspector `nbinfo` 2.0.0.b7fb7b62 reads the layer table of all five
models, including ShuffleNetV2's format `0x1001e`.

| Model | Hardware layers | Operation kinds |
| --- | --- | --- |
| `conv8`, `dual-u8`, `dual-i8` | ConvolutionReluPoolingLayer2 | Init, NN |
| `mimus-linear-q` | ConvolutionReluPoolingLayer2, TensorTranspose | Init, NN, NT |
| ShuffleNetV2 | ConcatLayer, ConvolutionReluPoolingLayer2, TensorCopy, TensorTranspose | Init, NN |

- **Simulator against reference.** ConvolutionReluPoolingLayer2 and
  TensorTranspose, with UINT8, INT8 and INT16 tensors.
- **Uncovered.** ConcatLayer and TensorCopy occur only in ShuffleNetV2, so no
  reference comparison covers them. A catalogue graph that contains them, with
  a reference, would cover them.
- **Native.** Reserved case 0 of [npu-session.py](../checks/npu-session.py)
  delivers output bytes for all five models through the session runtime in
  QEMU. Its register model derives an output from input bytes only for a
  neural descriptor that reads a model input and writes a model output. Native
  input delivery is therefore checked only for the first bytes of each input
  of `conv8` and the two-convolution graphs, and not at all for ShuffleNetV2
  or mimus, whose output descriptors read intermediate memory.
- **Board.** Pending.

## Cost and execution

The harness starts one container per run. Inside it, one pool of at most six
processes, under `nice -n 10`, builds the runners and runs them, the compiler
inferences and the inspector. Each runner invocation runs all of a model's
jobs on one prepared graph with the environment that the fixture pipelines
export (`VSIMULATOR_CONFIG=VIP9000NANODI_PLUS_PID0X1000003B`,
`VSIMULATOR_SHADER_CORE_COUNT=1`).

The baseline is the fixture pipeline at the base revision, run in the same
image with six CPUs: [npu-conv-pipeline.sh](../tools/npu-conv-pipeline.sh)
took 13.9 s and [npu-dual-pipeline.sh](../tools/npu-dual-pipeline.sh) 13.0 s.
Each invocation runs one simulator job, one runner build and six compiler
commands (import, input description, float inference, quantize, quantized
inference, export), and reproduces the catalogued model bytes.

| Measure | Fixture pipeline, per invocation | Conformance run | Budget |
| --- | --- | --- | --- |
| Simulator jobs | 1 | 84: 24 base, 55 input-line, 5 repeats | At most 25, 55 and 5 |
| Runner builds | 1 | 5 | One per model |
| Runner invocations | 1 | 5 | One per model |
| Compiler commands | 6 | 15 quantized inferences | At most 15 |
| Peak concurrent processes | 1 | 6 | At most 6 |
| Container starts | 1 | 1 | 1 |
| Wall time | 13.0–13.9 s | 47.1 s | Recorded, no budget |

Three passing runs took 47.1, 47.4 and 47.7 s. ShuffleNetV2's runner invocation, at
46.5 s for four jobs and a repeat, is the critical path; the 15 compiler
inferences take 1.7–2.2 s each and the five runner builds 0.2 s each. The
largest single process used 4.4 GiB.

**The harness keeps no result cache.** The design allowed one only if a cold
run took 60 s or more. Every run therefore repeats all 84 simulator jobs, and
a second run is as long as the first. Without a cache the runs share no state:
each has its own output directory and container name, so the design's lock on
a cache directory is not needed.

## Failure handling

The run fails, and reports every model's state, on any of these:

- a runner, reference or inspector failure, or a timeout;
- a wrong output size;
- a consumed input that differs from the vector;
- a missing or changed pinned file;
- a repeated vector with different outputs;
- a simulator output for the catalogued input that differs from the catalogue;
- any differing code.

A failing comparison
reports the number of differing codes, at most 16 of their positions and the
largest code difference for each output.

Time limits:

- **Runner build:** 120 s per model.
- **Compiler inference:** 120 s per vector.
- **Inspector:** 60 s per model.
- **Runner invocation:** the one-vector limit of the model's pipeline (120 s
  for the convolution graphs, 600 s for mimus and ShuffleNetV2) plus four times
  the measured per-vector time for each further job. The per-vector times in
  the reference table are 0.01 s for the convolution graphs, 0.1 s for mimus
  and 10.5 s for ShuffleNetV2.
- **Container:** the sum of the step limits plus 120 s, 4,569 s for this
  catalogue. On expiry the harness runs `docker kill` and removes the
  container.

After the container exits, the harness hashes the staged model and reference
files again and fails the run if any changed.

The host check [npu-conformance.py](../checks/npu-conformance.py) runs without
the image or the cache. It checks:

- the 84-job plan and the 55 input-line vectors for the catalogue, the seeded
  bytes and each line vector's single changed line;
- line coverage and the board job order;
- the int32-bias rule, including half-to-even rounding and clamping;
- the verdict for each failure listed above;
- the generated runner bindings and the time limits;
- the refusal of every changed or missing pinned file.

## Board evidence that would confirm conformance

A board session would write one directory with `capture.json` and the output
files, named as the simulator's `output<i>.bin` files are. `capture.json`
would record the image SHA-256, the source hashes of the session runtime,
service and driver, and the cache maintenance form (whole buffer or written
range). It would hold one row per job with the model, NBG SHA-256, input
SHA-256, each output file's SHA-256 and size, job order, completion mode,
interrupt count and session fault state.

Conformance holds when every board output equals the stored simulator output
for every catalogue model and vector in the board job order, every job
completes through the interrupt path with one NPU interrupt, and no session
retains a fault. Cache visibility would then be recorded as "no staleness
observed" for the maintenance form and runtime source hash in the capture.
Because `a733_npu_memory_publish` and `a733_npu_memory_finish` each invalidate
the output buffer, a job pair detects only the loss of both operations or a
wrong memory attribute on the output buffer. Per-operation coverage comes
from [npu-memory.c](../checks/npu-memory.c). The capture reader will be written
when a board application produces files; the producer is listed under
[remaining work](npu-session.md#remaining-work).

## Remaining work

- A board application and capture, as above.
- In `checks/npu-session.c`, `execute()` should compare, at completion, every
  byte of each model input that no output-producing descriptor reads with the
  bytes the client submitted. ShuffleNetV2 and mimus have no native input
  check until then.
- `tools/npu-mimus-linear.py` should expose the int32-bias rule as a function
  with unchanged behaviour; the harness would then import it in place of its
  own copy, `mimus_int32_bias`.
- `checks/npu-conv-reference.py` and `checks/npu-dual-reference.py` can drop
  their simulator comparisons: this harness compares the same fixtures on the
  same catalogued inputs and four more vectors each.
- A reference for ConcatLayer and TensorCopy needs a pinned-compiler catalogue
  graph that contains them.
