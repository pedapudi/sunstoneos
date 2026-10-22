#ifndef SUNSTONE_PVR_COMPUTE_MIMUS_H
#define SUNSTONE_PVR_COMPUTE_MIMUS_H
/* Exact mimus integer matrix products on the GPU (docs/gpu-mimus.md).
 *
 * One session per boot runs shaders/pvr_mimus_linear.cl over int8 weight
 * codes in two model regions and int16 activation codes in the arena span,
 * all read in place, and writes one job's tags and sums into the output
 * region. The GPU runtime owns the session's leases, GPU mappings, firmware
 * context, client ring, kick, completion record and interrupt; this module
 * owns the program and job bytes, admission, cache maintenance and the
 * completion proof. It makes no translation, hash or allocation, and no I/O
 * other than the stores to the job and program pages and the cache
 * maintenance it names. Every function except the inline ones runs at EL1 in
 * the GPU worker task, which serializes them. */
#include <stddef.h>
#include <stdint.h>

/* GPU address layout. Each model region holds one model file; the arena span
 * holds mimus's arena with the activation codes; the output region holds 64 KiB
 * of tag words and then the sums. */
#define PVR_MIMUS_MODEL_GPU UINT64_C(0x43000000)
#define PVR_MIMUS_MODEL_BYTES UINT64_C(0x00800000)
#define PVR_MIMUS_MODELS 2u
#define PVR_MIMUS_ARENA_GPU UINT64_C(0x46000000)
#define PVR_MIMUS_ARENA_BYTES UINT64_C(0x00c00000)
#define PVR_MIMUS_OUTPUT_GPU UINT64_C(0x46c00000)
#define PVR_MIMUS_OUTPUT_BYTES UINT64_C(0x00200000)
#define PVR_MIMUS_TAG_BYTES 65536u
/* One job: at most 2^26 multiply-accumulates and 512 workgroups of 16
 * channels by 32 rows, whose 32 tag words each fill the tag area. */
#define PVR_MIMUS_MAX_MACS (UINT64_C(1) << 26)
#define PVR_MIMUS_WORKGROUPS 512u
#define PVR_MIMUS_LANES 32u
#define PVR_MIMUS_GROUP_ROWS 32u
#define PVR_MIMUS_GROUP_CHANNELS 16u
/* Program pages hold the USC code; the job page holds, from byte 0, the PDS
 * data, the PDS program, the fence program and the 128-byte job-word area:
 * the 32-byte argument record, then the 60-byte CDM stream. */
#define PVR_MIMUS_PROGRAM_BYTES 8192u
#define PVR_MIMUS_JOB_PAGE_BYTES 4096u
#define PVR_MIMUS_PDS_DATA 0u
#define PVR_MIMUS_FENCE 192u
#define PVR_MIMUS_WORDS 256u
#define PVR_MIMUS_WORDS_BYTES 128u
#define PVR_MIMUS_ARGS_BYTES 32u
#define PVR_MIMUS_CDM (PVR_MIMUS_WORDS + PVR_MIMUS_ARGS_BYTES)
#define PVR_MIMUS_CDM_BYTES 60u

/* The linked program (drivers/gpu/pvr_compute_mimus_usc.c): USC code without
 * the compiler's 8-byte header, its length and its temporary registers. */
extern const uint32_t pvr_compute_mimus_usc[];
extern const uint32_t pvr_compute_mimus_usc_bytes, pvr_compute_mimus_usc_temps;

struct pvr_mimus_session_config {
    /* EL1 write-back mappings: the 4 KiB-aligned job page and the 8 KiB of
     * program pages. The GPU maps both read-only. */
    unsigned char *job_cpu, *program_cpu;
    /* CPU addresses of the arena span and the output region, 2 MiB aligned.
     * The output region is read-only at EL1; only DC CIVAC touches it. */
    uintptr_t arena_cpu, output_cpu;
    /* GPU addresses of the job page, inside the context's PDS heap, and of
     * the program pages, inside its USC heap; and both heap bases. */
    uint64_t job_gpu, program_gpu, pds_heap, usc_heap;
    /* Highest sequence any job of this boot used; 0 at the first open. */
    uint32_t last_sequence;
};
/* One tensor, admitted at open. weights is the GPU address of channel 0. */
struct pvr_mimus_tensor {
    uint64_t weights;
    uint32_t k, n_gpu, stride, job_rows;
};
/* One job: rows activation rows from x_offset in the arena span, x_stride
 * bytes apart, and the recognizer's sequence for this job. */
struct pvr_mimus_job {
    uint64_t x_offset;
    uint32_t rows, x_stride, sequence;
};
enum pvr_mimus_state { PVR_MIMUS_CLOSED, PVR_MIMUS_IDLE, PVR_MIMUS_IN_FLIGHT,
    PVR_MIMUS_STOPPED };
struct pvr_mimus_session {
    struct pvr_mimus_session_config c;
    uint32_t state, last_sequence;
    /* The job in flight: its output extent for the completion maintenance. */
    uint32_t rows, n_gpu, wide;
};

/* The largest job of a tensor: the MAC cap, the sum area and the tag area;
 * 0 when it is below 32 rows and the tensor runs on the CPU. */
uint32_t pvr_mimus_job_rows(uint32_t k, uint32_t n_gpu);
/* Session open, before any submit. Copies the linked program into the
 * program pages, writes the PDS data, PDS program and fence program into the
 * job page and cleans both. Returns 1, or 0 with nothing written when the
 * configuration is malformed. */
int pvr_mimus_session_open(struct pvr_mimus_session *, const struct pvr_mimus_session_config *);
/* The 96-byte firmware compute command of every job of the session, into a
 * buffer the caller has zeroed. */
void pvr_mimus_session_command(const struct pvr_mimus_session *, unsigned char compute[96],
                               uint64_t border, uint64_t resume);
/* Admission at open: K a multiple of 4 below 2^16, n_gpu below 2^16, the
 * weight offset and stride multiples of 4 with K <= stride < 2^16, and every
 * weight byte inside the named model region. Returns 1 and fills the tensor,
 * or 0 when the tensor runs on the CPU. */
int pvr_mimus_tensor_open(struct pvr_mimus_tensor *, unsigned region, uint64_t offset,
                          uint32_t stride, uint32_t k, uint32_t n_gpu);
/* Admission at submit, the job words and the two cleans before the kick.
 * Returns 1 when the runtime may kick; 0 refuses before any device access,
 * and the hook computes the job's rows on the CPU (a class-1 refusal). */
int pvr_mimus_submit(struct pvr_mimus_session *, const struct pvr_mimus_tensor *,
                     const struct pvr_mimus_job *);
/* After the worker read an OK completion record: cleans and invalidates the
 * job's tags and sums, behind the helper's leading DSB SY. Returns 1 when the
 * recognizer may read them; 0 stops the session, and the job counts as
 * incomplete. */
int pvr_mimus_complete(struct pvr_mimus_session *);
/* RESET, LOST or a tag mismatch: no further submit in this boot. Leaves the
 * output region and every page as they are. */
void pvr_mimus_stop(struct pvr_mimus_session *);

/* Recognizer, after the completion, at any EL: the job's tag count, its first
 * tag word and its sums, and whether every lane stored this job's sequence. */
static inline uint32_t pvr_mimus_tags(uint32_t rows, uint32_t n_gpu)
{
    return (n_gpu + PVR_MIMUS_GROUP_CHANNELS - 1) / PVR_MIMUS_GROUP_CHANNELS *
        ((rows + PVR_MIMUS_GROUP_ROWS - 1) / PVR_MIMUS_GROUP_ROWS) * PVR_MIMUS_LANES;
}
static inline const volatile uint32_t *pvr_mimus_sums(uintptr_t output_cpu)
{ return (const volatile uint32_t *)(output_cpu + PVR_MIMUS_TAG_BYTES); }
static inline int pvr_mimus_tags_complete(uintptr_t output_cpu, uint32_t rows, uint32_t n_gpu,
                                          uint32_t sequence)
{
    uint32_t tags = pvr_mimus_tags(rows, n_gpu), bad = 0;
    if (!tags || tags > PVR_MIMUS_TAG_BYTES / 4) return 0;
    const volatile uint32_t *tag = pvr_mimus_sums(output_cpu) - tags;
    for (uint32_t i = 0; i < tags; ++i) bad |= tag[i] ^ sequence;
    return !bad;
}
#endif
