# Length-independent NPU execution: tile graphs and job lists

**Status: the tile graphs are implemented and qualified on the board through the existing queue (copy per job); the rebinding calls of section 5 are open.** The graphs (entry, attention step, output, convolution, subsampling and head) are built, quantized and compiled for the 16 layers of the conformer encoder of the mimus speech recognizer (176 channels, four attention heads, 704-wide feed-forward, depthwise convolution kernel 31). The model-specific builders live in the mimus repository; this note covers the part that belongs to the operating system and the application kit. Records: [npu-tiled-execution.jsonl](../evidence/npu-tiled-execution.jsonl).

## 1. Requirement

A model offload must accept input of any length. The mimus recognizer encodes each voice-activity segment of at most 20 s (about 500 encoder frames of 40 ms) with attention over the whole segment, and other models offloaded through the SDK have their own lengths. One graph set per length does not meet this:

- every set repeats the weights, and the resident memory grows with the number of sets (a 276-frame set needs about 60 MB of NPU pool, a 368-frame set about 84 MB);
- the attention cost and the intermediate tensors grow with the square of the length;
- the engine's shape fields are 16 bits wide, so a flat shader tensor of 65,536 elements or more cannot run ([command formats](../hardware/npu-command-formats.md), "Long tensors").

The design compiles a small set of graphs for a fixed tile of N frames. A runtime job list runs them over any number of tiles. No tensor dimension of any graph depends on the input length, the pool holds one tile set, and the memory that grows with length is the application's own activation buffers (about 1 MB for 500 frames).

## 2. Decomposition of one conformer layer

The layer is the sequence feed-forward 1, self-attention with relative positions, convolution module, feed-forward 2, final layer norm. Every part except attention and the depthwise convolution is a function of one frame.

| Graph | Per-job input | Per-job output | Weights it holds |
| --- | --- | --- | --- |
| Entry (fused: convolution-module tail of the previous layer and head of this one) | tile `g` window of N + 30 frames (depthwise input), tile `x2` of N frames | tile `x1` (after feed-forward 1), `q+u`, `q+v`, `k`, `v` (4 x N x 44 each) | depthwise kernel, pointwise 2, feed-forward 2 and final norm of layer L - 1; feed-forward 1, layer norm and projections of layer L |
| Attention step | `q+u`, `q+v` tile i; `k`, `v` tile j; positional slice (4 x 44 x (2N - 1)); key mask (N); carried `o`, `m`, `l` | updated `o`, `m`, `l` | none |
| Output (per-frame) | `o` tile, `x1` tile | `x2` tile (after output projection), `g` tile (pointwise 1 and gate) | output projection, layer norm and pointwise 1 |

The depthwise convolution (kernel 31) needs 15 frames either side. The entry graph of the next layer consumes `g` as a window of N + 30 frames, so the application keeps `g` in one buffer of T + 30 frames (zeros in the 15 frames at each end and in the padding beyond T) and the job reads the window that starts N frames after the previous one. A tile is exact at its edges because the halo frames are the true neighbours. Each weight appears in exactly one graph, so the weights are stored once whatever the length. There are 16 output graphs, 17 entry graphs (the first has no convolution tail, the last has no head), the attention step graph (one per layer, see section 4), plus the subsampling and head tiles. The subsampling convolution (two stride-2 convolutions) tiles in the same way with a halo of the receptive field in input frames. The head is per-frame.

**Two sizes for the per-frame graphs.** Only the attention step is bounded by the 16-bit limit. A tile set holds each per-frame graph at a large size (for example 256 frames) and a small one (32). The runtime covers T with as many large tiles as fit and small tiles for the rest, so the last tile pads at most 31 frames. The attention step stays at N = 128 for the query side and the key side; its query tiles are the per-frame tiles regrouped (two tiles of 64 or one of 128), a view in the same buffer. A set is a list of (size, handle) pairs and the job list names the handle of the size it uses.

## 3. Attention in two directions

Query tile i against key tile j, with a running softmax per query row, as in FlashAttention. For one head, with scores `s = (q+u) k^T + (q+v) p`, scale `1/sqrt(44)`, mask added:

```
m'  = max(m, rowmax(s))
l'  = l * exp(m - m') + rowsum(exp(s - m'))
o'  = (o * l * exp(m - m') + exp(s - m') v) / l'        (o carried normalized)
```

Keeping `o` normalized keeps its range at the range of `v`, which a 16-bit dynamic fixed-point tensor needs. The first key tile starts from `m = -large`, `l = 0`, `o = 0`, which the runtime writes once per query tile. After the last key tile `o` is the attention output for the tile. The number of steps is `n^2` for `n = ceil(T / N)`.

**Relative-position term.** The score table uses the offset `d = (query position) - (key position)`. For tiles i and j the offsets run over `(i - j) N - (N - 1)` to `(i - j) N + (N - 1)`, which is 2N - 1 consecutive values. The layer's table of projected positional embeddings, indexed by offset, is a constant of the layer. The runtime feeds the step the 2N - 1 columns that start at `C0 - ((i - j) N + N - 1)`, where `C0` is the column of offset zero. Inside the step the shifted index of row a and column b is `N - 1 - a + b` for every tile pair, so **one compiled graph serves every tile pair** and the gather index table is `N x N` and constant. The table of one layer for 500 frames is 4 x 44 x 999 x 2 bytes, 352 KB, and 5.6 MB for 16 layers (stored once, packed with the model).

**Padding.** The key mask input adds a large negative number to the scores of padded keys of the last key tile. Query rows beyond T are computed and discarded. The score tensor of a step is 4 x N x N and the gather input 4 x N x (2N - 1) flattened to 4 x N(2N - 1), which for N = 128 is 32,640, below the 16-bit limit; N up to 180 stays below it (180 x 359 = 64,620). The loader refuses any larger.

**Where the merge runs.** The merge is inside the step graph (carried-state form), so consecutive steps of one query tile are chained by the queue's tensor links and the CPU is not in the loop. A CPU merge of block-local `o`, `m`, `l` is the fallback if a layer's exponentials and rescale lose precision in 16 bits; it costs an estimated 4% of the NPU time.

## 4. Runtime schedule, quantization and per-layer constants

For a segment of T frames, n tiles, per layer:

```
for tile t in 0..n-1:      entry(t)                      # n jobs, independent
for tile i in 0..n-1:                                    # n chains, independent
    for tile j in 0..n-1:  step(i, j)  after step(i, j-1)  # chain of n jobs, state carried by link
for tile t in 0..n-1:      output(t)  after chain t      # n jobs
```

That is `n` entry jobs, `n^2` steps and `n` output jobs, `2n + n^2` in all. The chains of different query tiles are independent, so the ring (depth 64) keeps the NPU busy while the CPU prepares the next job; the same graph is registered under two or three handles so that rebinding for one job never overlaps a running job of the same handle.

Each graph has fixed scales per tensor (dynamic fixed point, 16 bits). The per-frame graphs are calibrated on frames from many utterances; the lengths do not matter because the graph sees one frame at a time. The attention step needs ranges for `q+u`, `q+v`, `k`, `v`, the score, `m`, `l` and `o`; `l` ranges up to T and `m` over the score range. Because score ranges differ by layer (about -515 to 661 in the last layer, near zero in the first), each layer's step takes its own mask value and initial running maximum, derived from the layer's score statistics, declared in a manifest beside the five graphs of the layer and checked on the host: every masked padded key must lie at least 25 below the smallest valid row maximum (`npu-tiled-execution-layer-constants`).

The step has nine inputs and three outputs, more than the loader's default of eight tensors, so `qu` and `qv` travel as one tensor of eight heads. The compiler, simulator and hardware keep the maximum of negative values ([command formats](../hardware/npu-command-formats.md), "Maximum of negative values").

## 5. Implemented execution and the proposed SDK interface

**What runs.** Every tile graph of the 16 layers is compiled and runs on the board with outputs equal to the SDK simulator's (byte for byte for the neural-network, stream-processor and activation graphs; within the engine's half-precision rounding for graphs that carry half-precision tensors). The loader admits the entry graph, whose residual-stream tensor is both an intermediate and a result, because it accepts a graph output that later operations read again as an input. The compiled set needs an NPU pool of 24 MiB or more for the subsampling graphs; an 8 MiB pool refuses the reservation.

**How the jobs run.** Each tile graph is one resident handle with tensors fixed at registration. Tile data reach a graph by writing its input tensors (`ss_npu_queue_tensor`, then `ss_npu_queue_dirty` for the written ranges) and leave it by acquiring the handle and reading its output tensors. State that passes between two graphs without the CPU uses a tensor link declared at open (`ss_npu_queue_open_linked`). Each job therefore costs a copy of its tile in and out, and a handle is never rebound while in flight.

**Proposed, not implemented.** `ss_npu_queue_register_set`, `ss_npu_queue_region` and `ss_npu_queue_submit_jobs` are not in `include/sunstone_npu_queue.h` or the operating system service. They would let a tile loop run the same graph on many windows of one application buffer:

```
int ss_npu_queue_register_set(struct ss_npu_queue *, const unsigned *prepared_entries, size_t count, ss_npu_handle *out);
int ss_npu_queue_region(struct ss_npu_queue *, void *data, size_t bytes, enum ss_npu_access, ss_npu_region *out);
struct ss_npu_binding { ss_npu_region region; uint32_t offset; };
struct ss_npu_job { ss_npu_handle handle; struct ss_npu_binding in[SS_NPU_QUEUE_MAX_TENSORS], out[SS_NPU_QUEUE_MAX_TENSORS];
                    struct ss_npu_fence after; };
int ss_npu_queue_submit_jobs(struct ss_npu_queue *, const struct ss_npu_job *, size_t count, struct ss_npu_fence *last);
```

`submit_jobs` publishes a list (a layer's `2n + n^2` jobs) in one call; the ring accepts as many as it has room for and reports the count taken. For each job the EL1 service checks that every offset plus the tensor's byte size lies inside its region, then rewrites the address words of the slot's command stream and tensor descriptors for that tensor (the loader keeps the list of word positions and the tensor offsets, which the relocation step already computes) and cleans those cache lines before the job's doorbell. The same pattern serves a convolutional model tiled by rows with a halo, a transformer decoder running a query tile against key and value tiles of a cache, and a model with a per-frame body.

**Containment of the rebinding service (proposed).**

- The application names, per tensor, a region it registered and a byte offset. It never writes an address.
- Before writing anything the service checks in EL1: the region belongs to this queue; the region's access kind admits the tensor (read for an input, write for an output); `offset + tensor bytes` lies inside the region with no 64-bit overflow; the offset keeps the tensor's 64-byte alignment; no output window overlaps another output window of an unfinished job of the same queue. A job failing any check is refused whole, with nothing rewritten.
- It rewrites only the address words at positions the loader recorded and confined at admission, with the value `region device base + offset` (plus the tensor's own sub-offset).
- Containment holds without the service being right: the NPU's page tables map only registered regions and the buffers of loaded graphs, so a wrong word faults at an unmapped address.
- A rewrite for a handle is applied only when no job of that handle is in flight or queued ahead of it; the service orders it after the completion of the handle's previous job, in the same step that publishes the job, and cleans the rewritten cache lines before the doorbell.

The negative tests to write are listed in `npu-tiled-execution-rebinding-tests`.

## 6. Cost

The tile size N = 128: per-job overhead (0.025 ms for the SDK handshake) is 3% or less of a job, the score tensor and flat positional tensor stay in VIP SRAM and under the 16-bit limit, and padding waste averages 64 frames. The tile size is a parameter of the compiled set. Beyond about 400 frames the quadratic attention work sets the time.

| Figure | Value | Record |
| --- | --- | --- |
| 92-frame layer, whole | 1.26 ms | `npu-tiled-execution-cost-model` |
| 368-frame layer in 64-row attention blocks | 12.8 ms | same |
| Attention step, two jobs of 64 query rows against 128 keys | 458.8 µs (229.4 µs each) | `npu-tiled-execution-step-cost` |
| Step error against float, nine tile pairs of 294 frames, last layer | 1.2% of signal RMS (shader form: 4.7%) | same |
| Tiled encoder at 100, 250, 498 frames (composed from per-job board times, not an end-to-end run) | 44.9, 117, 345 ms | `npu-tiled-execution-composed-times` |

The cost model is `NPU(T) = 16 * (10 µs * Tp + b * Tp^2 + 0.025 ms * (2n + n^2)) + 0.048 ms * Tp + 2.1 ms` with padded length `Tp = n N` and `b` between 45 and 67 ns per query-key pair.

## 7. Open work

1. **The rebinding calls and the EL1 rebinding service are not implemented.** Until they exist every tile job pays a copy of its tile in and out, and an application cannot publish a layer's `2n + n^2` jobs in one call. The step runs as two jobs per tile pair that share the key, value and position inputs, so the copies per step double. The share of the copies in the step time is not measured.
2. **The production image needs an NPU pool of 24 MiB or more** for the subsampling graphs.
3. **The subsampling tile sizes are untuned.** Subsampling graphs for 32, 64 and 128 frames take 2.2, 3.7 and 8.7 ms on the board and are not compared with the schedule's other costs.
4. An end-to-end board run of the tiled encoder at several lengths; the figures above are composed.
