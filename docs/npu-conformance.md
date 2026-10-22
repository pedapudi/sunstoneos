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
staged inputs, every job's input, output and input read-back files, the
reference dumps and the logs.

The container runs as its root user with every capability dropped, six CPUs
and a 14 GiB memory limit with no swap. The SDK lies under the image's
`/root`, which only root can read, so the container cannot run as the host
user. It writes only to `RUN/work`, which the harness makes world-writable
for that purpose. The harness mounts the staged inputs in `RUN/work/inputs`
read-only and creates `RUN` with mode 0700, so no other local user can reach
the run's files.

## Models and references

The model set is the [session catalogue](../tools/npu-session-models.json):

| Model | Tensor types | Reference | Pinned files |
| --- | --- | --- | --- |
| 8×8 convolution (`conv8`) | UINT8 | Compiler quantized CPU inference | Imported model JSON and data, quantize file, input description |
| Two convolutions (`dual-u8`) | UINT8 | Compiler quantized CPU inference | As above |
| Same graph, INT8 (`dual-i8`) | INT8 | Compiler quantized CPU inference | As above |
| mimus query projection (`mimus-linear-q`) | INT16 | Exact integer accumulator, bias as an int32 code, one rounding to INT16 | Weight codes and scales, bias, study metadata, quantize file, `tools/npu-mimus-linear.py` |
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
  scale. One rounding, half to even, gives the INT16 output. The rule sums raw
  input codes, so the harness requires the catalogue's input zero point to be 0
  and its input scale to equal the study's. The weight codes, weight scales
  and bias come from the study's derived files. When their pins were recorded,
  the three files equalled the layer read from the mimus model file, whose
  SHA-256 the pinned study metadata records. The harness carries its own copy
  of the rule until the study script exposes it as a function; the SHA-256 pin
  on `tools/npu-mimus-linear.py` refuses a run after any change to the
  script.
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
all.

The last job of every model repeats one base vector, and the two outputs must
be equal and unsaturated; this detects simulator nondeterminism. The models
from the pinned compiler repeat the seeded vector. ShuffleNetV2 repeats the
maximum vector, because its seeded output `[255, 0]` sits at the clamp limits
and would change only if nondeterminism flipped the class.

The runner fills every output tensor with the byte `0x5a` before each job, so
a job that leaves an output unwritten cannot pass with the previous job's
bytes. After each run it reads every input tensor back, and that read-back
must equal the vector. The read-back shows that the input tensor still held
the vector after the run; it does not show which bytes the simulated engine
read.

The **board job order** is the job order above: base vectors, input-line
vectors, then the repeat. A board application that runs this order can
compare its captures with the stored simulator outputs. The report lists every
consecutive pair with equal expected outputs; the run below has none.

Coverage of a 64-byte line is defined per tensor role:

- **Input line.** Covered when its input-line job's expected output differs
  from the preceding job's. If only that line were stale on the board, the
  NPU would read the preceding vector and produce the preceding output, so the
  capture would differ from the stored output. A pair that changes many lines
  at once gives no such guarantee, so base-vector pairs cover no input line.
- **Output line.** Covered when some consecutive pair of jobs has expected
  outputs that differ in that line; a stale output line holds the preceding
  job's bytes.

An uncovered line of a pinned-compiler model fails the run. Every input and
output line of the four pinned-compiler models is covered.

## Result

The run below used the files pinned at this revision. Every comparison
requires equality; no tolerance entry exists.

| Model | Jobs | Base vectors | Differing codes | Unsaturated base vectors | Input / output lines covered | First job | Later jobs, mean / maximum | Runner start-up and release |
| --- | ---: | ---: | ---: | ---: | --- | ---: | --- | ---: |
| `conv8` | 9 | 5 | 0 | 5 | 3 of 3 / 2 of 2 | 2.6 ms | 0.8 ms / 1.0 ms | 2.0 s |
| `dual-u8` | 10 | 5 | 0 | 5 | 4 of 4 / 3 of 3 | 1.0 ms | 0.4 ms / 0.5 ms | 2.8 s |
| `dual-i8` | 10 | 5 | 0 | 5 | 4 of 4 / 3 of 3 | 2.9 ms | 0.7 ms / 1.1 ms | 2.0 s |
| `mimus-linear-q` | 50 | 5 | 0 | 5 | 44 of 44 / 44 of 44 | 17 ms | 17 ms / 97 ms | 2.3 s |
| ShuffleNetV2 | 5 | 4 | no reference | 1 | not planned | 9.4 s | 8.50 s / 8.56 s | 2.4 s |

The runner's first `RunGraph` call also completes the graph's preparation, so
the first job is reported apart from the later jobs; graph setup and
verification before it take 5–6 ms. The last column is the invocation's wall
time less every time the runner measures: process start-up, simulator set-up,
graph release and exit.

- **mimus.** The int32-bias rule matches all 1,408 output codes of every base
  vector, including the vectors whose codes all sit at an INT16 limit.
- **ShuffleNetV2.** The catalogued input gives `[0, 255]`, every code at zero
  gives `[255, 0]`, the seeded vector gives `[255, 0]`, and every code at 255
  gives `[251, 5]`, as does its repeat. That output is unsaturated, so a board
  capture of that vector would compare unclamped arithmetic of ShuffleNetV2's
  layers. Without a reference, it supports no arithmetic claim in the
  simulator.

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

Each runner process holds about 4.4 GiB from start-up, whatever its graph
size, so at most two run at once. They run in two lanes: the model with the
longest expected invocation (per-vector time times job count) takes one lane
alone, and the others run in turn in the second. That puts ShuffleNetV2 in
one lane and the four short models, about 10 s in all, in the other. The
compiler inferences and the inspector use the remaining four slots.

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
| Runner invocations | 1 | 5, at most 2 at once | One per model |
| Compiler commands | 6 | 15 quantized inferences | At most 15 |
| Peak concurrent processes | 1 | 6 | At most 6 |
| Container starts | 1 | 1 | 1 |
| Container memory peak | Not measured | 9.1 GiB | Under the 14 GiB limit |
| Wall time | 13.0–13.9 s | 46.2–46.8 s | Recorded, no budget |

The container memory peak is the cgroup's `memory.peak`, which the worker
reads when it ends; the report also gives the largest single process, 4.4
GiB. A measurement run under a 32 GiB limit peaked at 9,301 MiB; the 14 GiB
limit is that peak plus half, rounded up to a whole GiB. Three passing runs
at the 14 GiB limit took 46.2, 46.7 and 46.8 s with peaks of 9,297, 9,289
and 9,277 MiB. Before the runner lanes, when all five runners started
together, the peak was 20.3 GiB and runs took 47.1–47.7 s.

ShuffleNetV2's runner invocation, at 45.8–46.4 s, is the critical path. Each
model's jobs run on one prepared graph, so ShuffleNetV2's five jobs run in
series on one core. Splitting them across processes would shorten the run to
about 12 s, each process paying 2.4 s of start-up and a 9.4 s first job, but
would hold 4.4 GiB per process. The 15 compiler inferences take 1.7–1.9 s each
and the five runner builds 0.1–0.2 s each.

**The harness keeps no result cache.** The design allowed one only if a cold
run took 60 s or more. Every run therefore repeats all 84 simulator jobs, and
a second run is as long as the first. Without a cache the runs share no state:
each has its own output directory and container name, so the design's lock on
a cache directory is not needed.

## Failure handling

The run fails, and reports every model's state, on any of these:

- a runner, reference or inspector failure, or a timeout;
- a wrong output size;
- an input read-back that differs from the vector;
- a missing or changed pinned file;
- a repeated vector with different outputs, or with a saturated output;
- a simulator output for the catalogued input that differs from the catalogue;
- any differing code;
- an uncovered input or output line of a pinned-compiler model.

A failing comparison
reports the number of differing codes, at most 16 of their positions and the
largest code difference for each output.

Time limits:

- **Runner build:** 120 s per model.
- **Compiler inference:** 120 s per vector.
- **Inspector:** 60 s per model.
- **Runner invocation:** the one-vector limit of the model's pipeline (120 s
  for the convolution graphs, 600 s for mimus and ShuffleNetV2) plus four times
  the measured per-vector time for each further job. The one-vector limit
  covers graph preparation, so the per-vector times in the reference table
  are later-job means: 0.01 s for the convolution graphs, 0.1 s for mimus and
  8.5 s for ShuffleNetV2.
- **Container:** the sum of the step limits plus 120 s, 4,537 s for this
  catalogue. On expiry the harness runs `docker kill` and removes the
  container.

The harness verifies every pinned file once, when it stages the file. The
container cannot change a staged file afterwards: the inputs are mounted
read-only.

The host check [npu-conformance.py](../checks/npu-conformance.py) runs without
the image or the cache. It checks:

- the 84-job plan and the 55 input-line vectors for the catalogue, the seeded
  bytes, each line vector's single changed line and each model's repeated
  vector;
- line coverage, including line vectors whose outputs equal the preceding
  job's, which leave their lines uncovered;
- the int32-bias rule, including half-to-even rounding and clamping;
- the verdict for each failure listed above;
- the generated runner bindings, the runner timing, the runner lanes and the
  time limits;
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
