// The SunstoneOS side of mimus's exact-sum hook (docs/recognition-dispatch.md,
// docs/designs/gpu-recognition.md). The recognizer task runs every function
// here: os_gpu_open once after the model hashes, then Begin and Finish from
// inside the engine's matmul calls. Without the engine's header the hook
// does not exist and os_gpu_open keeps the GPU closed and unpowered.
extern "C" {
#include "dispatch.h"
#include "pvr_compute_mimus.h"
}
#include <cstddef>
#include <cstdint>

#if __has_include("mimus/asr/exact_sum_hook.h")
#include "mimus/asr/exact_sum_hook.h"

namespace {
// The call in progress. One job is in flight at a time, because each job
// reuses the output region; the next is submitted only after the engine
// read the previous one out.
struct Call {
  uint64_t x_offset;
  size_t rows, next, stride, k;
  uint32_t ordinal, n_gpu, job_rows, width;
  bool in_flight, stopped;
  a733_gpu_mimus_job job;
};
Call call;
// The sequence of the last job that completed with OK; each OK record must
// carry the next one, so no earlier job's tags can pass.
uint32_t completed;

bool Submit() {
  size_t rows = call.rows - call.next;
  if (rows > call.job_rows) rows = call.job_rows;
  call.job = {call.x_offset + call.next * call.stride, call.ordinal, static_cast<uint32_t>(rows),
              static_cast<uint32_t>(call.stride)};
  const int submitted = a733_gpu_mimus_begin(&call.job);
  if (submitted < 0) os_dispatch_failed(OS_DISPATCH_LOST);
  if (submitted == 0) {
    os_dispatch_refused();
    call.stopped = true;
    return false;
  }
  call.in_flight = true;
  return true;
}

#ifdef SUNSTONE_A733_OS_GPU_VALIDATE
// The validation build's check: every GPU sum against a scalar int64 sum of
// the same codes and weights.
bool SumsEqual(size_t first, size_t rows) {
  uint32_t k, stride;
  const int8_t* weights;
  if (!a733_gpu_mimus_tensor(call.ordinal, &k, &stride, &weights)) return false;
  const auto* base = reinterpret_cast<const unsigned char*>(OS_DISPATCH_OUTPUT + OS_DISPATCH_TAG_BYTES);
  for (size_t r = 0; r < rows; ++r) {
    const auto* x = reinterpret_cast<const int16_t*>(OS_DISPATCH_ARENA + call.x_offset + (first + r) * call.stride);
    for (uint32_t c = 0; c < call.n_gpu; ++c) {
      int64_t sum = 0;
      for (size_t i = 0; i < k; ++i) sum += static_cast<int64_t>(x[i]) * weights[static_cast<size_t>(c) * stride + i];
      const unsigned char* p = base + (r * call.n_gpu + c) * call.width;
      const int64_t gpu = call.width == 4 ? *reinterpret_cast<const volatile int32_t*>(p)
                                          : *reinterpret_cast<const volatile int64_t*>(p);
      if (gpu != sum) return false;
    }
  }
  return true;
}
#endif

size_t Begin(void*, uint32_t ordinal, size_t rows, size_t k, size_t n, const int16_t* codes, size_t stride) {
  call = {};
  const os_placement p = os_dispatch_place(ordinal);
  if (!p.n_gpu || !p.job_rows || !rows || p.n_gpu > n) return 0;
  uint32_t admitted_k, weight_stride;
  const int8_t* weights;
  const uintptr_t x = reinterpret_cast<uintptr_t>(codes);
  // The engine's call must match the tensor admitted at start, and its codes
  // must lie in the arena span the GPU reads.
  if (!a733_gpu_mimus_tensor(ordinal, &admitted_k, &weight_stride, &weights) || admitted_k != k ||
      (stride & 3) || stride < 2 * k || stride > 0xffff || (x & 3) || x < OS_DISPATCH_ARENA)
    return 0;
  const uint64_t offset = x - OS_DISPATCH_ARENA, extent = (rows - 1) * stride + 2 * k;
  if (offset > OS_DISPATCH_CODE_BYTES || extent > OS_DISPATCH_CODE_BYTES - offset) return 0;
  call = {offset, rows, 0, stride, k, ordinal, p.n_gpu, p.job_rows, k > 512 ? 8u : 4u, false, false, {}};
  if (!Submit()) return 0;
  os_dispatch_placed();
  return p.n_gpu;
}

bool Finish(void*, mimus::asr::ExactSumJob* out) {
  if (call.stopped || call.next >= call.rows) return false;
  if (!call.in_flight && !Submit()) return false;
  a733_gpu_mimus_completion done;
  a733_gpu_mimus_finish(&done);
  call.in_flight = false;
  if (done.result == A733_GPU_MIMUS_LOST) os_dispatch_failed(OS_DISPATCH_LOST);
  if (done.result != A733_GPU_MIMUS_OK) {
    os_dispatch_refused();
    call.stopped = true;
    return false;
  }
  const size_t rows = call.job.rows;
  bool complete = done.sequence == completed + 1 &&
      pvr_mimus_tags_complete(OS_DISPATCH_OUTPUT, static_cast<uint32_t>(rows), call.n_gpu, done.sequence);
#ifdef SUNSTONE_A733_OS_GPU_VALIDATE
  complete = complete && SumsEqual(call.next, rows);
#endif
  if (done.sequence == completed + 1) completed = done.sequence;
  if (!complete) {
    // Placement closes for the boot; the session stays open and idle.
    os_dispatch_incomplete();
    call.stopped = true;
    return false;
  }
  *out = {call.next, rows, reinterpret_cast<const void*>(OS_DISPATCH_OUTPUT + OS_DISPATCH_TAG_BYTES),
          static_cast<size_t>(call.n_gpu) * call.width, call.width};
  call.next += rows;
  return true;
}

const mimus::asr::ExactSumHook hook{Begin, Finish, nullptr};
}  // namespace

extern "C" void os_gpu_open(void) {
  if (os_dispatch_open()) mimus::asr::SetExactSumHook(&hook);
}
#else
// The engine has no exact-sum hook: no call could reach the GPU, so the
// session is never opened and the GPU stays unpowered.
extern "C" void os_gpu_open(void) {}
#endif
