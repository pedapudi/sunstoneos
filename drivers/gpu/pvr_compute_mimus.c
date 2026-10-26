/* Exact mimus integer matrix products on the GPU: admission, job bytes,
 * cache maintenance and the completion proof (docs/gpu-mimus.md). */
#include "pvr_compute_mimus.h"
#include "pvr_compute_job.h"
#include "aarch64_device_memory.h"
#include "a733_range.h"
#include <string.h>

#define LINE 64u
#define SPAN_ALIGN (UINT64_C(2) << 20)

static void put32(unsigned char *p, uint32_t v)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i)); }
static void put64(unsigned char *p, uint64_t v)
{ put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

uint32_t pvr_mimus_job_rows(uint32_t k, uint32_t n)
{
    if (!k || !n || k > 0xffff || n > 0xffff) return 0;
    uint64_t width = k > 512 ? 8 : 4;
    uint64_t rows = PVR_MIMUS_MAX_MACS / ((uint64_t)k * n);
    uint64_t sums = (PVR_MIMUS_OUTPUT_BYTES - PVR_MIMUS_TAG_BYTES) / (n * width);
    uint64_t groups = PVR_MIMUS_GROUP_ROWS *
        (PVR_MIMUS_WORKGROUPS / ((n + PVR_MIMUS_GROUP_CHANNELS - 1) / PVR_MIMUS_GROUP_CHANNELS));
    if (sums < rows) rows = sums;
    if (groups < rows) rows = groups;
    return rows < PVR_MIMUS_GROUP_ROWS ? 0 : (uint32_t)rows;
}

static int heap_offset(uint64_t address, uint64_t heap, uint64_t bytes, uint64_t limit)
{ return address >= heap && address - heap <= limit - bytes; }

int pvr_mimus_session_open(struct pvr_mimus_session *s, const struct pvr_mimus_session_config *c)
{
    uint32_t code = pvr_compute_mimus_usc_bytes, temps = pvr_compute_mimus_usc_temps;
    if (!a733_span_excl_nonempty(c, sizeof(*c))) return 0;
    const size_t arena = c->arena_bytes ? c->arena_bytes : PVR_MIMUS_ARENA_BYTES;
    const size_t output = c->output_bytes ? c->output_bytes : PVR_MIMUS_OUTPUT_BYTES;
    if (!a733_span_excl_nonempty(s, sizeof(*s)) ||
        !a733_span_excl_nonempty(c->job_cpu, PVR_MIMUS_JOB_PAGE_BYTES) ||
        !a733_span_excl_nonempty(c->program_cpu, PVR_MIMUS_PROGRAM_BYTES) ||
        ((uintptr_t)c->job_cpu & 4095) || ((uintptr_t)c->program_cpu & 4095) ||
        !c->arena_cpu || !c->output_cpu || (c->arena_cpu & (SPAN_ALIGN - 1)) ||
        (c->output_cpu & (SPAN_ALIGN - 1)) ||
        !arena || arena > PVR_MIMUS_ARENA_BYTES || (arena & 4095) ||
        output <= PVR_MIMUS_TAG_BYTES || output > PVR_MIMUS_OUTPUT_BYTES || (output & 4095) ||
        c->arena_cpu > UINTPTR_MAX - arena || c->output_cpu > UINTPTR_MAX - output ||
        !a733_ranges_disjoint_nonempty(c->job_cpu, PVR_MIMUS_JOB_PAGE_BYTES,
                                       c->program_cpu, PVR_MIMUS_PROGRAM_BYTES) ||
        !a733_ranges_disjoint_nonempty((const void *)c->arena_cpu, arena,
                                       (const void *)c->output_cpu, output))
        return 0;
    const void *pages[2] = { c->job_cpu, c->program_cpu };
    const size_t sizes[2] = { PVR_MIMUS_JOB_PAGE_BYTES, PVR_MIMUS_PROGRAM_BYTES };
    for (unsigned i = 0; i < 2; ++i)
        if (!a733_ranges_disjoint_nonempty(pages[i], sizes[i], (const void *)c->arena_cpu,
                                           arena) ||
            !a733_ranges_disjoint_nonempty(pages[i], sizes[i], (const void *)c->output_cpu,
                                           output) ||
            !a733_ranges_disjoint_nonempty(pages[i], sizes[i], s, sizeof(*s)) ||
            !a733_ranges_disjoint_nonempty(pages[i], sizes[i], c, sizeof(*c)))
            return 0;
    /* PDS offsets are 32-bit and the DOUTU code offset 35-bit. Neither page
     * may alias a session range that the GPU reads or writes. */
    if ((c->job_gpu & 4095) || (c->program_gpu & 4095) ||
        !heap_offset(c->job_gpu, c->pds_heap, PVR_MIMUS_JOB_PAGE_BYTES, UINT64_C(1) << 32) ||
        !heap_offset(c->program_gpu, c->usc_heap, PVR_MIMUS_PROGRAM_BYTES, UINT64_C(1) << 35) ||
        !code || (code & 3) || code > PVR_MIMUS_PROGRAM_BYTES - LINE || (temps + 3) / 4 > 63 ||
        c->last_sequence == UINT32_MAX)
        return 0;
    const uint64_t gpu[3][2] = { { PVR_MIMUS_MODEL_GPU, PVR_MIMUS_MODELS * PVR_MIMUS_MODEL_BYTES },
        { PVR_MIMUS_ARENA_GPU, PVR_MIMUS_ARENA_BYTES }, { PVR_MIMUS_OUTPUT_GPU, PVR_MIMUS_OUTPUT_BYTES } };
    for (unsigned i = 0; i < 3; ++i)
        if (!a733_extents_disjoint_checked(c->job_gpu, PVR_MIMUS_JOB_PAGE_BYTES, gpu[i][0], gpu[i][1]) ||
            !a733_extents_disjoint_checked(c->program_gpu, PVR_MIMUS_PROGRAM_BYTES, gpu[i][0], gpu[i][1]))
            return 0;
    if (!a733_extents_disjoint_checked(c->job_gpu, PVR_MIMUS_JOB_PAGE_BYTES,
                                       c->program_gpu, PVR_MIMUS_PROGRAM_BYTES))
        return 0;
    /* One copy of the program per session; the code stays immutable. */
    memset(c->program_cpu, 0, PVR_MIMUS_PROGRAM_BYTES);
    memcpy(c->program_cpu, pvr_compute_mimus_usc, code);
    memset(c->job_cpu, 0, PVR_MIMUS_JOB_PAGE_BYTES);
    /* The program reads no workgroup count, so the counts the PDS writes into
     * coefficients stay zero; the CDM stream carries each job's grid. */
    const uint32_t unused[3] = { 0, 0, 0 };
    pvr_compute_job_grid_pds(c->job_cpu + PVR_MIMUS_PDS_DATA,
        (c->program_gpu - c->usc_heap) | ((uint64_t)((temps + 3) / 4) << 35), unused,
        c->job_gpu + PVR_MIMUS_WORDS, PVR_MIMUS_OUTPUT_GPU);
    pvr_compute_job_fence_pds(c->job_cpu + PVR_MIMUS_FENCE);
    struct pvr_mimus_session value = { .c = *c, .state = PVR_MIMUS_IDLE,
                                       .last_sequence = c->last_sequence };
    value.c.arena_bytes = arena;
    value.c.output_bytes = output;
    if (!aarch64_device_cache(AARCH64_CACHE_CLEAN, (uintptr_t)c->program_cpu, PVR_MIMUS_PROGRAM_BYTES) ||
        !aarch64_device_cache(AARCH64_CACHE_CLEAN, (uintptr_t)c->job_cpu, PVR_MIMUS_JOB_PAGE_BYTES))
        return 0;
    *s = value;
    return 1;
}

void pvr_mimus_session_command(const struct pvr_mimus_session *s, unsigned char compute[96],
                               uint64_t border, uint64_t resume)
{ pvr_compute_job_command(compute, border, s->c.job_gpu + PVR_MIMUS_CDM, resume); }

int pvr_mimus_tensor_open(struct pvr_mimus_tensor *t, unsigned region, uint64_t offset,
                          uint32_t stride, uint32_t k, uint32_t n_gpu)
{
    uint32_t job_rows = pvr_mimus_job_rows(k, n_gpu);
    if (!t || region >= PVR_MIMUS_MODELS || !job_rows || (k & 3) || ((offset | stride) & 3) ||
        stride < k || stride > 0xffff)
        return 0;
    /* The two model regions are adjacent in GPU addresses, so an extent past
     * this region would read the other model. */
    uint64_t extent = (uint64_t)(n_gpu - 1) * stride + k;
    if (!a733_extent_within(0, PVR_MIMUS_MODEL_BYTES, offset, extent)) return 0;
    *t = (struct pvr_mimus_tensor){ PVR_MIMUS_MODEL_GPU + region * PVR_MIMUS_MODEL_BYTES + offset,
                                    k, n_gpu, stride, job_rows };
    return 1;
}

int pvr_mimus_submit(struct pvr_mimus_session *s, const struct pvr_mimus_tensor *t,
                     const struct pvr_mimus_job *j)
{
    /* At most 12 compares: the session state, the rows, the sequence, the
     * alignment and stride of the codes and their extent in the arena span.
     * Sequences rise through the boot, so every earlier tag differs. */
    uint32_t rows = j->rows, k = t->k, stride = j->x_stride;
    if (s->state != PVR_MIMUS_IDLE || !rows || rows > t->job_rows ||
        j->sequence <= s->last_sequence || ((j->x_offset | stride) & 3) ||
        stride < 2 * k || stride > 0xffff)
        return 0;
    uint64_t extent = (uint64_t)(rows - 1) * stride + 2 * k;
    const uint64_t sums = (uint64_t)rows * t->n_gpu * (k > 512 ? 8u : 4u);
    if (!a733_extent_within(0, s->c.arena_bytes, j->x_offset, extent) ||
        sums > s->c.output_bytes - PVR_MIMUS_TAG_BYTES) return 0;
    unsigned char *words = s->c.job_cpu + PVR_MIMUS_WORDS;
    put64(words, t->weights);
    put64(words + 8, PVR_MIMUS_ARENA_GPU + j->x_offset);
    put32(words + 16, k | t->n_gpu << 16);
    put32(words + 20, rows);
    put32(words + 24, t->stride | stride << 16);
    put32(words + 28, j->sequence);
    const uint32_t groups[3] = { (t->n_gpu + PVR_MIMUS_GROUP_CHANNELS - 1) / PVR_MIMUS_GROUP_CHANNELS,
                                 (rows + PVR_MIMUS_GROUP_ROWS - 1) / PVR_MIMUS_GROUP_ROWS, 1 };
    uint32_t pds = (uint32_t)(s->c.job_gpu - s->c.pds_heap);
    pvr_compute_job_grid_cdm(words + PVR_MIMUS_ARGS_BYTES, pds + PVR_MIMUS_PDS_DATA,
                             pds + PVR_MIMUS_FENCE, groups);
    /* The arena span is 2 MiB aligned, so the rounded codes stay inside it.
     * The helper's leading DSB SY orders this clean after the ParallelFor
     * join that made the other core's code stores visible. */
    uint64_t first = j->x_offset & ~(uint64_t)(LINE - 1);
    uint64_t end = (j->x_offset + extent + LINE - 1) & ~(uint64_t)(LINE - 1);
    if (!aarch64_device_cache(AARCH64_CACHE_CLEAN, s->c.arena_cpu + (uintptr_t)first,
                              (size_t)(end - first)) ||
        !aarch64_device_cache(AARCH64_CACHE_CLEAN, (uintptr_t)words, PVR_MIMUS_WORDS_BYTES))
        return 0;
    s->last_sequence = j->sequence;
    s->rows = rows;
    s->n_gpu = t->n_gpu;
    s->wide = k > 512;
    s->state = PVR_MIMUS_IN_FLIGHT;
    return 1;
}

int pvr_mimus_complete(struct pvr_mimus_session *s)
{
    if (s->state != PVR_MIMUS_IN_FLIGHT) return 0;
    /* Tags sit directly below the sums, so one call covers both. The tag
     * count is a multiple of 32, so the start is 128-byte aligned. DC CIVAC
     * needs read permission only; the output region has no CPU writer. */
    uint64_t tags = (uint64_t)pvr_mimus_tags(s->rows, s->n_gpu) * 4;
    uint64_t sums = (uint64_t)s->rows * s->n_gpu * (s->wide ? 8 : 4);
    uint64_t bytes = (tags + sums + LINE - 1) & ~(uint64_t)(LINE - 1);
    if (!aarch64_device_cache(AARCH64_CACHE_CLEAN_INVALIDATE,
                              s->c.output_cpu + PVR_MIMUS_TAG_BYTES - (uintptr_t)tags, (size_t)bytes)) {
        s->state = PVR_MIMUS_STOPPED;
        return 0;
    }
    s->state = PVR_MIMUS_IDLE;
    return 1;
}

void pvr_mimus_stop(struct pvr_mimus_session *s)
{ s->state = PVR_MIMUS_STOPPED; }
