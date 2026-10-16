#ifndef SUNSTONE_PVR_COMPUTE_PROGRAMS_H
#define SUNSTONE_PVR_COMPUTE_PROGRAMS_H
#include <stddef.h>
#include <stdint.h>
#include "pvr_firmware.h"

#define PVR_COMPUTE_USC_STORE_BYTES 544u
#define PVR_COMPUTE_USC_LOAD_BYTES 488u
#define PVR_COMPUTE_USC_TEMPS 10u
#define PVR_COMPUTE_SHARED_LOGICAL_BYTES 16432u
#define PVR_COMPUTE_PROGRAM_EXCLUSIONS 16u

enum pvr_compute_program_region {
    PVR_PROGRAM_USC_STORE, PVR_PROGRAM_USC_LOAD,
    PVR_PROGRAM_PDS_STORE_A, PVR_PROGRAM_PDS_LOAD_A,
    PVR_PROGRAM_PDS_STORE_B, PVR_PROGRAM_PDS_LOAD_B,
    PVR_PROGRAM_PDS_TERMINATE, PVR_PROGRAM_SHARED_A,
    PVR_PROGRAM_SHARED_B, PVR_PROGRAM_CDM_RESUME, PVR_PROGRAM_REGIONS
};
/* Exact allocation sizes, in enum order: 576, 576, 128, 128, 128, 128,
 * 64, 16448, 16448, 512. Every GPU address is 64-byte aligned. CPU byte
 * views may be unaligned. USC allocations include at least 32 prefetch bytes. */
extern const uint32_t pvr_compute_program_bytes[PVR_PROGRAM_REGIONS];
struct pvr_compute_program_span { void *cpu; size_t bytes; uint64_t gpu; };
struct pvr_compute_program_heap { uint64_t base, bytes; };
struct pvr_compute_program_config {
    uint64_t bvnc;
    struct pvr_compute_program_heap pds, usc, general;
    struct pvr_compute_program_span buffers[PVR_PROGRAM_REGIONS];
    /* First five entries: compute context, firmware register state, CCCB
     * control, CCCB ring, VM memory record, using their firmware virtual addresses.
     * These five addresses belong to the MIPS heap, not the workload GPU VM.
     * Append every other known conflicting allocation. These are exclusions,
     * not proof of mapping or of physical alias freedom. */
    struct pvr_compute_program_span excluded[PVR_COMPUTE_PROGRAM_EXCLUSIONS];
    uint32_t excluded_count;
    uint32_t execution_cores;
    uint64_t admission_token;
    void *admission_context;
    /* Required pure, bounded privileged check. Return 1 only while the token
     * proves the selected MC1 construction profile, private ownership,
     * mappings, permissions, and all backing/reservation exclusions. Retain
     * that proof and every buffer until independent cleanup authorizes reuse.
     * Physical core-count admission is a separate post-power requirement. */
    int (*admit)(void *, const struct pvr_compute_program_config *);
};
struct pvr_compute_program_result {
    /* Order accepted by pvr_compute_context_build. */
    uint64_t switch_registers[7];
    uint64_t cdm_resume_address;
};
/* Offline, bounded construction for BVNC 36.56.104.183 only. This embeds the
 * pinned compiler's two common USC programs and emits five PDS programs.
 * Zero complete owned allocations, including shared state and padding, then
 * write program bytes and result. Refusal preserves all outputs. Input records,
 * output result, and owned CPU/GPU spans must be disjoint.
 * Input and result records require their natural C alignment.
 * The admission callback/context must remain stable and disjoint from outputs;
 * it must not mutate inputs, publish buffers, or claim an unproven core mode.
 *
 * Requires an unattached private context and the MC1 construction profile.
 * Execution requires native post-power core-count/capability admission.
 * Multicore addressing is unimplemented, despite the profile's multicore
 * capability. This is not a reduction of required hardware coverage.
 * Publishing/cache maintenance, heap register programming, firmware attachment,
 * submission, completion, and block/drain isolation are separate prerequisites.
 * No allocation, MMIO, scheduling, general shader compilation, or KICK occurs. */
int pvr_compute_programs_build(const struct pvr_compute_program_config *,
                              struct pvr_compute_program_result *);
#endif
