/* Bounded exact-sum jobs using the production session and linked USC code.
 * Included by fw-job.inc; the surrounding caller owns firmware and cleanup. */
#include "pvr_compute_mimus.h"
#define MIMUS_ROWS 33u
#define MIMUS_CHANNELS 17u
#define MIMUS_BUFFER_BYTES (128u * 1024u)
#define MIMUS_MAP_PAGES (4u + 32u + 32u + 1u + 2u)
static unsigned char mimus_weights[16384] __attribute__((aligned(4096)));
static unsigned char mimus_arena[MIMUS_BUFFER_BYTES] __attribute__((aligned(2097152)));
static unsigned char mimus_output[MIMUS_BUFFER_BYTES] __attribute__((aligned(2097152)));
static unsigned char mimus_job_page[4096] __attribute__((aligned(4096)));
static unsigned char mimus_program[8192] __attribute__((aligned(4096)));
static struct pvr_mimus_session mimus_session;
static unsigned char mimus_command[96];
static uint32_t mimus_update[2];
static unsigned mimus_verified, mimus_bad_tags, mimus_bad_sums, mimus_bad_guards;
static int64_t mimus_minimum, mimus_maximum;

static int mimus_maps(unsigned *count)
{
    const struct { void *cpu; size_t bytes; uint64_t gpu; unsigned flags; } spans[] = {
        {mimus_weights, sizeof(mimus_weights), PVR_MIMUS_MODEL_GPU, PVR_GPU_VM_READ_ONLY},
        {mimus_arena, sizeof(mimus_arena), PVR_MIMUS_ARENA_GPU, PVR_GPU_VM_READ_ONLY},
        {mimus_output, sizeof(mimus_output), PVR_MIMUS_OUTPUT_GPU, PVR_GPU_VM_SLC_BYPASS},
        {mimus_job_page, sizeof(mimus_job_page), job_heap[0] + 0x1e0000, PVR_GPU_VM_READ_ONLY},
        {mimus_program, sizeof(mimus_program), job_heap[1] + 0x100000, PVR_GPU_VM_READ_ONLY}
    };
    for (unsigned i = 0; i < sizeof(spans) / sizeof(spans[0]); ++i) {
        if (!a733_gpu_identity_mapped((uintptr_t)spans[i].cpu, spans[i].bytes, A733_MAIR_NORMAL_WB, 1)) return 0;
        for (size_t at = 0; at < spans[i].bytes; at += 4096) {
            if (*count == JOB_MAP_PAGES) return 0;
            job_maps[(*count)++] = (struct pvr_gpu_mapping){spans[i].gpu + at,
                (uintptr_t)spans[i].cpu + at, 4096, 12, spans[i].flags};
        }
    }
    const struct pvr_mimus_session_config c = {
        .job_cpu = mimus_job_page, .program_cpu = mimus_program,
        .arena_cpu = (uintptr_t)mimus_arena, .output_cpu = (uintptr_t)mimus_output,
        .job_gpu = job_heap[0] + 0x1e0000, .program_gpu = job_heap[1] + 0x100000,
        .pds_heap = job_heap[0], .usc_heap = job_heap[1],
        .arena_bytes = sizeof(mimus_arena), .output_bytes = sizeof(mimus_output)
    };
    return pvr_mimus_session_open(&mimus_session, &c);
}

/* The fixed caller uses CPU-writable output while idle. It publishes guards
 * and stale tags before each kick, then never writes output until completion.
 * The OS's read-only output mapping is a separate qualification boundary. */
static int mimus_inputs(unsigned k, unsigned sequence)
{
    int8_t *weights = (int8_t *)mimus_weights;
    int16_t *codes = (int16_t *)mimus_arena;
    for (unsigned n = 0; n < MIMUS_CHANNELS; ++n)
        for (unsigned i = 0; i < k; ++i) weights[n * k + i] = n & 1 ? 127 : -128;
    for (unsigned r = 0; r < MIMUS_ROWS; ++r)
        for (unsigned i = 0; i < k; ++i) codes[r * k + i] = r & 1 ? -32767 : 32767;
    uint32_t *out = (uint32_t *)mimus_output;
    for (unsigned i = 0; i < sizeof(mimus_output) / 4; ++i) out[i] = PVR_COMPUTE_JOB_GUARD;
    const unsigned tags = pvr_mimus_tags(MIMUS_ROWS, MIMUS_CHANNELS);
    for (unsigned i = 0; i < tags; ++i) out[PVR_MIMUS_TAG_BYTES / 4 - tags + i] = sequence - 1;
    if (pvr_mimus_tags_complete((uintptr_t)mimus_output, MIMUS_ROWS, MIMUS_CHANNELS, sequence)) return 0;
    return aarch64_device_cache(AARCH64_CACHE_CLEAN, (uintptr_t)mimus_weights, sizeof(mimus_weights)) &&
        aarch64_device_cache(AARCH64_CACHE_CLEAN_INVALIDATE, (uintptr_t)mimus_output, sizeof(mimus_output));
}

static int mimus_verify(unsigned k, unsigned sequence)
{
    if (!pvr_mimus_complete(&mimus_session)) return 0;
    mimus_bad_tags += !pvr_mimus_tags_complete((uintptr_t)mimus_output, MIMUS_ROWS, MIMUS_CHANNELS, sequence);
    const unsigned width = k > 512 ? 2 : 1;
    const volatile uint32_t *sums = pvr_mimus_sums((uintptr_t)mimus_output);
    const int8_t *weights = (const int8_t *)mimus_weights;
    const int16_t *codes = (const int16_t *)mimus_arena;
    for (unsigned r = 0; r < MIMUS_ROWS; ++r) {
        for (unsigned n = 0; n < MIMUS_CHANNELS; ++n) {
            int64_t expected = 0;
            for (unsigned i = 0; i < k; ++i) expected += (int64_t)codes[r * k + i] * weights[n * k + i];
            const unsigned at = (r * MIMUS_CHANNELS + n) * width;
            int64_t actual = (int32_t)sums[at];
            if (width == 2) actual = (int64_t)(int32_t)sums[at + 1] * INT64_C(4294967296) + sums[at];
            mimus_bad_sums += actual != expected;
            if (expected < mimus_minimum) mimus_minimum = expected;
            if (expected > mimus_maximum) mimus_maximum = expected;
            ++mimus_verified;
        }
    }
    if (!aarch64_device_cache(AARCH64_CACHE_INVALIDATE, (uintptr_t)mimus_output, sizeof(mimus_output))) return 0;
    const volatile uint32_t *out = (const volatile uint32_t *)mimus_output;
    const unsigned first = PVR_MIMUS_TAG_BYTES / 4 - pvr_mimus_tags(MIMUS_ROWS, MIMUS_CHANNELS);
    const unsigned end = PVR_MIMUS_TAG_BYTES / 4 + MIMUS_ROWS * MIMUS_CHANNELS * width;
    for (unsigned i = 0; i < sizeof(mimus_output) / 4; ++i)
        if (i < first || i >= end) mimus_bad_guards += out[i] != PVR_COMPUTE_JOB_GUARD;
    return !mimus_bad_tags && !mimus_bad_sums && !mimus_bad_guards;
}
