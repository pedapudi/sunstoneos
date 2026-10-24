// A copy of firmware/components/asr/include/mimus/asr/exact_sum_hook.h of
// the mimus repository, commit 9c57286b7, whose engine implements the hook
// (docs/designs/gpu-recognition.md, "Changes needed in the mimus engine").
// checks/gpu-recognition-engine.py compiles apps/os/gpu_hook.cc against this
// copy and a model of the engine's side of the contract. The declarations
// below must stay token-identical to the engine's.
//
// An installable producer of exact integer sums for the integer matmuls
// (kernels.cc), through which an embedded application can compute some of
// a matmul's output channels on another device, such as a GPU. The engine
// reads the sums out with its own arithmetic, so the outputs are
// bit-identical to the CPU path. Host and AArch64 builds only; the ESP
// targets do not compile the hook.
#ifndef MIMUS_ASR_EXACT_SUM_HOOK_H_
#define MIMUS_ASR_EXACT_SUM_HOOK_H_

#include <cstddef>
#include <cstdint>

namespace mimus::asr {

// One job's exact integer sums: rows [first_row, first_row + rows) of the
// call by output channels [0, n_gpu). The sum of row r and channel c is the
// little-endian int32 (width 4) or int64 (width 8) at
// sums + (r - first_row) * stride + c * width bytes. The engine reads them
// on both inference workers, so they must stay valid, and visible to the
// second worker's core, until the next finish() call.
struct ExactSumJob {
  size_t first_row;
  size_t rows;
  const void* sums;
  size_t stride;
  unsigned width;
};

// Installed exact-sum producer for int8 weights and int16 activation codes
// with one scale per row, on AArch64. Linear, LinearAccumulate,
// LinearProduced, LinearProducedParallel and LinearProducedAccumulate call
// it as follows, on the task that installed it:
//
// begin(ctx, ordinal, weights, weight_stride, rows, k, n, codes, stride)
// once per call, after every row's codes and scales are final (after the
// producers' ParallelFor join), with the tensor's ordinal
// (TensorView::ordinal), its int8 weight codes (TensorView::data, channel 0)
// and weight row stride in bytes (TensorView::row_stride), the call's rows,
// K and N, row 0's codes and the code row stride in bytes. codes lie in the
// inference arena; stride is a multiple of 4 and at least 2 * k. It returns
// n_gpu, 0 <= n_gpu <= n: the engine computes output channels [n_gpu, n)
// itself. The hook compares the weights, their stride, K and N with the
// tensor it admitted under that ordinal and returns 0 on any difference, so
// an ordinal that names another tensor of the same shape never uses its
// weights.
//
// After its own channels, the engine calls finish(ctx, &job) until it
// returns false or refuses a job. Each true return hands over one job, under
// these rules:
//  - Jobs arrive in row order: a job's first_row is the row after the
//    previous job's last row (0 for the first job).
//  - Width 4 is refused when kp (K rounded up to 16) exceeds 512, where an
//    int32 sum can overflow; width 8 is accepted for any K. The stride must
//    be at least n_gpu * width.
//  - The engine reads a job's rows on both inference workers, the rows
//    split between them.
//  - Each output is read out in the CPU path's order: float(sum), rounded
//    once; times the weight scale, then times the row's activation scale;
//    plus the bias (or +0); then, for LinearAccumulate, y + alpha * value,
//    and otherwise the sink's elementwise operation, if any.
// After a false return, or a refused job (finish() is not called again for
// the call), the engine computes channels [0, n_gpu) of the rows after the
// last accepted job.
//
// Under an active ScopedBlockSink the engine reports the hooked call's
// columns as kernels.h requires: it applies a sink's elementwise operation
// to each column the GPU produced as it stores it, after the CPU columns'
// done() calls; without elementwise it holds done() for the call and calls
// done(0, n) once, on worker 0, after every column of every row is final.
// The streamed SRAM-ring path is off for hooked calls.
struct ExactSumHook {
  size_t (*begin)(void* ctx, uint32_t ordinal, const int8_t* weights,
                  size_t weight_stride, size_t rows, size_t k, size_t n,
                  const int16_t* codes, size_t stride);
  bool (*finish)(void* ctx, ExactSumJob* job);
  void* ctx;
};

// Installs the hook for later matmul calls (null removes it). The hook
// stays owned by the caller.
void SetExactSumHook(const ExactSumHook* hook);

}  // namespace mimus::asr

#endif  // MIMUS_ASR_EXACT_SUM_HOOK_H_
