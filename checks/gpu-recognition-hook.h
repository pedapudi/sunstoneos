// The exact-sum hook as the mimus engine would declare it, in
// firmware/components/asr/include/mimus/asr/exact_sum_hook.h of the mimus
// repository (docs/designs/gpu-recognition.md, "Changes needed in the mimus
// engine", item 2). The engine does not have it yet. The SunstoneOS hook in
// apps/os/gpu_hook.cc compiles against this declaration when the engine's
// header exists; checks/gpu-recognition-engine.py compiles it against this
// copy and models the engine's side of the contract.
#ifndef MIMUS_ASR_EXACT_SUM_HOOK_H_
#define MIMUS_ASR_EXACT_SUM_HOOK_H_

#include <cstddef>
#include <cstdint>

namespace mimus::asr {

// One job's exact integer sums: rows [first_row, first_row + rows) of the
// call by output channels [0, n_gpu). The sum of row r and channel c is the
// little-endian int32 (width 4) or int64 (width 8) at
// sums + (r - first_row) * stride + c * width bytes. Valid until the next
// finish() call.
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
// begin(ctx, ordinal, rows, k, n, codes, stride) once per call, after every
// row's codes and scales are final (after the producers' ParallelFor join),
// with the tensor's ordinal (TensorView::ordinal), the call's rows, K and N,
// row 0's codes and the row stride in bytes. codes lie in the inference
// arena; stride is a multiple of 4 and at least 2 * k. It returns n_gpu,
// 0 <= n_gpu <= n: the engine computes output channels [n_gpu, n) itself.
//
// After its own channels, the engine calls finish(ctx, &job) until it
// returns false. Each true return hands over one job, which the engine reads
// out with its own conversion (float(sum), then Combine with the call's
// scales, bias, alpha and residual). A false return hands over no rows: the
// engine then computes channels [0, n_gpu) of every row that no job covered.
//
// Under an active ScopedBlockSink the engine reports the hooked call's
// columns as kernels.h requires: it applies a sink's elementwise operation
// to each column the GPU produced as it stores it, after the CPU columns'
// done() calls; without elementwise it holds done() for the call and calls
// done(0, n) once, on worker 0, after every column of every row is final.
// The streamed SRAM-ring path is off for hooked calls.
struct ExactSumHook {
  size_t (*begin)(void* ctx, uint32_t ordinal, size_t rows, size_t k, size_t n,
                  const int16_t* codes, size_t stride);
  bool (*finish)(void* ctx, ExactSumJob* job);
  void* ctx;
};

// Installs the hook for later matmul calls (null removes it). The hook
// stays owned by the caller.
void SetExactSumHook(const ExactSumHook* hook);

}  // namespace mimus::asr

#endif  // MIMUS_ASR_EXACT_SUM_HOOK_H_
