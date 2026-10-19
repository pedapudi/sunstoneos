#ifndef SUNSTONE_PVR_COMPUTE_JOB_H
#define SUNSTONE_PVR_COMPUTE_JOB_H
#include "pvr_compute_programs.h"
#include "pvr_cccb.h"
#include "pvr_compute_package.h"

#define PVR_COMPUTE_JOB_WORDS 32u
#define PVR_COMPUTE_JOB_USC_BYTES 280u
#define PVR_COMPUTE_JOB_USC_TEMPS 6u
#define PVR_COMPUTE_JOB_GUARD UINT32_C(0x6d35a7c1)

enum pvr_compute_job_region { PVR_JOB_USC, PVR_JOB_PDS, PVR_JOB_FENCE_PDS,
    PVR_JOB_CDM, PVR_JOB_INPUT, PVR_JOB_OUTPUT, PVR_JOB_UFO, PVR_JOB_BORDER,
    PVR_JOB_REGIONS };
/* USC 320, dispatch PDS 128, fence PDS 64, CDM 4096, input 64, output 256,
 * UFO 64, unused border table 262144. All GPU addresses are 64-byte aligned;
 * CDM additionally begins on a 4096-byte page, including its fetch guard.
 * Output contains 64-byte guards before and after 128 bytes of results. */
extern const uint32_t pvr_compute_job_bytes[PVR_JOB_REGIONS];
struct pvr_compute_job_config {
    const struct pvr_compute_program_config *programs;
    const struct pvr_compute_program_result *program_result;
    struct pvr_compute_program_span buffers[PVR_JOB_REGIONS];
    uint32_t firmware_context, firmware_ufo, seed, factor, job_id;
    /* Optional authenticated, immutable trusted compiler product and input. */
    const struct pvr_compute_package *package;
    void *admission_context;
    /* Pure bounded privileged check: same proven single-core lease as context
     * programs, private fresh queue/UFO, immutable valid context/VM/programs,
     * all physical/mapping/cache permissions, heap bases, and complete alias
     * exclusions. A UFO is a firmware-owned 32-bit timeline word. Its first
     * command writes sequence 1. Completed/acquired jobs can advance that
     * timeline through repeat; sequence wrap and resource disposal are refused. */
    int (*admit)(void *, const struct pvr_compute_job_config *);
};
size_t pvr_compute_job_region_bytes(const struct pvr_compute_job_config *, unsigned);
unsigned pvr_compute_job_result_words(const struct pvr_compute_job_config *);
struct pvr_compute_job_image {
    unsigned char compute[96], update[8];
    uint32_t expected[PVR_COMPUTE_JOB_WORDS];
};
/* Offline atomic construction. Optional package mode copies trusted compiler
 * output/input and emits the same two-pointer launch with metadata-derived
 * resources. Default NULL package retains the fixed numerical program. Only the selected A733/single-core profile is
 * supported. Uses the pinned 32-instance arithmetic shader; no sampler access.
 * Initializes all eight private allocations, emits two CDM kernels (work and
 * PDS fence), terminates the stream, and encodes CDM then timeline UPDATE.
 * Refusal preserves all output bytes. Typed records require natural alignment;
 * byte spans may be unaligned on the CPU. The caller owns and stabilizes every
 * input, callback context and output, including nested records; its callback
 * context must be disjoint from every output. Existing context resources are
 * checked for logical conflicts but never changed. No publication or KICK. */
int pvr_compute_job_build(const struct pvr_compute_job_config *,
                          struct pvr_compute_job_image *);

enum pvr_compute_job_phase { PVR_JOB_NEW, PVR_JOB_CHECK_FENCE, PVR_JOB_PUBLISH,
    PVR_JOB_SUBMIT, PVR_JOB_KICK, PVR_JOB_WAIT_FENCE, PVR_JOB_READ_OUTPUT,
    PVR_JOB_COMPLETE, PVR_JOB_FAILED };
enum pvr_compute_job_error { PVR_JOB_NO_ERROR, PVR_JOB_CLOCK, PVR_JOB_TIMEOUT,
    PVR_JOB_IO, PVR_JOB_TRANSPORT, PVR_JOB_FENCE_VALUE, PVR_JOB_RESULT };
struct pvr_compute_job_ops {
    int (*now)(void *, uint64_t *);
    /* Publish all context/VM/program/job allocations and establish GPU-visible
     * translations/permissions before first KICK. This is not a separate
     * firmware attachment command. Failure may have partially published. */
    int (*publish)(void *, const struct pvr_compute_job_config *, uint64_t deadline);
    /* Acquire device writes with uncached mappings or safe invalidation; fence
     * is one aligned word transaction. Output covers the entire guarded region
     * (256 fixed bytes, or 4096 bytes with a compiled package).
     * Return exactly 1 on success. All callbacks are bounded and non-reentrant. */
    int (*read_fence)(void *, uint32_t *);
    int (*read_output)(void *, void *destination, size_t bytes);
};
struct pvr_compute_job {
    struct pvr_compute_job_ops ops;
    void *context;
    struct pvr_cccb *transport;
    const struct pvr_compute_job_config *config;
    const struct pvr_compute_job_image *image;
    uint64_t deadline, last_now, epoch;
    uint32_t phase, error, sequence;
    uint64_t serial;
    unsigned char update[8];
    uint32_t results[PVR_COMPUTE_PACKAGE_OUTPUT / 4];
};
/* No I/O. Initialize zeroed private state against an IDLE transport and ACTIVE
 * firmware queue, sharing one monotonic clock and one privileged serialized
 * worker. The clock uses microseconds. The queue/context must never have
 * submitted a job. Keep config,
 * image and all backing immutable/pinned through this operation and cleanup.
 * External admission already established pvr_compute_job_build success.
 * Root transport processing/events must continue between polls. */
int pvr_compute_job_init(struct pvr_compute_job *,
                         const struct pvr_compute_job_config *,
                         const struct pvr_compute_job_image *,
                         const struct pvr_compute_job_ops *, void *,
                         struct pvr_cccb *, uint64_t absolute_deadline);
/* One bounded step. Return 0 pending, 1 numerical job complete, -1 retained
 * fault. KICK acceptance only advances to fence wait. Completion requires
 * the current sequence plus coherent output/guard agreement. Publication precedes
 * the previous-sequence check, including cache maintenance for output. It does not clean up the
 * firmware context or establish isolation. No resource-release API exists;
 * every fault retains backing until independently proven stop and cleanup. */
/* Worker-only continuation after acquired COMPLETE. Preserve live client ring,
 * epoch and all immutable inputs. Refuse timeline/serial wrap. publish must
 * verify the old timeline before poisoning/publishing output for this sequence.
 * Refusal preserves state; acceptance invalidates the prior copied result. */
int pvr_compute_job_can_repeat(const struct pvr_compute_job *, uint64_t absolute_deadline);
int pvr_compute_job_repeat(struct pvr_compute_job *, uint64_t absolute_deadline);
int pvr_compute_job_poll(struct pvr_compute_job *);
int pvr_compute_job_abort(struct pvr_compute_job *);
#endif
