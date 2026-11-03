# Length-independent NPU execution: tile graphs and job lists

**Status: the tile graphs are implemented and run on the board through the existing queue; the rebinding calls of section 6 are not
implemented.** The graphs (entry, attention step, output, convolution, subsampling and head) are built, quantized and compiled for
the 16 layers of the conformer encoder of the mimus speech recognizer (176 channels, four attention heads, 704-wide feed-forward,
depthwise convolution kernel 31). The model-specific builders live in the mimus repository; this note covers the part that
belongs to the operating system and the application kit. Sections 1 to 5 describe the decomposition as built. Section 6 is a
proposal. Section 8 states what runs and what it costs, and section 11 lists the open items.

## 1. Requirement

A model offload must accept input of any length. The mimus recognizer encodes each voice-activity segment of at most 20 s
(about 500 encoder frames of 40 ms) with attention over the whole segment, and other models offloaded through the SDK have
their own lengths. Compiling one graph set per length does not meet this:

- every set repeats the weights, and the resident memory grows with the number of sets (a 276-frame set needs about 60 MB of
  NPU pool, a 368-frame set about 84 MB);
- the attention cost and the intermediate tensors grow with the square of the length;
- the engine's shape fields are 16 bits wide, so a flat shader tensor of 65,536 elements or more cannot run
  (`docs/hardware/npu-command-formats.md`, "Long tensors"); a graph for a long input hits the limit.

The design below compiles a small set of graphs for a fixed tile of N frames. A runtime job list runs them over any number of
tiles. No tensor dimension of any graph depends on the input length, the pool holds one tile set, and the memory that grows
with length is the application's own activation buffers (about 1 MB for 500 frames).

## 2. Decomposition of one conformer layer

The layer is the sequence feed-forward 1, self-attention with relative positions, convolution module, feed-forward 2, final
layer norm. Every part except attention and the depthwise convolution is a function of one frame, and those two read other
frames.

| Graph | Per-job input | Per-job output | Weights it holds |
| --- | --- | --- | --- |
| Entry (fused: convolution-module tail of the previous layer and head of this one) | tile `g` window of N + 30 frames (depthwise input), tile `x2` of N frames | tile `x1` (after feed-forward 1), `q+u`, `q+v`, `k`, `v` (4 x N x 44 each) | depthwise kernel, pointwise 2, feed-forward 2 and final norm of layer L - 1; feed-forward 1, layer norm and projections of layer L |
| Attention step | `q+u`, `q+v` tile i; `k`, `v` tile j; positional slice (4 x 44 x (2N - 1)); key mask (N); carried `o`, `m`, `l` | updated `o`, `m`, `l` | none |
| Output (per-frame) | `o` tile, `x1` tile | `x2` tile (after output projection), `g` tile (pointwise 1 and gate) | output projection, layer norm and pointwise 1 |

The depthwise convolution (kernel 31) needs 15 frames either side. The entry graph of the next layer consumes `g` as a window of
N + 30 frames, so the application keeps `g` in one buffer of T + 30 frames (zeros in the 15 frames at each end and in the padding
beyond T) and the job reads the window that starts N frames after the previous one. A tile is exact at its edges because the
halo frames are the true neighbours, not a recomputation. Each weight appears in exactly one graph, so the weights are stored
once whatever the length. The layer count is 16: 16 output graphs, 17 entry graphs (the first has no convolution tail, the last
has no head), the attention step graph (one per layer if the layers need their own scales, see section 5), plus the
subsampling and head tiles.

The subsampling convolution (two stride-2 convolutions) tiles in the same way with a halo of the receptive field in input
frames. The head is per-frame.

**Two sizes for the per-frame graphs.** Only the attention step is bounded by the 16-bit limit; a per-frame graph has no
quadratic tensor. A tile set therefore holds each per-frame graph at two sizes, a large one (for example 256 frames) and a small one
(32), and the runtime covers T with as many large tiles as fit and small tiles for the rest, so that the last tile pads at most 31
frames and a 500-frame segment is one tile of 256 and seven of 32 for 480 frames plus one more of 32 (padded by 12), or simply two of 256 (padded by 12). The
attention step stays at N = 128 for the query side and the key side; its query tiles are the per-frame tiles regrouped (two tiles of 64
or one of 128), which is a view in the same buffer. The SDK keeps the several sizes of a set in the API (section 6): a set is a list of
(size, handle) pairs and the job list names the handle of the size it uses.

## 3. Attention in two directions

Query tile i against key tile j, with a running softmax per query row, as in FlashAttention. For one head, with scores
`s = (q+u) k^T + (q+v) p`, scale `1/sqrt(44)`, mask added:

```
m'  = max(m, rowmax(s))
l'  = l * exp(m - m') + rowsum(exp(s - m'))
o'  = (o * l * exp(m - m') + exp(s - m') v) / l'        (o carried normalized)
```

Keeping `o` normalized keeps its range at the range of `v`, which a 16-bit dynamic fixed-point tensor needs. The first key tile
starts from `m = -large`, `l = 0`, `o = 0`, which the runtime writes once per query tile. After the last key tile `o` is the
attention output for the tile. The number of steps is `n^2` for `n = ceil(T / N)`.

**Relative-position term.** The score table uses the offset `d = (query position) - (key position)`. For tiles i and j the
offsets run over `(i - j) N - (N - 1)` to `(i - j) N + (N - 1)`, which is 2N - 1 consecutive values. The layer's table of
projected positional embeddings, indexed by offset, is a constant of the layer (it depends on the position, not on T). The
runtime feeds the step the 2N - 1 columns that start at `C0 - ((i - j) N + N - 1)`, where `C0` is the column of offset zero.
Inside the step the shifted index of row a and column b is `N - 1 - a + b` for every tile pair, so **one compiled graph serves
every tile pair**: the compiled artifacts contain no offset class, and the gather index table is `N x N` and constant. The
table of one layer for 500 frames is 4 x 44 x 999 x 2 bytes, 352 KB, and 5.6 MB for 16 layers (stored once, packed with the
model).

**Padding.** The key mask input adds a large negative number to the scores of padded keys of the last key tile. Query rows beyond
T are computed and discarded. The score tensor of a step is 4 x N x N and the gather input 4 x N x (2N - 1) flattened to
4 x N(2N - 1), which for N = 128 is 32,640, below the 16-bit limit; N up to 180 stays below it
(180 x 359 = 64,620). The loader refuses any larger.

**Where the merge runs.** The carried-state form puts the merge inside the step graph, so consecutive steps of one query tile
are chained by the queue's tensor links and the CPU is not in the loop. The alternative is for the step to return its block-local
`o`, `m`, `l` and for the CPU to merge: per layer and tile pair that is 4 x N x 44 multiply-accumulates, 22,500 for N = 128, and
`n^2` pairs of tiles with a few operations each, about 0.8 ms per layer at 500 frames (an estimate, not measured), 13 ms over 16 layers, about 4% of the NPU time. The carried form is the implemented one: its error in 16-bit fixed point against float is in section 8, and it avoids the CPU
round trip. The CPU merge remains the fallback if a layer's exponentials and rescale lose precision in 16 bits.

## 4. Runtime schedule

For a segment of T frames, n tiles, per layer:

```
for tile t in 0..n-1:      entry(t)                      # n jobs, independent
for tile i in 0..n-1:                                    # n chains, independent
    for tile j in 0..n-1:  step(i, j)  after step(i, j-1)  # chain of n jobs, state carried by link
for tile t in 0..n-1:      output(t)  after chain t      # n jobs
```

Per layer that is `n` entry jobs, `n^2` steps and `n` output jobs, `2n + n^2` in all. The chains of different
query tiles are independent, so the ring (depth 64) keeps the NPU busy while the CPU prepares the next job; the same graph is
registered under two or three handles so that rebinding for one job never overlaps a running job of the same handle (section 6).

## 5. Quantization

Each graph has fixed scales per tensor (dynamic fixed point, 16 bits). The per-frame graphs are calibrated on frames from
many utterances; the lengths do not matter because the graph sees one frame at a time. The attention step needs ranges for
`q+u`, `q+v`, `k`, `v`, the score, `m`, `l` and `o`; `l` ranges up to T and `m` over the score range. If one scale does not fit
all 16 layers the step is compiled once per layer (about 0.2 MB each, no weights).

## 6. SDK interface

**Status: proposed, not implemented.** None of `ss_npu_queue_register_set`, `ss_npu_queue_region` and `ss_npu_queue_submit_jobs`
exists in `include/sunstone_npu_queue.h` or in the operating system service. The queue binds each tensor of a handle to a buffer
fixed at registration, and a tile loop needs the same graph on many windows of one buffer. The proposal adds three calls and one
invariant. Section 8 states how the tile graphs run until they exist.

```
/* Register a set of tile graphs once (slots, handles, tensor sizes as now). */
int ss_npu_queue_register_set(struct ss_npu_queue *, const unsigned *prepared_entries, size_t count, ss_npu_handle *out);

/* A region the application owns (its activation buffer), registered once and mapped for the NPU. */
int ss_npu_queue_region(struct ss_npu_queue *, void *data, size_t bytes, enum ss_npu_access, ss_npu_region *out);

/* One job: a handle, a binding for each tensor (region, byte offset), and an optional dependency. */
struct ss_npu_binding { ss_npu_region region; uint32_t offset; };
struct ss_npu_job { ss_npu_handle handle; struct ss_npu_binding in[SS_NPU_QUEUE_MAX_TENSORS], out[SS_NPU_QUEUE_MAX_TENSORS];
                    struct ss_npu_fence after; };
int ss_npu_queue_submit_jobs(struct ss_npu_queue *, const struct ss_npu_job *, size_t count, struct ss_npu_fence *last);
```

`submit_jobs` publishes a whole list (a layer's `2n + n^2` jobs) in one call; the ring accepts as many as it has room for and
reports the count taken, so an application feeds it in batches without a call per job. For each job the OS service, in EL1,
checks that every offset plus the tensor's byte size lies inside its region, then rewrites the address words of the slot's
command stream and tensor descriptors for that tensor (the loader keeps for each slot the list of word positions and the
tensor offsets they were derived from, which the relocation step already computes) and cleans those cache lines before the
job's doorbell. A rewrite touches a few words. Containment is unchanged: the NPU's page tables map only registered regions, and
the service refuses a binding outside one. A handle's bindings change only when no job of that handle is in flight, which the
service enforces by ordering and the application meets by registering hot graphs under several handles.

The same pattern serves other models. A convolutional model tiles by rows with a halo; a transformer decoder runs one query
tile against key and value tiles of a cache; a model with a per-frame body runs it over any number of frames. The SDK sees only
"graph, bindings, dependencies".

## 7. Cost model

Measurements (one slot, 852 MHz and 800 mV, byte-exact against the simulator unless noted):

| Measured | Time |
| --- | --- |
| 92-frame layer, whole (everything in VIP SRAM) | 1.26 ms |
| 368-frame feed-forward block alone | 1.32 ms (3.6 us a frame) |
| 368-frame layer in 64-row attention blocks | 12.8 ms |
| 368-frame attention part (norm, projections, 64-row blocks, output projection) | 9.79 ms |
| 368-frame convolution module alone (norm, pointwise 1 with gate, depthwise, pointwise 2) | 1.31 ms (3.6 us a frame; output position -2) |
| 460 and 512 frames in 64-row blocks | 25.9 and 38.6 ms |
| per-job overhead of the SDK handshake (chain run) | about 0.025 ms |
| subsampling, head and rescales at 92 frames (chain minus 16 layers) | 4.4 ms |

From these, per layer: the per-frame parts cost 10 to 12.6 us a frame (two feed-forwards 7.2, four projections 1.8, convolution
module 3.6 measured alone; the sum of the separately measured parts is 7% above the whole layer because the compiler overlaps
neighbouring operations, which a tile graph of one part loses); the table uses 10, the fit to the whole layers. The attention core costs 40 ns per query-key pair for all four heads when the block sits in VIP SRAM (the 92-frame
layer) and 67 ns per pair in the 64-row blocks at 368 frames. The model for a segment of T frames, tile N, padded length
`Tp = n N`, `n = ceil(T / N)`:

```
NPU(T) = 16 * (10 us * Tp + b * Tp^2 + 0.025 ms * (2n + n^2)) + 0.048 ms * Tp + 2.1 ms (front end on the CPU)
```

with `b` between 45 and 67 ns. The CPU path is 109.6 ms at 92 frames; scaling it linearly (a lower bound, because its attention
grows with the square) gives 1.19 ms a frame.

| Frames T | Tile N | Jobs | NPU, 45 ns (ms) | NPU, 67 ns (ms) | CPU path, linear (ms) | Gain |
| --- | --- | --- | --- | --- | --- | --- |
| 92 | 128 | 48 | 41.7 | 47.5 | 109.6 | 2.3 to 2.6x |
| 92 | 64 | 128 | 43.7 | 49.5 | 109.6 | 2.2 to 2.5x |
| 184 | 128 | 128 | 105.7 | 128.8 | 219 | 1.7 to 2.1x |
| 184 | 64 | 240 | 74.6 | 87.6 | 219 | 2.5 to 2.9x |
| 368 | 128 | 240 | 194 | 246 | 438 | 1.8 to 2.3x |
| 368 | 64 | 768 | 207 | 259 | 438 | 1.7 to 2.1x |
| 500 | 128 | 384 | 307 | 399 | 596 | 1.5 to 1.9x |
| 500 | 64 | 1,280 | 329 | 422 | 596 | 1.4 to 1.8x |
| 500 | 256 | 128 | 301 | 393 | 596 | 1.5 to 2.0x |

For comparison the fixed 92-frame graph set takes 24.6 ms at 92 frames (4.5 times the CPU path), faster than any tile size,
because it has no padding to 128 and no per-job overhead beyond 18 jobs. It may stay as a fast path for short segments; the
tile set is the general path.

**Where the per-job overhead dominates.** An attention step does N^2 x 45 ns of work: 0.046 ms for N = 32 (the 0.025 ms
overhead is 35% of the job), 0.18 ms for N = 64 (12%), 0.74 ms for N = 128 (3%), 2.9 ms for N = 256 (1%). A per-frame job
of N frames does 10 us x N: the overhead is 7% at N = 32, 4% at 64, 2% at 128. The overhead as a share of the whole
segment at 500 frames is 115 ms of 413 for N = 32, 32 ms of 329 for N = 64, 9.6 ms of 307 for N = 128 and 3 ms of 301 for
N = 256. **N = 128 is the choice**: the overhead is 3% or less, the score tensor (4 x 128 x 128) and the flat positional
tensor (32,640) stay in VIP SRAM and under the 16-bit limit, and the padding waste averages 64 frames. N = 256 saves little more and its flat
positional tensor, 4 x 256 x 511 = 130,816, exceeds the 16-bit limit unless the gather is reshaped. The tile size is a parameter of
the compiled set, so a model with different costs picks its own.

Beyond about 400 frames the quadratic attention work, not the overhead, sets the time: at 500 frames the attention core is
70% of the NPU time. That is a property of full attention and applies to the CPU path too.

## 8. Implementation, numerics and measured cost

**What runs.** Every tile graph of the 16 layers is compiled and runs on the board with outputs equal to the SDK simulator's
(byte for byte for the neural-network, stream-processor and activation graphs; within the engine's half-precision rounding for
graphs that carry half-precision tensors). The loader admits the entry graph, whose residual-stream tensor is both an intermediate
and a result, because it accepts a graph output that later operations read again as an input. The compiled set needs an NPU pool
of 24 MiB or more for the subsampling graphs; an 8 MiB pool refuses the reservation.

**How the jobs run now.** The current queue runs the schedule of section 4. Each tile graph is one resident handle with tensors
fixed at registration, so tile data reach a graph only by writing its input tensors (`ss_npu_queue_tensor`, then
`ss_npu_queue_dirty` for the written ranges) and leave it by acquiring the handle and reading its output tensors. State that
passes between two graphs without the CPU uses a tensor link declared at open (`ss_npu_queue_open_linked`). This is the
copy-per-job form named in section 11: each job costs a copy of its tile in and out, and a handle is never rebound while in
flight. The tiled encoder's time at 100, 250 and 498 frames is 44.9, 117 and 345 ms. These figures are composed from
board-measured per-job times (the four tile graphs at tile 128 take 752.5, 217.8, 177.3 and 320.2 microseconds per job, multiplied
by the job count, plus subsampling and head). They are not an end-to-end run, and no board entry holds them. The whole-clip CPU
path takes 118, 255 and 585 ms at the same lengths, but that is whole recognition (front end, encoder and decode), so the two
columns are not comparable. The word error rate on held-out utterances of 90 to 130 encoder frames, outside the calibration sets,
equals the float encoder's, 5.63%; it comes from running every graph in the SDK simulator, not from the board.

**What is not implemented.** Regions, per-job bindings, `submit_jobs` and the EL1 rebinding service of section 9 do not exist,
so section 9's tests have not been written and its containment argument has not been exercised. The copies in the previous
paragraph are the cost that those calls would remove.

**Per-layer constants.** The score range of the last layer (about -515 to 661, against near zero in the first layers) shows that
one mask value and one initial running maximum cannot serve all layers: with the first layer's values a padded key (a zero key
row, scoring up to +131) won the maximum, the exponentials of the valid keys underflowed to zero, and the encoder output at 100
frames had cosine 0.940 with float. Each layer's step therefore takes its own mask and initial maximum, derived from the layer's
score statistics (largest padded-key score, smallest row maximum over valid keys, lowest valid score), declared in a manifest
beside the five graphs of the layer and checked on the host: the check re-derives the constants and requires every masked padded
key to lie at least 25 below the smallest valid row maximum. With them the output at 100 frames has cosine 0.9998.

**Numerics of the step (host and SDK simulator, one real layer).** The step graph in float equals full attention to 1e-7. Run in
the simulator over every tile pair of one layer, carrying the state as the simulator returned it, against float full attention:

| Frames | Steps | Calibration | RMS error (signal RMS) | Largest error | Cosine |
| --- | --- | --- | --- | --- | --- |
| 100 | 1 | 184 frames, other utterance | 0.032 (7.82) | 0.83 | 0.999992 |
| 500 | 16 | 184 frames, other utterance | 0.245 (8.89) | 23.7, in 7 of 2,000 head-rows | 0.99962 |
| 500 | 16 | the 500 frames themselves | 0.089 (8.89) | 1.28 | 0.999950 |
| 100 | 1 | the 500 frames | 0.044 (7.82) | 0.33 | 0.999984 |

Sixteen-bit fixed point holds: 1% relative RMS error at 500 frames when the ranges cover the data. The seven bad head-rows of the
second line have a largest score of 20 to 22 while that calibration (largest score 16.3) clipped at 16, which is a range limit of
the calibration and not of the form. The production calibration uses many utterances with headroom. The resolution that matters
is the row sum `l` (range 512 when it must reach 500 keys, step 0.016), which costs accuracy on rows whose softmax is concentrated
on one key; carrying `l / 64` instead would reduce that cost. The maximum of negative values is kept by the compiler, the
simulator and the hardware (`docs/hardware/npu-command-formats.md`, "Maximum of negative values"). The step has nine inputs
and three outputs, more than the loader's default of eight tensors, so `qu` and `qv` travel as one tensor of eight heads.

**Cost of the attention step (board, one slot, 852 MHz and 800 mV).** The step, not the per-frame graphs, sets the time at 500
frames: 16 layers of 16 steps. The first form, with the gather that aligns the relative-position scores, took about 1.1 ms for
four heads and 128 query rows. Aligning by slices took 902 microseconds, and the best single-job form with the native softmax took
859. The step now runs as **two jobs of 64 query rows against the 128 keys of a tile, 229.4 microseconds each, 458.8 for the
step**, and the board's outputs equal the simulator's byte for byte. The forms and every timing are in the mimus repository
(`docs/npu-encoder-offload.md`, section 8). The reformulation rests on three facts about the hardware, stated with the unit of
each operation in [the placement note](../hardware/npu-op-placement.md):

- The activation unit computes `Elu`, and `Elu(x) + 1` equals `exp(x)` for `x` at or below zero, which every softmax numerator
  satisfies after the maximum is subtracted. The shader's exponential cost 88 microseconds more in a 64-row job.
- A product with a column of ones appended to the values returns the row sum with the weighted values, and a reciprocal of the
  small row-sum tensor replaces the shader's division of the large output.
- When the tensors live at one time exceed the 512 KiB of on-chip memory, the same operations cost several times as much. Jobs
  of 64 rows stay inside it; a job of 128 rows takes 731 microseconds, not 459.

The accuracy of the form is in the mimus note. Over the nine tile pairs of 294 frames of the last layer it has a root-mean-square
error of 1.2% of the signal, against 4.7% for the shader form on the same calibration.

## 9. The rebinding service in EL1: containment and tests (proposed)

The OS service that applies a job's bindings is the only new code on the job path, so its argument is stated here.

- **What the application controls.** For each tensor of a job it names a region it registered and a byte offset. It never writes an
  address.
- **What the service checks, in EL1, before it writes anything.** The region belongs to this queue; the access kind of the region
  admits the tensor (read for an input, write for an output); `offset + tensor bytes` lies inside the region (no overflow in 64
  bits); the offset keeps the alignment the tensor requires (64 bytes); no output window of the job overlaps another output window
  of an unfinished job of the same queue. A job failing any check is refused whole, with nothing rewritten.
- **What it rewrites.** The address words at the positions the loader recorded when it admitted the graph, with the value
  `region device base + offset` (plus the tensor's own sub-offset). The positions come from the loader's own relocation list, which
  were already confined at admission; the service adds no position.
- **Why containment holds without the service being right.** The NPU's page tables map only registered regions and the
  buffers of loaded graphs. A wrong word faults at an unmapped address; it cannot reach other memory. The service's checks
  keep a job inside its own regions; the page tables keep the NPU inside the union of all regions.
- **Stale bindings.** A rewrite for a handle is applied only when no job of that handle is in flight or queued ahead of it; the
  service orders a job's rewrite after the completion of the handle's previous job, in the same step that publishes the job, and
  cleans the rewritten cache lines before the doorbell. A job published before its rewrite completes is impossible by
  construction; a job whose handle still runs waits, and the application's second handle (section 4) avoids the wait.

Negative tests (host, with the real runtime and service as in `checks/os-npu-service.c`): an offset one byte past the region is refused
and the command stream is byte-for-byte unchanged; an offset that overflows 64 bits is refused; a region of another queue is refused;
an output window overlapping the output window of an unfinished job is refused; a rewrite for a handle with a job in flight is
refused, and after that job completes the rewrite succeeds; a binding that names a region the application then unregisters leaves
no job able to reach it. Board: one job with an address one page past its region faults with an NPU MMU exception and the next job
is correct (as the existing fault tests).

## 10. Proof plan

The plan below was run on all 16 layers; section 8 has the results.

On one layer (a real layer of the trained model, real weights), build the tile graphs for N = 128, compile and calibrate them, and
run a job list on the board at three lengths: 100, 250 and 500 frames. Compare:

- each tile graph on the board against its own simulator output, byte for byte (as for the fixed graphs);
- the tiled layer, composed from simulator outputs on the host, against the untiled graph's simulator output where that graph is
  valid (100 frames whole-length; the 250-frame graph in 64-row blocks), and against the float layer otherwise. Byte equality
  with the untiled graph is not expected, because the tile graphs quantize their intermediates at different points; the
  comparison reports cosine, mean and maximum error, and the same quantities for the untiled graph against float, so that the
  tiled error can be judged against it;
- the job times against the model above.

The step graph's numerics (exponentials, running maximum, rescale in 16-bit fixed point) are the main risk and are tested first.
If they fail, the merge moves to the CPU (section 3) and the step graph returns block-local `o`, `m`, `l`.

## 11. Limits and open questions

Resolved by the implementation:

- The attention step takes 458.8 microseconds as two jobs of 64 rows (section 8), under the 600 microseconds it was required to
  meet.
- The compiler accepts `ReduceMax`, `Exp`, `ReduceSum` (as a product with a column of ones), `Softmax`, `Gather` and `Div` on the
  shapes of the step, and the step runs on the board. Mask values and the initial state are representable in the scales of
  `m` and `l` once each layer has its own constants (section 8).
- The proof layer ran through the existing queue with a copy per job, which measured numerics and time without the rebinding
  service.

Open:

- **The rebinding calls are not implemented.** `ss_npu_queue_register_set`, `ss_npu_queue_region` and `ss_npu_queue_submit_jobs`
  (section 6) and the EL1 service with its containment tests (section 9) do not exist. Until they do, every tile job pays the
  copy of section 8 and an application cannot publish a layer's `2n + n^2` jobs in one call. The step now runs as two jobs per
  tile pair, and the two jobs share the key, value and position inputs, so the copies per step double and the rebinding
  calls remove more than before. The share of the copies in the step time is not measured.
- **The production image needs an NPU pool of 24 MiB or more** for the subsampling graphs.
- **The subsampling tile sizes are untuned.** Subsampling graphs named for 32, 64 and 128 were timed on the board at 2.2, 3.7
  and 8.7 ms, and none has been compared with the schedule's other costs.
