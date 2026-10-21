#ifndef SUNSTONE_PVR_COMPUTE_SERVICE_H
#define SUNSTONE_PVR_COMPUTE_SERVICE_H
#include "pvr_compute_job.h"
#include <stdatomic.h>
#include "pvr_mips.h"
#include "a733_gpu_vm_memory.h"

struct pvr_gpu_service;
struct pvr_gpu_service_config;
#define PVR_COMPUTE_SERVICE_OBJECTS 6u
#define PVR_COMPUTE_SERVICE_MAPS 128u
#define PVR_COMPUTE_SERVICE_TABLE_PAGES 256u
/* Six firmware-only objects: context, register state, CCCB control/ring, VM
 * record, timeline. Each owns whole uncached CPU pages. Their MIPS addresses
 * are page aligned and distinct from workload GPU virtual addresses. Data
 * buffers own separate page-rounded write-back allocations; buffers[UFO]
 * is exactly firmware[5]. Page lists and every input remain immutable/pinned.
 *
 * A trusted offline producer has already built job/program/context bytes and
 * PREPARED vm tables through a733_gpu_vm_memory_init/build. Its immutable,
 * naturally aligned physical_pages list retains that adapter contract. maps
 * describes that exact table build. Only 4 KiB pages
 * are admitted, exactly covering programs and non-UFO job allocations without
 * extra aliases. Firmware objects are never mapped into the workload VM.
 * Native translation checks establish CPU/PA correspondence, not reservation
 * ownership or GPU visibility. job.admit must also prove the table/map binding,
 * physical exclusions of all other platform users and heap bases. The offline
 * producer selects the MC1 address layout. Physical core-mode admission occurs
 * after power enable and before computation through service.compute_mode. */
struct pvr_compute_service_config {
    const struct pvr_compute_job_config *job;
    const struct pvr_compute_job_image *image;
    struct a733_gpu_vm_memory *vm;
    const struct pvr_gpu_mapping *maps;
    unsigned map_count;
    struct pvr_mips_buffer firmware[PVR_COMPUTE_SERVICE_OBJECTS];
    uint32_t firmware_address[PVR_COMPUTE_SERVICE_OBJECTS];
    struct pvr_mips_buffer programs[PVR_PROGRAM_REGIONS], buffers[PVR_JOB_REGIONS];
};
enum pvr_compute_service_phase { PVR_COMPUTE_SERVICE_NEW, PVR_COMPUTE_SERVICE_BOUND,
    PVR_COMPUTE_SERVICE_DATA, PVR_COMPUTE_SERVICE_TABLES, PVR_COMPUTE_SERVICE_READY,
    PVR_COMPUTE_SERVICE_FORMAT, PVR_COMPUTE_SERVICE_RUNNING,
    PVR_COMPUTE_SERVICE_COMPLETE, PVR_COMPUTE_SERVICE_FAILED,
    PVR_COMPUTE_SERVICE_CLEANUP, PVR_COMPUTE_SERVICE_CLEANED };
enum pvr_compute_service_error { PVR_COMPUTE_SERVICE_NO_ERROR,
    PVR_COMPUTE_SERVICE_MAPPING, PVR_COMPUTE_SERVICE_CLOCK,
    PVR_COMPUTE_SERVICE_ADMISSION, PVR_COMPUTE_SERVICE_PUBLICATION,
    PVR_COMPUTE_SERVICE_QUEUE, PVR_COMPUTE_SERVICE_JOB,
    PVR_COMPUTE_SERVICE_RESET, PVR_COMPUTE_SERVICE_LIFECYCLE, PVR_COMPUTE_SERVICE_CANCELLED,
    PVR_COMPUTE_SERVICE_CLEANUP_ERROR };
struct pvr_compute_service {
    const struct pvr_compute_service_config *config;
    struct pvr_gpu_service *service;
    struct pvr_cccb client;
    struct pvr_compute_job job;
    struct pvr_mips_object objects[PVR_COMPUTE_SERVICE_OBJECTS];
    uint64_t deadline, last_now, epoch;
    uint32_t phase, error, buffer, page;
    struct pvr_ccb_token cleanup_token;
    uint32_t cleanup_context, cleanup_status;
    /* One copied request, never GPU mapped. Ticket/client bind its queue lifetime.
     * 0=empty, 1=caller copying, 2=worker-owned queued bytes. */
    atomic_uint input_state;
    uint64_t input_ticket;
    uint32_t input_client, input_bytes;
    unsigned char input_copy[PVR_COMPUTE_PACKAGE_INPUT];
};
/* Before scheduler startup and firmware publication, at EL1 with enabled MMU
 * and cache. Bind to a zero/uninitialized service. Refusal preserves outputs.
 * Validates complete CPU/PA spans and aliases against service firmware/resources,
 * VM backing, private state, descriptors and page lists. Caller supplies the
 * remaining reservation proof through job.admit. No buffer writes,
 * cache maintenance or GPU access. Success sets config.compute. */
int pvr_compute_service_rebind(struct pvr_compute_service *,struct pvr_gpu_service *,
    struct pvr_gpu_service_config *,const struct pvr_compute_service_config *);
int pvr_compute_service_bind(struct pvr_compute_service *, struct pvr_gpu_service *,
    struct pvr_gpu_service_config *, const struct pvr_compute_service_config *);
/* Internal privileged service-worker interface. Objects join the initial MIPS
 * table build; no live firmware-table update occurs. OPEN publication cleans
 * one data page or one VM table page per poll. READY requires complete admission
 * and publication. begin admits the fixed workload repeatedly after acquired completion, retaining
 * the client ring and advancing its timeline without wrap. No reinitialization
 * or resource release is supported. */
/* Post-power admission before the first firmware register write. Rechecked at
 * COMPUTE; no observation survives a failed or replaced lifecycle. */
int pvr_compute_service_admit_mode(struct pvr_compute_service *);
int pvr_compute_service_prepare(struct pvr_compute_service *, uint64_t deadline);
int pvr_compute_service_begin(struct pvr_compute_service *, uint64_t deadline);
/* Worker-only replacement: 0 refuses without writes, -1 retains a fault,
 * 1 accepts after acquired completion and EL1 input publication. */
int pvr_compute_service_replace(struct pvr_compute_service *, uint32_t sequence,
    const void *, size_t bytes, uint64_t deadline);
int pvr_compute_service_poll(struct pvr_compute_service *);
/* Compute-only retained CLOSE. Admission/previous completion are rechecked
 * before the fixed FWCOMMONCONTEXT request. Poll after servicing control/FWCCB;
 * only exact status1 marks CLEANED. No mapping, buffer or lease is released. */
int pvr_compute_service_cleanup_begin(struct pvr_compute_service *,uint64_t);
int pvr_compute_service_cleanup_poll(struct pvr_compute_service *);
/* No I/O: poison retained state when service/control/reset fails. */
void pvr_compute_service_abort(struct pvr_compute_service *, unsigned error);
/* Returns 1 for unrelated events, 0 for malformed/reset events. Any firmware
 * reset faults this single-context qualification session, even after completion.
 * The service forwards unrelated events to their original handlers. */
int pvr_compute_service_event(struct pvr_compute_service *, const unsigned char[88]);
#endif
