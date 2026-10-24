// Host model of the mimus engine's side of the exact-sum hook
// (checks/gpu-recognition-hook.h) against the SunstoneOS hook in
// apps/os/gpu_hook.cc and the dispatcher in apps/os/dispatch.c.
//
// The engine model follows the contract the hook header states: begin once
// per call after the codes are final, the engine's own channels [n_gpu, N),
// then finish until it returns false, each job read out with the engine's
// conversion (float(sum), then Combine's order with bias and residual), and
// channels [0, n_gpu) of every row no job covered computed on the CPU. Under
// a block sink it applies an elementwise sink to the GPU's columns as it
// stores them, and otherwise holds done() and calls done(0, N) once. A
// modeled GPU runtime (a733_gpu_mimus_*) computes exact sums from the codes
// in the arena and the weights in the model region, writes them and the
// tags into the output region, and can refuse, lose a job, drop a tag or
// corrupt one sum. Every output of every case is compared bit for bit with
// the engine's CPU-only path.
//
// The arena, model and output addresses are the image's: the runner maps
// 0x44000000-0x47000000 at those addresses before main runs.
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <vector>

extern "C" {
#include "dispatch.h"
#include "pvr_compute_mimus.h"
}
#ifdef ENGINE_HOOK
#include <span>
#include "mimus/asr/exact_sum_hook.h"
#include "mimus/asr/kernels.h"
#endif

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

// The tensors of the generated test table (checks/gpu-recognition-engine.py):
// K, N, the weight stride and the weights' offset in the base model region,
// for every ordinal, including those the table keeps on the CPU.
struct Tensor { uint32_t k, n, stride, offset; };
#include "engine-tensors.h"
extern "C" const struct os_placement os_placement_table[];
extern "C" const struct os_placement_tensor os_placement_tensors[];
extern "C" const uint32_t os_placement_rows, os_placement_gpu_rows;
#ifdef ENGINE_HOOK
// The installed hook and the ordinals it is offered (SetExactSumOrdinals; an
// empty span offers every call), as kernels.cc keeps them.
static const mimus::asr::ExactSumHook *engine_hook;
static std::span<const uint8_t> engine_ordinals;
static bool offered(uint32_t ordinal)
{
    return engine_ordinals.empty() ||
        (ordinal / 8 < engine_ordinals.size() && (engine_ordinals[ordinal / 8] >> (ordinal % 8) & 1));
}
#endif

// ---- Modeled recognizer and GPU runtime -----------------------------------
static jmp_buf stop;
static unsigned board_fails, opens, open_reply = 1;
extern "C" void os_dispatch_stop(void) { ++board_fails; longjmp(stop, 1); }
extern "C" int a733_gpu_mimus_open(void) { ++opens; return open_reply; }
extern "C" int os_thermal_throttled(void) { return 0; }

enum Fault { NONE, REFUSE_SUBMIT, REFUSE_RECORD, LOSE, DROP_TAG, CORRUPT_SUM, LOSE_AT_SUBMIT, STALE_RECORD };
static struct { unsigned job; Fault fault; } script = {~0u, NONE};
static unsigned submitted_jobs, sequence;
static pvr_mimus_request in_flight;
static bool busy;
static int thermal_result = 1;
static unsigned thermal_calls;
static unsigned retire_calls, retire_reply = 1, consumed_jobs;
static bool output_borrowed;
extern "C" int a733_gpu_mimus_thermal(void)
{ CHECK(!busy); ++thermal_calls; return thermal_result; }
extern "C" int a733_gpu_mimus_retire(void)
{
    CHECK(!busy && !output_borrowed && consumed_jobs == script.job);
    CHECK(os_dispatch_record.incomplete && !os_dispatch_active);
    ++retire_calls;
    return retire_reply;
}

extern "C" int a733_gpu_mimus_tensor(uint32_t ordinal, struct os_gpu_tensor *out)
{
    for (uint32_t i = 0; i < os_placement_gpu_rows; ++i)
        if (os_placement_tensors[i].ordinal == ordinal) {
            const struct os_placement_tensor &t = os_placement_tensors[i];
            *out = {t.k, t.n, t.stride, reinterpret_cast<const int8_t *>(OS_DISPATCH_MODELS + t.offset)};
            return 1;
        }
    return 0;
}
extern "C" int a733_gpu_mimus_begin(const struct pvr_mimus_request *job)
{
    CHECK(!busy && job->rows);
    unsigned index = submitted_jobs++;
    if (index == script.job && script.fault == REFUSE_SUBMIT) return 0;
    if (index == script.job && script.fault == LOSE_AT_SUBMIT) return -1;
    in_flight = *job;
    busy = true;
    return 1;
}
extern "C" void a733_gpu_mimus_finish(struct pvr_mimus_record *done)
{
    CHECK(busy);
    busy = false;
    unsigned index = submitted_jobs - 1;
    CHECK(os_dispatch_gpu_state() == OS_DISPATCH_GPU_WAITING);
    if (index == script.job && script.fault == REFUSE_RECORD) { *done = {sequence + 1, PVR_MIMUS_REFUSED}; return; }
    if (index == script.job && script.fault == LOSE) { *done = {sequence + 1, PVR_MIMUS_LOST}; return; }
    os_gpu_tensor t;
    CHECK(a733_gpu_mimus_tensor(in_flight.ordinal, &t));
    const uint32_t k = t.k, stride = t.stride;
    const int8_t *w = t.weights;
    const os_placement p = os_placement_table[in_flight.ordinal];
    const unsigned width = k > 512 ? 8 : 4, n_gpu = p.n_gpu;
    // The output region is read-only in the image; the modeled GPU writes it.
    auto *sums = reinterpret_cast<unsigned char *>(OS_DISPATCH_OUTPUT + OS_DISPATCH_TAG_BYTES);
    for (uint32_t r = 0; r < in_flight.rows; ++r) {
        const auto *x = reinterpret_cast<const int16_t *>(OS_DISPATCH_ARENA + in_flight.x_offset + r * in_flight.x_stride);
        for (uint32_t c = 0; c < n_gpu; ++c) {
            int64_t s = 0;
            for (uint32_t i = 0; i < k; ++i) s += int64_t{x[i]} * w[size_t{c} * stride + i];
            if (index == script.job && script.fault == CORRUPT_SUM && r == 1 && c == 3) s += int64_t{1} << 20;
            if (width == 4) { int32_t v = static_cast<int32_t>(s); std::memcpy(sums + (size_t{r} * n_gpu + c) * 4, &v, 4); }
            else std::memcpy(sums + (size_t{r} * n_gpu + c) * 8, &s, 8);
        }
    }
    ++sequence;
    const uint32_t tags = pvr_mimus_tags(in_flight.rows, n_gpu);
    auto *tag = reinterpret_cast<uint32_t *>(OS_DISPATCH_OUTPUT + OS_DISPATCH_TAG_BYTES) - tags;
    for (uint32_t i = 0; i < tags; ++i) tag[i] = sequence;
    if (index == script.job && script.fault == DROP_TAG) tag[tags / 2] = sequence - 1;
    *done = {sequence, PVR_MIMUS_OK};
    if (index == script.job && script.fault == STALE_RECORD) --done->sequence;
}

// ---- Engine model ---------------------------------------------------------
struct Sink {
    void (*done)(void *ctx, size_t n0, size_t nb, int worker);
    void *ctx;
    void (*elementwise)(float *values, size_t n);
};
static float sigmoid(float v) { return 1.0f / (1.0f + std::exp(-v)); }
static void silu(float *v, size_t n) { for (size_t i = 0; i < n; ++i) v[i] = v[i] * sigmoid(v[i]); }
// GLU over N = 2d columns: out[r][c - d] = y[r][c - d] * sigmoid(y[r][c]) for each
// reported column c >= d; it reads column c - d, which must be final.
struct Glu { const float *y; float *out; size_t rows, d, ldy; };
static void glu_done(void *ctx, size_t n0, size_t nb, int)
{
    auto *g = static_cast<Glu *>(ctx);
    for (size_t r = 0; r < g->rows; ++r)
        for (size_t c = n0; c < n0 + nb; ++c)
            if (c >= g->d) g->out[r * g->d + c - g->d] = g->y[r * g->ldy + c - g->d] * sigmoid(g->y[r * g->ldy + c]);
}

struct Call {
    uint32_t ordinal;
    size_t rows, k, n, stride;          // stride in bytes between code rows
    const int16_t *codes;               // in the arena
    const int8_t *w; size_t w_stride;   // weight codes, row per channel
    std::vector<float> ws, xs, bias;
    float alpha; bool residual;
};
// Combine's order (kernels.cc): (0 + d * ws * xs) + bias, then the residual.
static void store(const Call &c, float *y, size_t r, size_t ch, float d, const Sink *sink)
{
    float acc = 0;
    acc += d * c.ws[ch] * c.xs[r];
    float v = acc + (c.bias.empty() ? 0.0f : c.bias[ch]);
    float &out = y[r * c.n + ch];
    out = c.residual ? out + c.alpha * v : v;
    if (sink && sink->elementwise) sink->elementwise(&out, 1);
}
static int64_t exact(const Call &c, size_t r, size_t ch)
{
    const int16_t *x = reinterpret_cast<const int16_t *>(reinterpret_cast<const unsigned char *>(c.codes) + r * c.stride);
    int64_t s = 0;
    for (size_t i = 0; i < c.k; ++i) s += int64_t{x[i]} * c.w[ch * c.w_stride + i];
    return s;
}
static float convert(int64_t s, size_t k) { return k <= 512 ? static_cast<float>(static_cast<int32_t>(s)) : static_cast<float>(s); }
// One matmul call. order_bug reports the CPU columns before the GPU's are
// final, which the contract forbids (the negative control).
static void linear(const Call &c, float *y, const Sink *sink, bool use_hook, bool order_bug = false)
{
    size_t n_gpu = 0;
#ifdef ENGINE_HOOK
    if (use_hook && engine_hook && offered(c.ordinal))
        n_gpu = engine_hook->begin(engine_hook->ctx, c.ordinal, c.w, c.w_stride, c.rows, c.k, c.n, c.codes, c.stride);
#else
    (void)use_hook;
#endif
    CHECK(n_gpu <= c.n);
    // The engine's own channels; with no GPU, done() per 16-column block in
    // increasing order as the CPU path reports them.
    for (size_t r = 0; r < c.rows; ++r)
        for (size_t ch = n_gpu; ch < c.n; ++ch) store(c, y, r, ch, convert(exact(c, r, ch), c.k), sink);
    if (sink && !sink->elementwise && (!n_gpu || order_bug))
        for (size_t n0 = n_gpu; n0 < c.n; n0 += 16) sink->done(sink->ctx, n0, n0 + 16 <= c.n ? 16 : c.n - n0, 0);
    std::vector<bool> covered(c.rows, false);
#ifdef ENGINE_HOOK
    if (n_gpu) {
        // The hook may remove itself inside finish; this call keeps it.
        const mimus::asr::ExactSumHook *hook = engine_hook;
        mimus::asr::ExactSumJob job;
        size_t next = 0;
        while (hook->finish(hook->ctx, &job)) {
            output_borrowed = true;
            // The engine's rules: jobs in row order, int64 sums when K
            // rounded up to 16 exceeds 512, a stride that holds the sums.
            CHECK(job.first_row == next && job.rows && job.first_row + job.rows <= c.rows &&
                  (job.width == 8 || (job.width == 4 && (c.k + 15) / 16 * 16 <= 512)) &&
                  job.stride == n_gpu * job.width);
            next += job.rows;
            for (size_t r = 0; r < job.rows; ++r) {
                CHECK(!covered[job.first_row + r]);
                covered[job.first_row + r] = true;
                const unsigned char *row = static_cast<const unsigned char *>(job.sums) + r * job.stride;
                for (size_t ch = 0; ch < n_gpu; ++ch) {
                    int64_t s;
                    if (job.width == 4) { int32_t v; std::memcpy(&v, row + ch * 4, 4); s = v; }
                    else std::memcpy(&s, row + ch * 8, 8);
                    store(c, y, job.first_row + r, ch, convert(s, c.k), sink);
                }
            }
            output_borrowed = false;
            ++consumed_jobs;
        }
    }
#endif
    for (size_t r = 0; r < c.rows; ++r)
        if (!covered[r]) for (size_t ch = 0; ch < n_gpu; ++ch) store(c, y, r, ch, convert(exact(c, r, ch), c.k), sink);
    if (sink && !sink->elementwise && n_gpu) {
        if (order_bug) for (size_t n0 = 0; n0 < n_gpu; n0 += 16) sink->done(sink->ctx, n0, n0 + 16 <= n_gpu ? 16 : n_gpu - n0, 0);
        else sink->done(sink->ctx, 0, c.n, 0);
    }
}
#ifdef ENGINE_HOOK
namespace mimus::asr {
void SetExactSumHook(const ExactSumHook *hook) { engine_hook = hook; }
void SetExactSumOrdinals(std::span<const uint8_t> placed) { engine_ordinals = placed; }
}
#endif

// ---- Cases ------------------------------------------------------------------
static uint64_t seed = 0x2545f4914f6cdd1du;
static uint32_t rnd() { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return static_cast<uint32_t>(seed); }
enum SinkKind { PLAIN, SILU, GLU };
struct Spec { uint32_t ordinal; size_t rows; SinkKind sink; bool residual; };

// weights_of names the tensor whose weights the engine passes; it differs
// from the ordinal only in the case that models an ordinal mismatch.
static Call make(const Spec &s, size_t arena_offset, uint32_t weights_of)
{
    Call c{};
    c.ordinal = s.ordinal;
    const Tensor &t = engine_tensors[weights_of];
    const size_t k = t.k;
    c.k = k; c.w = reinterpret_cast<const int8_t *>(OS_DISPATCH_MODELS + t.offset); c.w_stride = t.stride;
    c.rows = s.rows;
    // Codes rows padded to a multiple of 4 bytes, as the engine allocates them.
    c.stride = (2 * k + 4 + 3) & ~size_t{3};
    auto *codes = reinterpret_cast<int16_t *>(OS_DISPATCH_ARENA + arena_offset);
    for (size_t r = 0; r < s.rows; ++r)
        for (size_t i = 0; i < c.stride / 2; ++i)
            codes[r * (c.stride / 2) + i] = i < k ? static_cast<int16_t>(r == 0 ? 32767 : static_cast<int32_t>(rnd() % 65535) - 32767) : 0x5a5a;
    c.codes = codes;
    c.residual = s.residual; c.alpha = 0.5f;
    return c;
}

// Runs one call through the hook and through the CPU path from the same
// starting outputs; returns 1 when every output is bit-identical.
static int compare(const Spec &s, size_t n, bool order_bug = false, uint32_t weights_of = ~0u)
{
    Call c = make(s, 0x10000, weights_of == ~0u ? s.ordinal : weights_of);
    CHECK(engine_tensors[s.ordinal].n == n);
    c.n = n;
    c.ws.resize(n); c.xs.resize(c.rows); c.bias.resize(n);
    for (auto &v : c.ws) v = std::ldexp(static_cast<float>(rnd() % 2048 + 1), -18);
    for (auto &v : c.xs) v = std::ldexp(static_cast<float>(rnd() % 4096 + 1), -14);
    for (auto &v : c.bias) v = static_cast<float>(static_cast<int32_t>(rnd() % 2001) - 1000) / 256.0f;
    std::vector<float> start(c.rows * n);
    for (auto &v : start) v = static_cast<float>(static_cast<int32_t>(rnd() % 2001) - 1000) / 64.0f;
    std::vector<float> a = start, b = start, out_a(c.rows * n / 2), out_b(c.rows * n / 2);
    Glu ga{a.data(), out_a.data(), c.rows, n / 2, n}, gb{b.data(), out_b.data(), c.rows, n / 2, n};
    Sink sa{glu_done, &ga, nullptr}, sb{glu_done, &gb, nullptr}, silu_sink{nullptr, nullptr, silu};
    const Sink *sink_a = s.sink == GLU ? &sa : s.sink == SILU ? &silu_sink : nullptr;
    const Sink *sink_b = s.sink == GLU ? &sb : s.sink == SILU ? &silu_sink : nullptr;
    if (setjmp(stop)) return -1;
    linear(c, a.data(), sink_a, true, order_bug);
    linear(c, b.data(), sink_b, false);
    int same = !std::memcmp(a.data(), b.data(), a.size() * 4) &&
        (s.sink != GLU || !std::memcmp(out_a.data(), out_b.data(), out_a.size() * 4));
    return same;
}
static void reset()
{
    std::memset(&os_dispatch_record, 0, sizeof(os_dispatch_record));
    os_dispatch_active = 0;
#ifdef ENGINE_HOOK
    engine_hook = nullptr;
    engine_ordinals = {};
#endif
    opens = board_fails = 0; open_reply = 1; script = {~0u, NONE}; submitted_jobs = 0; busy = false;
    retire_calls = consumed_jobs = 0; retire_reply = 1; output_borrowed = false;
    os_gpu_open();
}

// The image's model, arena and output addresses, and the weight codes: row 0
// of each tensor at -128, the rest random.
static void map_regions()
{
    void *p = mmap(reinterpret_cast<void *>(OS_DISPATCH_MODELS), OS_DISPATCH_READ_ONLY + OS_DISPATCH_READ_ONLY_BYTES -
                   OS_DISPATCH_MODELS, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    CHECK(p == reinterpret_cast<void *>(OS_DISPATCH_MODELS));
    for (const Tensor &t : engine_tensors) {
        auto *w = reinterpret_cast<int8_t *>(OS_DISPATCH_MODELS + t.offset);
        for (size_t i = 0; i < size_t{t.n} * t.stride; ++i)
            w[i] = i < t.k ? -128 : static_cast<int8_t>(static_cast<int32_t>(rnd() % 256) - 128);
    }
}

int main(int argc, char **argv)
{
    const bool thermal_only = argc == 2 && std::strcmp(argv[1], "--thermal-only") == 0;
    const bool retire_only = argc == 2 && std::strcmp(argv[1], "--retire-only") == 0;
    map_regions();
    // The table: see checks/gpu-recognition-engine.py.
    struct { Spec spec; size_t n; } nominal[] = {
        {{0, 222, SILU, false}, 704},  // feed-forward first linear, n_gpu 320 < N, four jobs
        {{1, 222, GLU, false}, 352},   // pointwise convolution 1, n_gpu 160 < d = 176
        {{2, 222, PLAIN, true}, 176},  // feed-forward second linear, K 704: int64 sums, residual
        {{3, 222, GLU, false}, 352},   // pointwise convolution 1 with n_gpu = N
        {{4, 222, SILU, false}, 704},  // feed-forward first linear with n_gpu = N
        {{5, 70, PLAIN, false}, 176},  // subsampling convolution, K 1,584, n_gpu = N
        {{6, 100, PLAIN, false}, 176}, // a tensor the table keeps on the CPU
        {{7, 100, PLAIN, false}, 176}, // a second 176 x 176 tensor on the GPU
    };
    reset();
#ifndef ENGINE_HOOK
    CHECK(!thermal_only && !retire_only);
    // Without the engine's hook header the session is never opened.
    CHECK(!opens && !os_dispatch_place(0).n_gpu);
    for (auto &t : nominal) CHECK(compare(t.spec, t.n) == 1);
    CHECK(!submitted_jobs);
    std::printf("PASS engine model without the hook: no OPEN, %zu calls on the CPU\n", sizeof(nominal) / sizeof(*nominal));
    return 0;
#else
    CHECK(opens == 1 && os_dispatch_record.opened && engine_hook);
    if (retire_only) {
        const Spec two = {0, 66, SILU, false};
        script = {1, CORRUPT_SUM}; retire_reply = 0;
        CHECK(compare(two, 704) == -1 && board_fails == 1 && retire_calls == 1 &&
              consumed_jobs == 1 && os_dispatch_record.failure == OS_DISPATCH_LOST && !os_dispatch_active);
        CHECK(engine_hook); // failed shutdown does not silently remove the hook and continue
        for (Fault fault : {DROP_TAG, CORRUPT_SUM, STALE_RECORD}) {
            reset(); script = {1, fault};
            CHECK(compare(two, 704) == 1 && !board_fails && retire_calls == 1 && consumed_jobs == 1 &&
                  !engine_hook && !os_dispatch_active && os_dispatch_record.incomplete);
            const unsigned before = submitted_jobs;
            CHECK(compare(two, 704) == 1 && submitted_jobs == before && retire_calls == 1);
        }
        std::printf("PASS rejected output retirement: prior borrow consumed, no rejected output lent; "
                    "tag/sum/sequence rejection closes before CPU fallback; failed close is LOST\n");
        return 0;
    }
    if (thermal_only) {
        const Spec one = {0, 1, SILU, false};
        thermal_result = 0;
        CHECK(compare(one, 704) == 1 && !submitted_jobs && thermal_calls == 1);
        thermal_result = 1;
        CHECK(compare(one, 704) == 1 && submitted_jobs == 1 && thermal_calls == 2);
        os_dispatch_incomplete();
        CHECK(compare(one, 704) == 1 && submitted_jobs == 1 && thermal_calls == 2);
        reset(); thermal_result = -1;
        CHECK(compare(one, 704) == -1 && !os_dispatch_active && board_fails == 1);
        const unsigned before = thermal_calls;
        thermal_result = 1;
        CHECK(compare(one, 704) == 1 && thermal_calls == before && !submitted_jobs);
        std::printf("PASS hook thermal boundary: parked CPU path, ready GPU job, retained incomplete/failure admission\n");
        return 0;
    }
    // The hook is offered exactly the table's placed ordinals.
    CHECK(engine_ordinals.size() == (os_placement_rows + 7) / 8);
    for (uint32_t i = 0; i < os_placement_rows; ++i) CHECK(offered(i) == (os_placement_table[i].n_gpu != 0));
    unsigned jobs = 0, calls = 0;
    for (auto &t : nominal) {
        unsigned before = submitted_jobs;
        CHECK(compare(t.spec, t.n) == 1);
        jobs += submitted_jobs - before; calls += submitted_jobs != before;
    }
    CHECK(os_dispatch_record.placed == calls && !os_dispatch_record.refused && !os_dispatch_record.incomplete);
    CHECK(os_dispatch_gpu_state() == OS_DISPATCH_GPU_IDLE);
    std::printf("nominal: %u placed calls, %u jobs, every output bit-identical\n", calls, jobs);
    // An engine whose ordinal names another tensor of the same shape: the
    // hook sees other weights and leaves the call on the CPU.
    {
        unsigned before = submitted_jobs;
        uint64_t placed = os_dispatch_record.placed;
        CHECK(compare(nominal[7].spec, nominal[7].n, false, 6) == 1);
        CHECK(submitted_jobs == before && os_dispatch_record.placed == placed);
        std::printf("ordinal of another 176 x 176 tensor: refused at begin, computed on the CPU\n");
    }
    // The forbidden order: CPU columns reported to the GLU sink before the GPU's
    // low columns are final. The model detects it.
    reset();
    CHECK(compare(nominal[1].spec, nominal[1].n, true) == 0);
    std::printf("order control: reporting CPU columns first changes the GLU output, as expected\n");
    struct { Fault fault; unsigned job; const char *name; } faults[] = {
        {REFUSE_SUBMIT, 0, "first job refused at submit"}, {REFUSE_SUBMIT, 2, "third job refused at submit"},
        {REFUSE_RECORD, 1, "second job refused by the worker"}, {DROP_TAG, 1, "second job misses a tag"},
        {CORRUPT_SUM, 2, "third job has one wrong sum"},
    };
    for (auto &f : faults) {
        reset();
        script = {f.job, f.fault};
        // The first case runs the four-job SiLU call, then every other call.
        for (auto &t : nominal) CHECK(compare(t.spec, t.n) == 1);
        bool closes = f.fault == DROP_TAG || f.fault == CORRUPT_SUM;
        CHECK(!board_fails && os_dispatch_record.incomplete == (closes ? 1u : 0u));
        // Closing placement removes the hook, which restores the CPU paths.
        CHECK(closes ? !engine_hook : engine_hook != nullptr);
        CHECK(closes ? !os_dispatch_active : os_dispatch_record.refused == 1);
        std::printf("%s: every output bit-identical, %s\n", f.name, closes ? "placement closed for the boot" : "session open");
    }
    for (Fault f : {LOSE, LOSE_AT_SUBMIT}) {
        reset();
        script = {1, f};
        CHECK(compare(nominal[0].spec, nominal[0].n) == -1 && board_fails == 1 &&
              os_dispatch_record.failure == OS_DISPATCH_LOST && !os_dispatch_active);
    }
    std::printf("lost job: the boot ends inside finish, before the CPU computes any row of that job\n");
    std::printf("PASS engine model with the hook\n");
    return 0;
#endif
}
