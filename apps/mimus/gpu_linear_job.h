#ifndef SUNSTONE_MIMUS_GPU_LINEAR_JOB_H
#define SUNSTONE_MIMUS_GPU_LINEAR_JOB_H
/* The job that mimus passes to the GPU compute queue for one linear-layer
 * slice (docs/designs/gpu-compute-queue.md, "mimus as an ordinary client").
 *
 * Status: prototype. Nothing calls this yet.
 *
 * The kernel is shaders/pvr_mimus_linear.cl, unchanged. It reads a 32-byte
 * record through its first pointer argument (unified registers 0-1): the GPU
 * addresses of the weights and the activation codes, then four shape words.
 * Its second pointer argument (unified registers 2-3) is the output region:
 * 64 KiB of tag words, then the sums. Under the queue's argument rule the
 * output is binding 0, the weights and codes are bindings 1 and 2, whose
 * addresses the queue writes as the record's first 16 bytes, and the shape
 * words are the job's 16 argument bytes. */
#include "gpu_queue.h"

#define MIMUS_GPU_TAG_BYTES 65536u
#define MIMUS_GPU_LANES 32u
#define MIMUS_GPU_GROUP_ROWS 32u
#define MIMUS_GPU_GROUP_CHANNELS 16u
#define MIMUS_GPU_LINE 64u

enum { MIMUS_GPU_OUTPUT, MIMUS_GPU_WEIGHTS, MIMUS_GPU_CODES, MIMUS_GPU_BINDINGS };

/* The job's argument bytes, as the kernel reads them from byte 16 of its
 * record. */
struct mimus_gpu_shape {
    uint32_t k_n;        /* K in bits 0-15, n_gpu in bits 16-31 */
    uint32_t rows;
    uint32_t strides;    /* weight row stride in bits 0-15, code row stride in bits 16-31 */
    uint32_t sequence;   /* the tag value every lane stores */
};
_Static_assert(sizeof(struct mimus_gpu_shape) == 16, "kernel shape words");
_Static_assert((MIMUS_GPU_BINDINGS - 1) * 8 + sizeof(struct mimus_gpu_shape) == 32,
               "kernel argument record");

/* One tensor admitted by mimus at start: the byte offset of weight row 0 in
 * the model lease, K, the GPU channel count, the weight row stride and the
 * largest job's rows. */
struct mimus_gpu_tensor { uint32_t weight_offset, k, n_gpu, stride, job_rows; };

/* The lease indices that gpu_queue_lease returned for the output region,
 * the model files and the arena, and the program index that
 * gpu_queue_program_load returned. */
struct mimus_gpu_handles { uint16_t output, models, arena, program; };

static inline uint32_t mimus_gpu_tags(uint32_t rows, uint32_t n_gpu)
{
    return (n_gpu + MIMUS_GPU_GROUP_CHANNELS - 1) / MIMUS_GPU_GROUP_CHANNELS *
        ((rows + MIMUS_GPU_GROUP_ROWS - 1) / MIMUS_GPU_GROUP_ROWS) * MIMUS_GPU_LANES;
}

/* Fill one job. The caller has already checked the call against the
 * tensor (rows <= job_rows, code stride and alignment, the extent inside the
 * arena), as the hook does today; the queue checks every range again
 * against its lease. Maintenance covers what the session code maintains
 * today: the job's code extent before the kick, and its tags and sums after
 * the timeline update. The model files were published at boot and have no
 * CPU writer, so the weight binding has no maintenance range. */
static inline void mimus_gpu_linear_job(struct gpu_queue_job *job, const struct mimus_gpu_handles *l,
                                        const struct mimus_gpu_tensor *t, uint32_t x_offset,
                                        uint32_t rows, uint32_t x_stride, uint32_t sequence)
{
    const uint32_t width = t->k > 512 ? 8u : 4u;
    const uint32_t tags = mimus_gpu_tags(rows, t->n_gpu) * 4u;
    const uint32_t sums = rows * t->n_gpu * width;
    const uint32_t extent = (rows - 1u) * x_stride + 2u * t->k;
    const uint32_t first = x_offset & ~(MIMUS_GPU_LINE - 1u);
    const uint32_t end = (x_offset + extent + MIMUS_GPU_LINE - 1u) & ~(MIMUS_GPU_LINE - 1u);
    *job = (struct gpu_queue_job){
        .sequence = sequence, .program = l->program, .bindings = MIMUS_GPU_BINDINGS,
        .argument_bytes = sizeof(struct mimus_gpu_shape),
        .groups = {(t->n_gpu + MIMUS_GPU_GROUP_CHANNELS - 1) / MIMUS_GPU_GROUP_CHANNELS,
                   (rows + MIMUS_GPU_GROUP_ROWS - 1) / MIMUS_GPU_GROUP_ROWS, 1},
        .binding = {
            [MIMUS_GPU_OUTPUT] = {l->output, GPU_QUEUE_GPU_READ_WRITE, 0, 0,
                                  MIMUS_GPU_TAG_BYTES - tags, tags + sums},
            [MIMUS_GPU_WEIGHTS] = {l->models, GPU_QUEUE_GPU_READ, 0, t->weight_offset, 0, 0},
            [MIMUS_GPU_CODES] = {l->arena, GPU_QUEUE_GPU_READ, 0, x_offset, first, end - first},
        }};
    const struct mimus_gpu_shape shape = {t->k | t->n_gpu << 16, rows, t->stride | x_stride << 16, sequence};
    __builtin_memcpy(job->arguments, &shape, sizeof(shape));
}
#endif
