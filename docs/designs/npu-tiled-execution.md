# Length-independent NPU execution: tile graphs and job lists

**Status: design, not implemented.** Nothing in this note has run except the measurements in section 8, which are single-graph
board runs of the conformer layer of the mimus speech encoder (176 channels, four attention heads, 704-wide feed-forward,
depthwise convolution kernel 31).

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
`n^2` pairs of tiles with a few operations each, about 0.8 ms per layer at 500 frames (an estimate, not measured), 13 ms over 16 layers, about 4% of the NPU time. The decision rests on two measurements to be made on the proof layer: the error of the carried form in 16-bit
fixed point against float, and the time of each. The carried form is the default because it avoids the CPU round trip; the CPU
merge is the fallback if the exponentials and the rescale do not hold their precision in 16 bits.

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

The queue today binds each tensor of a handle to a buffer fixed at registration. A tile loop needs the same graph on many
windows of one buffer. The proposal adds three calls and one invariant.

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

## 8. Numerics of the attention step (measured on the host and the SDK simulator)

The step graph of section 3 compiles (`Max`-free form: the running maximum is `rowmax + relu(m - rowmax)`, the row maximum is
taken over scores lifted by 40 because the reduction starts at zero and loses negative maxima, `Exp`, `Div` and the row sum as a
product with a column of ones) in 422,879 estimated cycles per step at N = 128. The step in float equals full attention to 1e-7.
Run in the simulator over every tile pair of one real layer (carrying the state as the simulator returned it), against float full
attention:

| Frames | Steps | Calibration | RMS error (signal RMS) | Largest error | Cosine |
| --- | --- | --- | --- | --- | --- |
| 100 | 1 | 184 frames, other utterance | 0.032 (7.82) | 0.83 | 0.999992 |
| 500 | 16 | 184 frames, other utterance | 0.245 (8.89) | 23.7, in 7 of 2,000 head-rows | 0.99962 |
| 500 | 16 | the 500 frames themselves | 0.089 (8.89) | 1.28 | 0.999950 |
| 100 | 1 | the 500 frames | 0.044 (7.82) | 0.33 | 0.999984 |

Sixteen-bit fixed point therefore holds: 1% relative RMS error at 500 frames when the ranges cover the data. The 7 bad rows of the
second line are head-rows whose largest score is 20 to 22 while the calibration (one 184-frame utterance, largest score 16.3) clipped
at 16: a calibration range, not a limit of the form. The production calibration uses many utterances with headroom. The
resolution that matters is the row sum `l` (range 512 when it must reach 500 keys, step 0.016), which costs accuracy on rows whose
softmax is concentrated on one key; it can carry `l / 64` instead.

The compiler leaves some inputs of this graph in half precision on its own (the mask and the running maximum): it offers a float16
path per tensor, with 11 bits of mantissa, where 16-bit fixed point lacks the range. An input in half precision is written to the
simulator as decimal values, not codes. A whole-graph half-precision compile was not tried.

Two compiler findings that shaped the graph: a `Max` of values that may be negative and a `ReduceMax` of negative values return 0 in
the simulator; and the step has nine inputs and three outputs, more than the loader's default of eight tensors, so an image that
runs it is built with a larger `SUNSTONE_OS_NPU_MAX_TENSORS` (or the step packs `qu` and `qv`, `k` and `v`, and `m` and `l` in
pairs).

## 9. The rebinding service in EL1: containment and tests

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

- The compiler must accept `ReduceMax`, `Exp`, `ReduceSum` and `Div` on the shapes of the step; it accepted `Softmax` and
  `Gather`, and the shader corpus includes `Exp`, but the combination has not been compiled.
- Mask values and the first-tile initial state must be representable in the scales of `m` and `l`.
- The rebinding service (section 6) is new OS code with its own containment proof; the proof layer can run with a CPU copy per
  job through the existing queue (one handle per tile graph, the application copying tile data into the graph's own tensors) to
  measure numerics and time before that exists, at the cost of a copy per job.
- Subsampling with halo and the head need the same treatment and are not designed in detail here.
