// The SunstoneOS side of mimus's exact-sum hook (docs/gpu-mimus-service.md,
// docs/designs/gpu-recognition.md). The recognizer task runs every function
// here: os_gpu_open once after the model hashes, then Begin and Finish from
// inside the engine's matmul calls. Without the engine's header the hook
// does not exist and os_gpu_open keeps the GPU closed and unpowered.
extern "C" {
#include "dispatch.h"
#include "gpu-progress.h"
#include "pvr_compute_mimus.h"
}
#include <cstddef>
#include <cstdint>

#if __has_include("mimus/asr/exact_sum_hook.h")
#include <span>
#include "mimus/asr/exact_sum_hook.h"
#include "mimus/asr/kernels.h"

namespace {
// The call in progress. One job is in flight at a time, because each job
// reuses the output region; the next is submitted only after the engine
// read the previous one out.
struct Call {
  uint64_t x_offset;
  size_t rows, next, stride;
  uint32_t ordinal, n_gpu, job_rows, width;
  bool in_flight, stopped;
  os_gpu_tensor tensor;
  pvr_mimus_request job;
};
Call call;
// The sequence of the last job that completed with OK; each OK record must
// carry the next one, so no earlier job's tags can pass.
uint32_t completed;

#ifdef SUNSTONE_A733_OS_GPU_BISECT
// Jobs the GPU ran: each submitted job either completes OK, is refused by the
// service before any device access (Finish takes it back), or ends the boot.
uint32_t bisect_jobs;
bool bisect_reached;
// Called once the limit refuses a job or a call; the CPU computes the rest.
void BisectReached() {
  if (bisect_reached) return;
  bisect_reached = true;
  sunstone_crumb(OS_GPU_CRUMB_BISECT_LIMIT, completed);
}
// Whether the call of this ordinal may reach the GPU at all.
bool BisectCall(uint32_t ordinal) {
#if OS_GPU_BISECT_STAGE == 3
  (void)ordinal;
  return bisect_jobs < OS_GPU_BISECT_LIMIT;
#elif OS_GPU_BISECT_STAGE == 4
  // An ordinal outside the table is left to os_dispatch_place, which refuses it.
  return ordinal >= os_placement_rows || os_placement_layer[ordinal] <= OS_GPU_BISECT_LIMIT;
#else
  (void)ordinal;
  return false;
#endif
}
#endif

bool Submit() {
#ifdef SUNSTONE_A733_OS_GPU_BISECT
  // The engine computes the rows after the last accepted job itself.
  if (OS_GPU_BISECT_STAGE == 3 && bisect_jobs >= OS_GPU_BISECT_LIMIT) {
    BisectReached();
    call.stopped = true;
    return false;
  }
#endif
  size_t rows = call.rows - call.next;
  if (rows > call.job_rows) rows = call.job_rows;
  call.job = {call.x_offset + call.next * call.stride, call.ordinal, static_cast<uint32_t>(rows),
              static_cast<uint32_t>(call.stride)};
  const int submitted = a733_gpu_mimus_begin(&call.job);
  if (submitted < 0) os_dispatch_failed(submitted == OS_GPU_CALL_TIMEOUT ? OS_DISPATCH_TIMEOUT : OS_DISPATCH_LOST);
  if (submitted == 0) {
    os_dispatch_refused();
    call.stopped = true;
    return false;
  }
  call.in_flight = true;
#ifdef SUNSTONE_A733_OS_GPU_BISECT
  ++bisect_jobs;
#endif
  sunstone_crumb(OS_GPU_CRUMB_JOB_SUBMIT, call.ordinal);
  return true;
}

// Every GPU sum against a scalar int64 sum of the same codes and weights.
// The check stays until the board shows GPU cache invalidation at job start,
// GPU read-only enforcement, cross-core code writes and fault-slot retention
// across a WDT0 reset (docs/designs/gpu-recognition.md); at two cycles per
// MAC it costs about 1.9 s per bench recording on the A76 pair.
bool SumsEqual(size_t first, size_t rows) {
  const auto* base = reinterpret_cast<const unsigned char*>(OS_DISPATCH_OUTPUT + OS_DISPATCH_TAG_BYTES);
  for (size_t r = 0; r < rows; ++r) {
    const auto* x = reinterpret_cast<const int16_t*>(OS_DISPATCH_ARENA + call.x_offset + (first + r) * call.stride);
    for (uint32_t c = 0; c < call.n_gpu; ++c) {
      int64_t sum = 0;
      for (size_t i = 0; i < call.tensor.k; ++i)
        sum += static_cast<int64_t>(x[i]) * call.tensor.weights[static_cast<size_t>(c) * call.tensor.stride + i];
      const unsigned char* p = base + (r * call.n_gpu + c) * call.width;
      const int64_t gpu = call.width == 4 ? *reinterpret_cast<const volatile int32_t*>(p)
                                          : *reinterpret_cast<const volatile int64_t*>(p);
      if (gpu != sum) return false;
    }
  }
  return true;
}

size_t Begin(void*, uint32_t ordinal, const int8_t* weights, size_t weight_stride, size_t rows, size_t k, size_t n,
             const int16_t* codes, size_t stride) {
  call = {};
  if (!os_dispatch_active) return 0;
#ifdef SUNSTONE_A733_OS_GPU_BISECT
  if (!BisectCall(ordinal)) {
    BisectReached();
    return 0;
  }
#endif
  const int ready = a733_gpu_mimus_thermal();
  if (ready < 0) os_dispatch_failed(ready == OS_GPU_CALL_TIMEOUT ? OS_DISPATCH_TIMEOUT : OS_DISPATCH_LOST);
  if (!ready) return 0;
  const os_placement p = os_dispatch_place(ordinal);
  if (!p.n_gpu || !p.job_rows || !rows) return 0;
  os_gpu_tensor t;
  const uintptr_t x = reinterpret_cast<uintptr_t>(codes);
  // The call must be the tensor admitted under this ordinal, weights included:
  // the attention projections share K and N, so an ordinal that names
  // another of them would otherwise run on the wrong weights unnoticed. Its
  // codes must lie in the arena span the GPU reads.
  if (!a733_gpu_mimus_tensor(ordinal, &t) || t.weights != weights || t.stride != weight_stride || t.k != k ||
      t.n != n || p.n_gpu > n || (stride & 3) || stride < 2 * k || stride > 0xffff || (x & 3) ||
      x < OS_DISPATCH_ARENA)
    return 0;
  const uint64_t offset = x - OS_DISPATCH_ARENA, extent = (rows - 1) * stride + 2 * k;
  if (offset > OS_DISPATCH_CODE_BYTES || extent > OS_DISPATCH_CODE_BYTES - offset) return 0;
  call = {offset, rows, 0, stride, ordinal, p.n_gpu, p.job_rows, k > 512 ? 8u : 4u, false, false, t, {}};
  if (!Submit()) return 0;
  os_dispatch_placed();
  return p.n_gpu;
}

bool Finish(void*, mimus::asr::ExactSumJob* out) {
  if (call.stopped || call.next >= call.rows) return false;
  if (!call.in_flight && !Submit()) return false;
  pvr_mimus_record done;
  // Only this wait counts as waiting on the GPU for the supervisor's stop
  // detail; the engine's own channels ran before it.
  os_dispatch_waiting(1);
  const int received = a733_gpu_mimus_finish(&done);
  if (received != 1) os_dispatch_failed(received == OS_GPU_CALL_TIMEOUT ? OS_DISPATCH_TIMEOUT : OS_DISPATCH_LOST);
  if (done.result == PVR_MIMUS_LOST) os_dispatch_failed(OS_DISPATCH_LOST);
  os_dispatch_waiting(0);
  call.in_flight = false;
  sunstone_crumb(OS_GPU_CRUMB_JOB_DONE, static_cast<uint32_t>(done.result) << 12 | (done.sequence & 0xfff));
  if (done.result != PVR_MIMUS_OK) {
#ifdef SUNSTONE_A733_OS_GPU_BISECT
    --bisect_jobs;
#endif
    os_dispatch_refused();
    call.stopped = true;
    return false;
  }
  const size_t rows = call.job.rows;
  const bool next = done.sequence == completed + 1;
  if (next) completed = done.sequence;
  if (!next || !pvr_mimus_tags_complete(OS_DISPATCH_OUTPUT, static_cast<uint32_t>(rows), call.n_gpu, done.sequence) ||
      !SumsEqual(call.next, rows)) {
    // This job lends no output to the engine. Previous output was consumed
    // before this Finish call. Retire through acknowledged shutdown before
    // removing the hook that also owns thermal reconciliation.
    os_dispatch_incomplete();
    if (!a733_gpu_mimus_retire()) os_dispatch_failed(OS_DISPATCH_LOST);
    mimus::asr::SetExactSumHook(nullptr);
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

#ifdef SUNSTONE_A733_OS_GPU_BISECT
extern "C" int os_gpu_retire(void) {
  // As below, but a limit may have refused the rest of the final call, and
  // stage 1 never opened the session. Every submitted job completed OK.
  if (call.in_flight || bisect_jobs != completed || os_dispatch_record.incomplete || os_dispatch_record.failure)
    return 0;
  // A refused constructor keeps its own untouched report.
  if (!completed && !os_dispatch_record.opened && os_dispatch_record.open_failed)
    return a733_gpu_mimus_return_untouched();
  if (os_dispatch_record.opened != (OS_GPU_BISECT_STAGE != 1)) return 0;
  mimus::asr::SetExactSumHook(nullptr);
  os_dispatch_active = 0;
  return a733_gpu_mimus_return_bisect(completed);
}
#elif defined(SUNSTONE_A733_OS_CPU_RETURN)
extern "C" int os_gpu_retire(void) {
  // Called after synchronous transcription and every parallel stop ACK.
  // No engine task can still read a previously lent output descriptor.
  if (!completed && !call.in_flight && call.next == call.rows)
    return a733_gpu_mimus_return_untouched();
  // A refused job stops its call early and the engine computes the rest of
  // the rows itself, so a stopped call is finished too.
  if (call.in_flight || (call.next != call.rows && !call.stopped) || !completed ||
      !os_dispatch_record.opened || os_dispatch_record.incomplete || os_dispatch_record.failure) return 0;
  mimus::asr::SetExactSumHook(nullptr);
  os_dispatch_active = 0;
  return a733_gpu_mimus_return(completed);
}
#endif
extern "C" void os_gpu_open(void) {
#ifdef SUNSTONE_A733_OS_GPU_BISECT
  // Stage 1 never sends OPEN, so dispatch stays inactive; stage 2 opens. In
  // both, the installed hook places no call.
  if (OS_GPU_BISECT_STAGE != 1 && !os_dispatch_open()) return;
  if (OS_GPU_BISECT_STAGE <= 2) BisectReached();
#else
  if (!os_dispatch_open()) return;
#endif
  // The engine offers the hook only the placed tensors' calls; every other
  // call keeps the CPU paths it has without a hook.
  mimus::asr::SetExactSumOrdinals(std::span<const uint8_t>(os_placement_gpu_ordinals, (os_placement_rows + 7) / 8));
  mimus::asr::SetExactSumHook(&hook);
}
#else
// The engine has no exact-sum hook: no call could reach the GPU, so the
// session is never opened and the GPU stays unpowered.
extern "C" void os_gpu_open(void) {}
#ifdef SUNSTONE_A733_OS_CPU_RETURN
extern "C" int os_gpu_retire(void) { return 0; }
#endif
#endif
