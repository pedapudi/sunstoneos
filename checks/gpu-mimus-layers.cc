// Host model of exact mimus linear layers on the GPU (docs/gpu-mimus.md).
//
// Runs the session code of drivers/gpu/pvr_compute_mimus.c, the kernel
// source shaders/pvr_mimus_linear.cl compiled as C over its grid, and the
// read-out entry include/pvr_compute_mimus_readout.h, and compares every
// output bit for bit with mimus's own CPU path (GemmCombine and
// MimusGemmS16Accumulate in gemm_combine_portable.cc).
//
// Memory model: the GPU reads and writes memory; the CPU sees memory through
// a write-back cache. Activation codes, job words and program bytes reach the
// GPU only through the driver's DC CVAC; the CPU sees GPU sums and tags only
// for lines the driver cleaned and invalidated. The output region is mapped
// PROT_READ outside the modeled GPU. aarch64_device_cache records each
// operation and its two DSB SY in the order the hook, worker and runtime
// models call them. No hardware is accessed.
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cinttypes>
#include <csignal>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

extern "C" {
#include "aarch64_device_memory.h"
#include "gpu-mimus-layers-shim.h"
#include "pvr_compute_mimus.h"
#include "pvr_compute_mimus_readout.h"
void gpu_mimus_lane(void);
}
#undef min
#include "mimus/asr/gemm.h"

namespace mimus::asr {
void MaybeYield() {}
}  // namespace mimus::asr

namespace {

[[noreturn]] void fail(const char *what) {
    std::fprintf(stderr, "FAIL %s\n", what);
    std::exit(1);
}
#define REQUIRE(c) do { if (!(c)) fail(#c); } while (0)

constexpr uint64_t kMiB = 1 << 20;
constexpr uint64_t kJobGpu = 0x48001000, kPdsHeap = 0x48000000;
constexpr uint64_t kProgramGpu = 0x4a002000, kUscHeap = 0x4a000000;
constexpr unsigned kLine = 64;

uint8_t *aligned(uint64_t bytes, uint64_t alignment) {
    void *p = mmap(nullptr, bytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(p != MAP_FAILED);
    return reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(p) + alignment - 1) & ~(alignment - 1));
}

// ---- Memory -------------------------------------------------------------
struct Memory {
    std::vector<int8_t> model[2];
    uint8_t *arena_view, *arena_mem;      // CPU cache view and GPU memory
    uint8_t *out_rw, *out_ro, *out_view;  // GPU memory, CPU mapping, CPU cache
    std::vector<uint8_t> out_valid;       // per line of out_view
    uint8_t *job_view, *job_mem, *program_view, *program_mem;
} m;

void memory_init() {
    for (auto &r : m.model) r.assign(PVR_MIMUS_MODEL_BYTES, 0);
    m.arena_view = aligned(PVR_MIMUS_ARENA_BYTES, 2 * kMiB);
    m.arena_mem = aligned(PVR_MIMUS_ARENA_BYTES, 4096);
    int fd = memfd_create("gpu-mimus-output", 0);
    REQUIRE(fd >= 0 && ftruncate(fd, PVR_MIMUS_OUTPUT_BYTES) == 0);
    m.out_rw = static_cast<uint8_t *>(mmap(nullptr, PVR_MIMUS_OUTPUT_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    void *reserve = mmap(nullptr, 4 * kMiB, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(m.out_rw != MAP_FAILED && reserve != MAP_FAILED);
    void *ro = reinterpret_cast<void *>((reinterpret_cast<uintptr_t>(reserve) + 2 * kMiB - 1) & ~(2 * kMiB - 1));
    m.out_ro = static_cast<uint8_t *>(mmap(ro, PVR_MIMUS_OUTPUT_BYTES, PROT_READ, MAP_SHARED | MAP_FIXED, fd, 0));
    REQUIRE(m.out_ro == ro);
    m.out_view = aligned(PVR_MIMUS_OUTPUT_BYTES, 4096);
    m.out_valid.assign(PVR_MIMUS_OUTPUT_BYTES / kLine, 0);
    m.job_view = aligned(4096, 4096);
    m.job_mem = aligned(4096, 4096);
    m.program_view = aligned(PVR_MIMUS_PROGRAM_BYTES, 4096);
    m.program_mem = aligned(PVR_MIMUS_PROGRAM_BYTES, 4096);
}

// CPU loads of the output region: a line not in the cache fills from memory.
void out_refresh(uint64_t offset, uint64_t bytes) {
    for (uint64_t line = offset / kLine; line < (offset + bytes + kLine - 1) / kLine; ++line)
        if (!m.out_valid[line]) {
            std::memcpy(m.out_view + line * kLine, m.out_ro + line * kLine, kLine);
            m.out_valid[line] = 1;
        }
}

// ---- Event log and cache maintenance model ------------------------------
enum Event { JOIN, DSB, CLEAN_CODES, CLEAN_WORDS, CLEAN_PAGE, CIVAC, KICK, RECORD, POST };
std::vector<int> events;
struct Counters {
    uint64_t clean_lines = 0, civac_lines = 0, dsb = 0, jobs = 0, tags = 0, gpu_macs = 0, lane_macs = 0,
             load_bytes = 0, refused = 0, placed = 0;
} count;
int refuse_cache = -1;          // refuse the n-th helper call from now
bool skip_code_clean = false;   // fault injection: one activation clean has no effect
bool skip_civac = false;        // fault injection: one completion maintenance has no effect
bool verify_sums = true;        // compare every GPU sum with the exact int64 sum

bool inside(uintptr_t a, size_t n, const void *base, uint64_t bytes) {
    uintptr_t b = reinterpret_cast<uintptr_t>(base);
    return a >= b && a - b <= bytes && n <= bytes - (a - b);
}

}  // namespace

extern "C" int aarch64_device_cache(enum aarch64_cache_op op, uintptr_t a, size_t n) {
    if (refuse_cache == 0) { refuse_cache = -1; return 0; }
    if (refuse_cache > 0) --refuse_cache;
    if (!a || !n || (a & 63) || (n & 63)) return 0;
    events.push_back(DSB); ++count.dsb;
    if (inside(a, n, m.arena_view, PVR_MIMUS_ARENA_BYTES)) {
        if (op != AARCH64_CACHE_CLEAN) fail("activation maintenance must be DC CVAC");
        uint64_t off = a - reinterpret_cast<uintptr_t>(m.arena_view);
        if (skip_code_clean) skip_code_clean = false;
        else std::memcpy(m.arena_mem + off, m.arena_view + off, n);
        count.clean_lines += n / kLine;
        events.push_back(CLEAN_CODES);
    } else if (inside(a, n, m.job_view, 4096) || inside(a, n, m.program_view, PVR_MIMUS_PROGRAM_BYTES)) {
        if (op != AARCH64_CACHE_CLEAN) fail("job and program maintenance must be DC CVAC");
        bool job = inside(a, n, m.job_view, 4096);
        uint8_t *view = job ? m.job_view : m.program_view, *mem = job ? m.job_mem : m.program_mem;
        uint64_t off = a - reinterpret_cast<uintptr_t>(view);
        std::memcpy(mem + off, view + off, n);
        bool words = job && off == PVR_MIMUS_WORDS && n == PVR_MIMUS_WORDS_BYTES;
        if (words) count.clean_lines += n / kLine;
        events.push_back(words ? CLEAN_WORDS : CLEAN_PAGE);
    } else if (inside(a, n, m.out_ro, PVR_MIMUS_OUTPUT_BYTES)) {
        // DC IVAC is checked as a write and faults on the read-only mapping;
        // DC CVAC alone leaves stale lines.
        if (op != AARCH64_CACHE_CLEAN_INVALIDATE) fail("output maintenance must be DC CIVAC");
        uint64_t off = a - reinterpret_cast<uintptr_t>(m.out_ro);
        if (skip_civac) skip_civac = false;
        else std::memset(&m.out_valid[off / kLine], 0, n / kLine);
        count.civac_lines += n / kLine;
        events.push_back(CIVAC);
    } else {
        fail("maintenance outside the session ranges");
    }
    events.push_back(DSB); ++count.dsb;
    return 1;
}

// ---- GPU model ----------------------------------------------------------
uint gpu_vtxin[4], gpu_group[2], gpu_local[2];
namespace {
bool gpu_fault;
unsigned gpu_tag_stores, gpu_fenced_stores;

const uint8_t *gpu_map(uint64_t a, unsigned bytes, bool write) {
    struct Range { uint64_t gpu, bytes; uint8_t *host; bool writable; };
    const Range ranges[] = {
        { PVR_MIMUS_MODEL_GPU, PVR_MIMUS_MODEL_BYTES, reinterpret_cast<uint8_t *>(m.model[0].data()), false },
        { PVR_MIMUS_MODEL_GPU + PVR_MIMUS_MODEL_BYTES, PVR_MIMUS_MODEL_BYTES,
          reinterpret_cast<uint8_t *>(m.model[1].data()), false },
        { PVR_MIMUS_ARENA_GPU, PVR_MIMUS_ARENA_BYTES, m.arena_mem, false },
        { PVR_MIMUS_OUTPUT_GPU, PVR_MIMUS_OUTPUT_BYTES, m.out_rw, true },
        { kJobGpu, 4096, m.job_mem, false },
        { kProgramGpu, PVR_MIMUS_PROGRAM_BYTES, m.program_mem, false },
    };
    for (const Range &r : ranges)
        if (a >= r.gpu && a - r.gpu <= r.bytes - bytes) {
            if (write && !r.writable) break;
            return r.host + (a - r.gpu);
        }
    gpu_fault = true;  // the GPU MMU faults; the job ends in RESET
    return nullptr;
}
uint64_t address(uint2 a) { return (uint64_t)a.y << 32 | a.x; }
}  // namespace

extern "C" uint gpu_ld1(uint2 a) {
    uint v = 0;
    const uint8_t *p = (address(a) & 3) ? nullptr : gpu_map(address(a), 4, false);
    if (!p) { gpu_fault = true; return 0; }
    std::memcpy(&v, p, 4);
    count.load_bytes += 4;
    return v;
}
extern "C" uint4 gpu_ld4(uint2 a) {
    uint4 v{};
    const uint8_t *p = (address(a) & 15) ? nullptr : gpu_map(address(a), 16, false);
    if (!p) { gpu_fault = true; return v; }
    std::memcpy(&v, p, 16);
    return v;
}
extern "C" void gpu_st(int fence, uint2 a, const uint *w, unsigned n) {
    uint8_t *p = (address(a) & 3) ? nullptr : const_cast<uint8_t *>(gpu_map(address(a), 4 * n, true));
    if (!p || n != 1) { gpu_fault = true; return; }
    std::memcpy(p, w, 4);
    if (fence) ++gpu_fenced_stores; else ++gpu_tag_stores;
}

namespace {
uint32_t rd32(const uint8_t *p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

enum Outcome { OK, RESET, LOST };
struct Inject { int64_t missing_lane = -1; bool reset = false, lost = false; };

// The runtime's kick: the GPU reads the job page from memory only.
Outcome gpu_job(const Inject &inject) {
    const uint8_t *pds = m.job_mem + PVR_MIMUS_PDS_DATA, *cdm = m.job_mem + PVR_MIMUS_CDM;
    // DOUTW H32/L32 order: the even register receives the low word.
    gpu_vtxin[0] = rd32(pds + 84); gpu_vtxin[1] = rd32(pds + 80);
    gpu_vtxin[2] = rd32(pds + 100); gpu_vtxin[3] = rd32(pds + 96);
    REQUIRE(rd32(cdm + 4) == kJobGpu - kPdsHeap + PVR_MIMUS_PDS_DATA && rd32(cdm + 8) == rd32(cdm + 4) + 128);
    REQUIRE((rd32(pds) | (uint64_t)(rd32(pds + 4) & 7) << 32) == kProgramGpu - kUscHeap);
    REQUIRE(std::memcmp(m.program_mem, pvr_compute_mimus_usc, pvr_compute_mimus_usc_bytes) == 0);
    uint32_t gx = rd32(cdm + 12) + 1, gy = rd32(cdm + 16) + 1;
    REQUIRE(rd32(cdm + 20) == 0 && rd32(cdm + 56) == 2u << 30);
    if (inject.lost) return LOST;
    if (inject.reset) return RESET;
    gpu_fault = false;
    for (uint32_t y = 0; y < gy; ++y)
        for (uint32_t x = 0; x < gx; ++x)
            for (uint32_t li = 0; li < PVR_MIMUS_LANES; ++li) {
                if ((int64_t)((y * gx + x) * PVR_MIMUS_LANES + li) == inject.missing_lane) continue;
                gpu_group[0] = x; gpu_group[1] = y;
                gpu_local[0] = li % 4; gpu_local[1] = li / 4;
                gpu_mimus_lane();
                if (gpu_fault) return RESET;
            }
    return OK;
}

// ---- Tensors and the CPU path ------------------------------------------
struct Tensor {
    unsigned k, n, region, stride;
    uint64_t offset;
    bool residual, has_bias;
    float alpha;
    std::vector<int16_t> packed;  // mimus's packed int16 groups of 8 channels
    std::vector<float> ws, bias;  // padded to whole groups
    pvr_mimus_tensor gpu{};
    unsigned groups() const { return (n + 7) / 8; }
};
std::mt19937 rng(20261022);

int8_t weight_code(int pattern) {
    if (pattern == 1) return -128;
    if (pattern == 2) return 127;
    int r = (int)(rng() % 8);
    return r == 0 ? -128 : r == 1 ? 127 : (int8_t)(rng() % 256 - 128);
}
int16_t act_code(int pattern) {
    if (pattern) return pattern;
    int r = (int)(rng() % 8);
    return r == 0 ? -32767 : r == 1 ? 32767 : (int16_t)((int)(rng() % 65535) - 32767);
}
float scale() {
    float v = std::ldexp((float)(rng() % 1000 + 1), -(int)(rng() % 12 + 10));
    return rng() % 2 ? v : -v;
}

void tensor_init(Tensor &t, int pattern) {
    for (unsigned c = 0; c < t.n; ++c)
        for (unsigned k = 0; k < t.k; ++k)
            m.model[t.region][t.offset + (uint64_t)c * t.stride + k] = weight_code(pattern);
    t.packed.assign((size_t)t.groups() * 8 * t.k, 0);
    for (unsigned c = 0; c < t.n; ++c)
        for (unsigned k = 0; k < t.k; ++k)
            t.packed[mimus::asr::PackedIndex(c, k, 8 * t.k)] = m.model[t.region][t.offset + (uint64_t)c * t.stride + k];
    t.ws.assign(t.groups() * 8, 0.0f);
    t.bias.assign(t.groups() * 8, 0.0f);
    for (unsigned c = 0; c < t.n; ++c) { t.ws[c] = scale(); t.bias[c] = scale(); }
    t.ws[t.n / 2] = -0.0f;
    if (t.has_bias) t.bias[1] = -0.0f;
}

// mimus's CPU path for rows [r0, r0 + rows) and channels [c0, c1).
void cpu_path(const Tensor &t, const std::vector<int16_t> &x, const std::vector<float> &xs, unsigned r0,
              unsigned rows, unsigned c0, unsigned c1, std::vector<float> &y, unsigned y_stride) {
    if (!rows || c0 >= c1) return;
    MimusGemmS16Args a{};
    a.x = x.data() + (size_t)r0 * t.k;
    a.w = t.packed.data() + (size_t)(c0 / 8) * 8 * t.k;
    a.out = y.data() + (size_t)r0 * y_stride + c0;
    a.rows = rows; a.groups = (c1 - c0 + 7) / 8; a.kp = t.k;
    a.x_stride = t.k; a.w_stride = 8 * t.k; a.out_stride = y_stride;
    a.ws = t.ws.data() + c0; a.xs = xs.data() + r0; a.bias = t.has_bias ? t.bias.data() + c0 : nullptr;
    bool fits32 = t.k <= 512;
    if (t.residual) MimusGemmS16Accumulate(&a, t.alpha, fits32);
    else if (fits32) MimusGemmS16F32C(&a);
    else MimusGemmS16FC(&a);
}

// ---- Session, worker and hook models ------------------------------------
pvr_mimus_session session;
uint32_t boot_sequence;      // the recognizer's counter: 1 at the first job of a boot
bool gpu_usable;             // the dispatcher's active count is nonzero
struct Failure { bool failed = false, incomplete = false, cpu_after_failure = false; } failure;

void boot(bool clear_tags) {
    // The start code zeroes the tag area and cleans it before the read-only
    // mapping takes effect; the CPU may then hold any line of the region.
    if (clear_tags) std::memset(m.out_rw, 0, PVR_MIMUS_TAG_BYTES);
    std::fill(m.out_valid.begin(), m.out_valid.end(), 0);
    out_refresh(0, PVR_MIMUS_OUTPUT_BYTES);
    boot_sequence = 0;
    failure = {};
    pvr_mimus_session_config c{};
    c.job_cpu = m.job_view; c.program_cpu = m.program_view;
    c.arena_cpu = reinterpret_cast<uintptr_t>(m.arena_view);
    c.output_cpu = reinterpret_cast<uintptr_t>(m.out_ro);
    c.job_gpu = kJobGpu; c.program_gpu = kProgramGpu; c.pds_heap = kPdsHeap; c.usc_heap = kUscHeap;
    events.clear();
    REQUIRE(pvr_mimus_session_open(&session, &c) == 1);
    REQUIRE(std::memcmp(m.program_mem, pvr_compute_mimus_usc, pvr_compute_mimus_usc_bytes) == 0);
    events.clear();
    count.dsb = 0;
    gpu_usable = true;
}

struct JobInject { int refuse = -1, missing = -1, reset = -1, lost = -1; int64_t lane = 0; };

// One call through the hook: CPU channels first, then GPU jobs of at most
// job_rows rows on 32-row boundaries, each read out after its completion.
void call(Tensor &t, unsigned rows, unsigned n_gpu, unsigned job_rows, uint64_t x_offset,
          const JobInject &inject = {}, int code_pattern = 0, bool expect_match = true) {
    const unsigned y_stride = t.groups() * 8, x_stride = 2 * t.k;
    std::vector<int16_t> x((size_t)rows * t.k);
    for (auto &v : x) v = act_code(code_pattern);
    // Producers (cores 6 and 7) store the codes into the arena through the
    // CPU cache; core 6 then joins them.
    for (unsigned r = 0; r < rows; ++r)
        std::memcpy(m.arena_view + x_offset + (uint64_t)r * x_stride, &x[(size_t)r * t.k], 2 * t.k);
    events.push_back(JOIN);
    std::vector<float> xs(rows), y((size_t)rows * y_stride), ref;
    for (auto &v : xs) v = std::fabs(scale());
    xs[0] = -0.0f;
    for (auto &v : y) v = t.residual ? scale() : NAN;
    if (t.residual) y[0] = -0.0f;
    ref = y;
    cpu_path(t, x, xs, 0, rows, 0, t.n, ref, y_stride);
    if (!gpu_usable) n_gpu = 0;
    // The worker's open-time admission of this tensor for its table row.
    pvr_mimus_tensor placed{};
    REQUIRE(!n_gpu || pvr_mimus_tensor_open(&placed, t.region, t.offset, t.stride, t.k, n_gpu) == 1);
    cpu_path(t, x, xs, 0, rows, n_gpu, t.n, y, y_stride);
    unsigned first = 0;
    for (int index = 0; n_gpu && first < rows; ++index) {
        unsigned rows_now = std::min(job_rows, rows - first);
        REQUIRE(first % 32 == 0 && rows_now <= placed.job_rows);
        pvr_mimus_job job{ x_offset + (uint64_t)first * x_stride, rows_now, x_stride, ++boot_sequence };
        if (index == inject.refuse) refuse_cache = 0;
        size_t mark = events.size();
        bool fallback = false;
        if (!pvr_mimus_submit(&session, &placed, &job)) {
            ++count.refused;  // class 1: no kick; the session stays open
            refuse_cache = -1;
            fallback = true;
        } else {
            events.push_back(KICK);
            Inject gpu;
            if (index == inject.missing) gpu.missing_lane = inject.lane;
            gpu.reset = index == inject.reset;
            gpu.lost = index == inject.lost;
            Outcome outcome = gpu_job(gpu);
            if (outcome != OK) {
                // Class 3: the worker masks the interrupt and keeps every
                // page; the hook records the failure before any CPU row and
                // the boot ends through board_fail and the watchdog.
                pvr_mimus_stop(&session);
                failure.failed = true;
                gpu_usable = false;
                return;
            }
            events.push_back(RECORD);
            bool acquired = pvr_mimus_complete(&session) == 1;
            events.push_back(POST);
            uint32_t tags = pvr_mimus_tags(rows_now, n_gpu);
            uint64_t width = t.k > 512 ? 8 : 4, sums = (uint64_t)rows_now * n_gpu * width;
            out_refresh(PVR_MIMUS_TAG_BYTES - 4 * tags, 4 * tags + sums);
            uintptr_t view = reinterpret_cast<uintptr_t>(m.out_view);
            if (!acquired || !pvr_mimus_tags_complete(view, rows_now, n_gpu, job.sequence)) {
                // Class 2: the worker stops the GPU, acknowledges, and the
                // output region is never read again in this boot.
                pvr_mimus_stop(&session);
                failure.incomplete = true;
                gpu_usable = false;
                fallback = true;
            } else {
                std::vector<int> expect = { DSB, CLEAN_CODES, DSB, DSB, CLEAN_WORDS, DSB, KICK, RECORD, DSB, CIVAC,
                                            DSB, POST };
                REQUIRE(std::vector<int>(events.begin() + mark, events.end()) == expect);
                REQUIRE(index || (mark && events[mark - 1] == JOIN));
                if (verify_sums && expect_match)
                    for (unsigned r = 0; r < rows_now; ++r)
                        for (unsigned c = 0; c < n_gpu; ++c) {
                            int64_t exact = 0, got;
                            for (unsigned k = 0; k < t.k; ++k)
                                exact += (int64_t)x[(size_t)(first + r) * t.k + k] *
                                         m.model[t.region][t.offset + (uint64_t)c * t.stride + k];
                            const uint8_t *at = m.out_view + PVR_MIMUS_TAG_BYTES + ((uint64_t)r * n_gpu + c) * width;
                            if (width == 4) { int32_t v; std::memcpy(&v, at, 4); got = v; }
                            else std::memcpy(&got, at, 8);
                            if (got != exact) {
                                std::fprintf(stderr, "sequence %u row %u channel %u: sum %" PRId64 ", exact %" PRId64 "\n",
                                             job.sequence, first + r, c, got, exact);
                                fail("GPU sum differs from the exact sum");
                            }
                        }
                pvr_mimus_readout r{ const_cast<const uint32_t *>(pvr_mimus_sums(view)), t.ws.data(), xs.data() + first,
                                     t.has_bias ? t.bias.data() : nullptr, y.data() + (size_t)first * y_stride,
                                     rows_now, n_gpu, y_stride, t.alpha };
                if (width == 4) t.residual ? pvr_mimus_readout_i32_accumulate(&r) : pvr_mimus_readout_i32(&r);
                else t.residual ? pvr_mimus_readout_i64_accumulate(&r) : pvr_mimus_readout_i64(&r);
                ++count.jobs; count.tags += tags; count.gpu_macs += (uint64_t)rows_now * t.k * n_gpu;
                count.lane_macs += (uint64_t)tags * 16 * t.k;
            }
        }
        if (fallback) {
            // The CPU computes rows [first, rows) of the GPU channels; the CPU
            // branch already computed (and accumulated) the others.
            if (failure.failed) failure.cpu_after_failure = true;
            cpu_path(t, x, xs, first, rows - first, 0, n_gpu, y, y_stride);
            break;
        }
        first += rows_now;
    }
    if (n_gpu) ++count.placed;
    bool same = true;
    for (unsigned r = 0; r < rows && same; ++r)
        for (unsigned c = 0; c < t.n && same; ++c)
            if (std::memcmp(&y[(size_t)r * y_stride + c], &ref[(size_t)r * y_stride + c], 4)) {
                same = false;
                if (expect_match)
                    std::fprintf(stderr, "K %u N %u n_gpu %u row %u channel %u: %a, CPU path %a\n", t.k, t.n, n_gpu,
                                 r, c, y[(size_t)r * y_stride + c], ref[(size_t)r * y_stride + c]);
            }
    if (same != expect_match) fail(expect_match ? "output differs from the CPU path" : "fault went undetected");
}

unsigned table_rows(const Tensor &t, unsigned n_gpu) {
    return pvr_mimus_job_rows(t.k, n_gpu) / 32 * 32;
}

// ---- Cases ---------------------------------------------------------------
struct Shape { unsigned rows, k, n; bool residual; };
const std::vector<Shape> &bench() {
    static std::vector<Shape> calls;
    if (calls.empty()) {
        auto add = [](unsigned count, Shape s) { for (unsigned i = 0; i < count; ++i) calls.push_back(s); };
        add(19, { 224, 1584, 176, false }); add(1, { 184, 1584, 176, false });
        add(6, { 32, 3520, 176, false }); add(1, { 30, 3520, 176, false });
        add(32, { 222, 176, 704, false }); add(32, { 222, 704, 176, true });
        add(48, { 222, 176, 176, false }); add(32, { 222, 176, 176, true });
        add(16, { 443, 176, 176, false }); add(16, { 222, 176, 352, false });
        add(1, { 222, 176, 1025, false });
    }
    return calls;
}

std::map<std::string, Tensor> tensors;
Tensor &tensor(unsigned k, unsigned n, bool residual) {
    return tensors.at(std::to_string(k) + "x" + std::to_string(n) + (residual ? "r" : ""));
}
void tensors_init() {
    struct Spec { unsigned k, n; bool residual, bias; float alpha; unsigned region; };
    const Spec specs[] = { { 1584, 176, false, true, 0, 0 }, { 3520, 176, false, true, 0, 0 },
                           { 176, 704, false, true, 0, 0 },  { 704, 176, true, true, 0.5f, 0 },
                           { 176, 176, false, false, 0, 0 }, { 176, 176, true, true, 1.0f, 0 },
                           { 176, 352, false, true, 0, 0 },  { 176, 1025, false, true, 0, 1 } };
    uint64_t offset = 0;
    for (const Spec &s : specs) {
        Tensor t{ s.k, s.n, s.region, s.k, 0, s.residual, s.bias, s.alpha, {}, {}, {}, {} };
        // The output head ends exactly at the end of model region 1.
        t.offset = s.region ? PVR_MIMUS_MODEL_BYTES - ((uint64_t)(s.n - 1) * s.k + s.k) : offset;
        if (!s.region) offset += (uint64_t)s.n * s.k + 64;
        tensor_init(t, 0);
        REQUIRE(pvr_mimus_tensor_open(&t.gpu, t.region, t.offset, t.stride, t.k, t.n) == 1);
        tensors[std::to_string(s.k) + "x" + std::to_string(s.n) + (s.residual ? "r" : "")] = t;
    }
}

void case_refusals() {
    boot(true);
    Tensor &t = tensor(704, 176, true);
    auto refused = [&](pvr_mimus_job j, const pvr_mimus_tensor &g) {
        std::vector<uint8_t> before(m.job_view, m.job_view + 4096);
        size_t logged = events.size();
        uint32_t state = session.state, last = session.last_sequence;
        REQUIRE(pvr_mimus_submit(&session, &g, &j) == 0);
        REQUIRE(std::memcmp(before.data(), m.job_view, 4096) == 0 && events.size() == logged);
        REQUIRE(session.state == state && session.last_sequence == last);
    };
    refused({ 0, 0, 2 * t.k, 1 }, t.gpu);                         // rows == 0
    for (auto &[name, u] : tensors)                                // rows = job_rows + 1, every tensor
        refused({ 0, u.gpu.job_rows + 1, 2 * u.k, 1 }, u.gpu);
    refused({ 0, 32, 2 * t.k, 0 }, t.gpu);                        // sequence 0
    refused({ 2, 32, 2 * t.k, 1 }, t.gpu);                        // offset not a multiple of 4
    refused({ 0, 32, 2 * t.k + 2, 1 }, t.gpu);                    // stride not a multiple of 4
    refused({ 0, 32, 2 * t.k - 4, 1 }, t.gpu);                    // stride below 2K
    refused({ 0, 32, 0x10000, 1 }, t.gpu);                        // stride beyond 16 bits
    refused({ PVR_MIMUS_ARENA_BYTES - 31 * 2 * t.k - 2 * t.k + 4, 32, 2 * t.k, 1 }, t.gpu);  // past the arena
    pvr_mimus_job edge{ PVR_MIMUS_ARENA_BYTES - 31 * 2 * t.k - 2 * t.k, 32, 2 * t.k, 1 };
    REQUIRE(pvr_mimus_submit(&session, &t.gpu, &edge) == 1);   // ends exactly at the span's end
    refused({ 0, 32, 2 * t.k, 2 }, t.gpu);                       // a job already in flight
    REQUIRE(pvr_mimus_complete(&session) == 1);
    refused({ 0, 32, 2 * t.k, 1 }, t.gpu);                       // sequence not above the last one
    pvr_mimus_tensor g;
    REQUIRE(!pvr_mimus_tensor_open(&g, 0, 0, 180, 178, 16));     // K not a multiple of 4
    REQUIRE(!pvr_mimus_tensor_open(&g, 0, 0, 172, 176, 16));     // weight stride below K
    REQUIRE(!pvr_mimus_tensor_open(&g, 0, 2, 176, 176, 16));     // weight offset not a multiple of 4
    REQUIRE(!pvr_mimus_tensor_open(&g, 0, 0, 178, 176, 16));     // weight stride not a multiple of 4
    REQUIRE(!pvr_mimus_tensor_open(&g, 2, 0, 176, 176, 16));     // no third model region
    uint64_t end = PVR_MIMUS_MODEL_BYTES - (15 * 176 + 176);
    REQUIRE(pvr_mimus_tensor_open(&g, 0, end, 176, 176, 16));    // ends at the region's end
    REQUIRE(!pvr_mimus_tensor_open(&g, 0, end + 4, 176, 176, 16));  // reaches the other model region
    REQUIRE(!pvr_mimus_tensor_open(&g, 0, 0, 3520, 3520, 1025)); // job bound below 32 rows
    std::puts("case refusals: every refusal precedes any job-word store or maintenance");
}

void case_bench(FILE *calls_out) {
    boot(true);
    Counters start = count;
    uint64_t profile_macs = 0;
    for (const Shape &s : bench()) {
        Tensor &t = tensor(s.k, s.n, s.residual);
        call(t, s.rows, s.n, table_rows(t, s.n), 0);
        profile_macs += (uint64_t)s.rows * s.k * s.n;
        if (calls_out) std::fprintf(calls_out, "%u %u %u %u\n", s.rows, s.k, s.n, s.residual ? 1 : 0);
    }
    REQUIRE(!failure.failed && !failure.incomplete && count.refused == start.refused);
    uint64_t jobs = count.jobs - start.jobs;
    REQUIRE(jobs == bench().size() && count.dsb == 6 * jobs && gpu_tag_stores);
    // The profile counts 4,167,910,560 MACs; the bench call list stays within 0.06%.
    REQUIRE(std::fabs((double)profile_macs / 4167910560.0 - 1) < 0.0006);
    std::printf("bench jobs %" PRIu64 " gpu_macs %" PRIu64 " clean_lines %" PRIu64 " civac_lines %" PRIu64
                " tags %" PRIu64 " dsb_per_job %.0f load_bytes_per_lane_mac %.3f\n",
                jobs, count.gpu_macs - start.gpu_macs, count.clean_lines - start.clean_lines,
                count.civac_lines - start.civac_lines, count.tags - start.tags, (double)count.dsb / jobs,
                (double)(count.load_bytes - start.load_bytes) / (count.lane_macs - start.lane_macs));
}

void case_refusal_and_failures() {
    // A refused job inside the bench sequence: the CPU computes its rows.
    boot(true);
    uint64_t refused = count.refused;
    unsigned i = 0;
    for (const Shape &s : bench()) {
        if (++i % 23) continue;
        Tensor &t = tensor(s.k, s.n, s.residual);
        JobInject inject; if (i == 46) inject.refuse = 0;
        call(t, s.rows, s.n, table_rows(t, s.n), 0, inject);
    }
    REQUIRE(count.refused == refused + 1 && gpu_usable);
    // RESET and LOST: the failure callback runs before any CPU row.
    for (int which = 0; which < 2; ++which) {
        boot(true);
        Tensor &t = tensor(176, 704, false);
        JobInject inject; (which ? inject.lost : inject.reset) = 0;
        call(t, 222, 704, table_rows(t, 704), 0, inject, 0, false);
        REQUIRE(failure.failed && !failure.cpu_after_failure && session.state == PVR_MIMUS_STOPPED);
    }
    std::puts("case refusal and failures: refusal computed on the CPU; RESET and LOST stop before any CPU row");
}

void case_residual_split() {
    // A residual layer with n_gpu < N in a three-job call (64, 64, 32 rows).
    Tensor &t = tensor(704, 176, true);
    boot(true);
    JobInject refuse; refuse.refuse = 1;
    call(t, 160, 112, 64, 4096, refuse);
    REQUIRE(gpu_usable && session.state == PVR_MIMUS_IDLE);
    call(t, 160, 112, 64, 4096);  // the session stays open after a refusal
    // The same call with one missing tag in the middle job: stop, then CPU.
    boot(true);
    JobInject missing; missing.missing = 1; missing.lane = 37;
    call(t, 160, 112, 64, 4096, missing);
    REQUIRE(failure.incomplete && !gpu_usable && session.state == PVR_MIMUS_STOPPED);
    call(t, 160, 112, 64, 4096);  // every later call runs on the CPU
    std::puts("case residual split: refused and incomplete middle jobs match the CPU path");
}

void case_stale_tags() {
    Tensor &t = tensor(176, 352, false);
    // An earlier boot left tags equal to this boot's first sequences.
    for (int clear = 1; clear >= 0; --clear) {
        uint32_t *tags = reinterpret_cast<uint32_t *>(m.out_rw);
        for (unsigned i = 0; i < PVR_MIMUS_TAG_BYTES / 4; ++i) tags[i] = 1;
        boot(clear);
        JobInject missing; missing.missing = 0; missing.lane = 5;
        // Without the boot clear the stale tag hides the missing lane, and
        // its rows keep the earlier boot's sums.
        call(t, 222, 352, table_rows(t, 352), 0, missing, 0, clear == 1);
        REQUIRE(failure.incomplete == (clear == 1));
    }
    std::puts("case stale tags: the boot clear exposes a missing lane behind an earlier boot's tags");
}

void case_maintenance_faults() {
    Tensor &t = tensor(176, 176, false);
    boot(true);
    call(t, 222, 176, table_rows(t, 176), 0);
    skip_code_clean = true;  // the GPU reads the previous call's codes behind matching tags
    call(t, 222, 176, table_rows(t, 176), 0, {}, 0, false);
    REQUIRE(!failure.incomplete);
    boot(true);
    call(t, 222, 176, table_rows(t, 176), 0);
    skip_civac = true;       // the CPU reads the previous job's tags and sums
    call(t, 222, 176, table_rows(t, 176), 0);
    REQUIRE(failure.incomplete);
    std::puts("case maintenance faults: an omitted clean or clean-and-invalidate is detected by the model");
}

void case_negative_chunks() {
    for (unsigned k : { 1584u, 3520u }) {
        Tensor t{ k, 32, 1, k, 0, false, true, 0, {}, {}, {}, {} };
        for (int pattern = 1; pattern <= 2; ++pattern) {
            tensor_init(t, pattern);  // every weight -128, or 127
            REQUIRE(pvr_mimus_tensor_open(&t.gpu, 1, 0, k, k, 32) == 1);
            boot(true);
            call(t, 64, 32, table_rows(t, 32), 0, {}, 32767);
            call(t, 64, 32, table_rows(t, 32), 0, {}, -32767);
            call(t, 64, 32, table_rows(t, 32), 0, {}, 0);
        }
    }
    std::puts("case negative chunks: K = 1,584 and 3,520 with every chunk sum at its negative and positive limit");
}

void case_stray_store() {
    pid_t child = fork();
    REQUIRE(child >= 0);
    if (!child) {
        std::signal(SIGSEGV, SIG_DFL);  // no sanitizer report: the default action ends the child
        *reinterpret_cast<volatile uint32_t *>(m.out_ro + 4) = 1;
        _exit(0);
    }
    int status = 0;
    REQUIRE(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    std::puts("case stray store: a CPU store into the output region faults");
}

}  // namespace

int main(int argc, char **argv) {
    bool quick = argc > 1 && std::strcmp(argv[1], "--quick") == 0;
    FILE *calls_out = argc > 2 ? std::fopen(argv[2], "w") : nullptr;
    memory_init();
    tensors_init();
    case_refusals();
    case_stray_store();
    case_residual_split();
    case_stale_tags();
    case_maintenance_faults();
    case_negative_chunks();
    case_refusal_and_failures();
    verify_sums = false;  // the bench compares the read-out with mimus's CPU path only
    if (!quick) case_bench(calls_out);
    if (calls_out) std::fclose(calls_out);
    std::puts("PASS exact GPU sums and read-out equal mimus's CPU path in every case");
    return 0;
}
