# Operation placement and cost of compiled NPU graphs

This note records which engine unit the vendor compiler assigns to each operation of a graph, what each placement costs on the
A733 NPU, and which graph shapes the compiler or the hardware handles badly. The facts come from compiled graphs that the OS
loader decodes and from timings on the board. The graphs are the attention step of the mimus speech encoder (four heads of 44
channels, 128 key rows, a score tensor of 65,536 elements), but each fact is a property of an operation and a tensor shape, not
of that model. Status: measured; the entries that hold each figure are in the last section.

## Method

Each graph runs as a single job in the resident queue (`apps/npu-graphs`, built with `NPU_GRAPHS_QUEUE_LAT=100`). The application
submits a job, waits until it completes, and records the counter ticks; `apps/npu-graphs/host.py` (`latency_of`) turns the 100
ticks into percentiles. Every figure below is the median of those 100 single jobs on an NPU at 852 MHz and 800 mV with the
Cortex-A55 submitting core at 1794 MHz, including the job's fixed overhead of about 5 microseconds (measured with a graph of one
addition: 5.0 microseconds). The p99 is within 15% of the median for every job longer than 200 microseconds. The compiler prints a cycle estimate for every
operation except those on the shader, and the sum does not predict the board: for the product graph it is 119 microseconds against
55 measured, and for the graph that ends in the shader's exponential it is 362 against 483. A cost of one operation in this note
is the difference between the measurements of two graphs that differ in that operation only.

## Which unit runs which operation

The loader reports the unit of each operation of a compiled graph as its kind. The vendor's layer names show which operations
share a kind.

| Unit (loader kind) | Operations the compiler placed there | Notes |
| --- | --- | --- |
| Neural-network core (2) | `MatMul` and `BatchGemm`; `Add` and `Mul` of two tensors of equal shape | Runs fixed-point products at the highest rate: 55 microseconds for the four-head 128-by-44 by 44-by-128 product. |
| Stream processor (6) | `Sub` (in every graph measured); `Add` and `Mul` with a broadcast operand; `ReduceMax`; `Softmax` | `Softmax` is 40 operations for the four-head, 128-row case and costs 82 microseconds over its input. |
| Transpose unit (8) | `StrideSlice` (`Slice`), `Transpose`, `Concat` | The two slices that align the position scores of 65,536 elements cost 81 microseconds together. |
| Activation unit (7) | `Relu`, `Sigmoid`, `Softplus`, `Elu` | `Elu` of 65,536 elements is one operation. |
| Shader (1) | `Exp`, `Div`, `Gather`, `Tile`, with one exception below | No cycle estimate. See the costs below. |

Exception: an `Exp` that takes the output of the stream processor's `Mul` and `Add` operations directly, on a tensor of at most 64
columns, adds no operation and no time. Two graphs that return the scores of four heads and 128 rows, one as the scores and one
as `Exp` of the first 8 or 64 columns, both measured 291.6 microseconds. The result is half precision. The same `Exp` after a
`Sub` with a broadcast operand and a slice does not fuse: the 64-column halves cost 457 microseconds over the row maximum,
against 269 for the shader's full-width `Sub` and `Exp`.

## Costs measured at 65,536 elements

| Operation | Unit | Cost (microseconds) |
| --- | --- | --- |
| `Exp` | shader | 192 |
| `Gather` that aligns the position scores | shader | 260 |
| Slice-based alignment of the same scores (two slices) | transpose unit | 81 |
| `Sub` with a broadcast operand | stream processor | 77 |
| `Tile` of a `[4, 128, 1]` tensor to `[4, 128, 128]` | shader | not measured alone; a probe graph whose only shader operation is the `Tile` took 495.8 in all |
| Three `Tile` operations in the 64-row step: `[4, 64, 1]` to `[4, 64, 128]` and twice to `[4, 64, 44]` | shader | about 170 together, 57 each: the step took about 304 against 229.4 without them while the estimates of its other operations fell by 95 (record `1794491788400711310-b14ea0fe`) |
| Native `Softmax` | stream processor | 82 |
| `ReduceMax` over the last axis | stream processor | 5 |
| `Elu` of the same tensor (the compiler estimates 113 microseconds) | activation unit | not measured alone; it replaced the shader `Exp` in a 64-row job and saved 88 of that job's 391 microseconds |
| Product of 4 by 128 by 44 with 4 by 44 by 128 | neural-network core | 55 |
| Product of 4 by 128 by 44 with 4 by 44 by 255 | neural-network core | 137 |

`Elu(x) + 1` equals `exp(x)` for `x` at or below zero. Every exponential of a difference from a maximum has that sign.
Replacing the shader `Exp` of the step (the tile's exponentials and the two rescaling factors) by `Elu` plus one lowered
the job of 64 query rows from 391.2 to 303.3 microseconds and the job of 128 rows from 906.6 to 731.0.

## The cost of a graph does not add up: on-chip memory

The NPU has 512 KiB of on-chip memory (VIP SRAM). When the tensors of a graph that are live at one time exceed it, the compiler
places intermediates in DRAM and the same operation costs several times its cost in memory. The signature is a cost that grows
faster than the number of elements:

| Graph | Heads or rows | Microseconds |
| --- | --- | --- |
| Scores of 128 rows | 1, 2, 4, 8 heads | 58.3, 112.7, 291.6, 729.2 |
| Online-softmax step with the native `Softmax` | 128 rows, 64 rows, 32 rows | 871.4, 303.3, 161.8 |
| Online-softmax step with `Elu` | 128 rows, 64 rows | 731.0, 303.3 |

The `Softmax` step shows the effect most clearly: consuming the `Softmax` output with a `ReduceMax` adds 420 microseconds at 128
rows (793.5 over the 373.3 of the `Softmax` alone), and at 64 rows the step costs 303.3, which is less than half of the 128-row step. The
compiler's own estimates for the upstream operations of the 128-row graph rise to between 1.9 and 11 times those of the graph
that ends at the `Softmax`, while the operations themselves are unchanged.

A graph author reduces the live tensors by running fewer rows per job. The optimum is where the per-job overhead and the
per-operation overhead stop being negligible: for the attention step it is 64 rows (two jobs of 303.3 microseconds against four
of 161.8, 647 in all).

## Shapes the compiler refuses or lowers badly

- A `MatMul` whose inner dimension is 1 (used to spread a `[H, N, 1]` tensor across a row) is refused at export:
  "Fatal model generation error: 64768".
- `Tile` is accepted and the compiler lowers it to a shader operation, so it is not a cheap broadcast.
- `Exp` of every shape tried (`[4,128,128]`, `[4,16384,1]`, `[1,65536,1]`, `[64,1024,1]`, `[4,4096,4]`, `[4,128,16]`, `[4,64,1]`)
  is a shader operation, except in the fused case above.

## Consequences for graph and SDK design

- Use `Elu` plus one for `exp` of non-positive arguments (softmax numerators after subtracting the maximum).
- Take a row sum from a product with a column of ones appended to the values, and a reciprocal of the small result times the
  output, instead of a shader division of the large output.
- Split a step into jobs of 64 query rows. The jobs of one key tile share the key, value and position inputs, which the current
  queue (fixed tensor bindings per handle) makes the application copy for every job; the per-job rebinding of
  `docs/designs/npu-tiled-execution.md`, section 6, removes those copies.

## Evidence

Single-job latency entries, from the board queue (each holds a receipt with the 100 ticks): scores of 1, 2, 4 and 8 heads
`1793655194828963698-91b8b70e`, `1793655224596920795-6c872c06`, `1793652973276364590-45a688ca`, `1793655876932723276-43ebed66`;
product `1793651961578323231-c47283de`; position product `1793652601052226254-0335ff5d`; alignment by gather
`1793652681733470371-7f0517a7` and by slice `1793652942776575668-83acfd56`; row maximum `1793653152953376465-60145f85`; native
softmax `1793653617346409812-fdcdc369`; shader exponential `1793653649220474759-9453e28a`; fused exponential
`1793657636102823244-2e4617a4`; softmax with a consumer `1793654641792039586-cfd92d72`; probes with the tile
`1793664173227863386-90f70e13` and with the exponential on halves `1793663674220336593-99dbc180`; steps
`1793666062767994561-6a9d8cc7` (softmax, 64 rows), `1793666001596247189-bae5598b` (softmax, 128 rows), `1793666171093864199-12181392`
(Elu, 64 rows), `1793666202399765909-18a4fce7` (Elu, 128 rows), `1793665620604466101-a312e346` (shader exponential, 64 rows),
`1793665824405367432-7def0e13` (shader exponential, 128 rows).
