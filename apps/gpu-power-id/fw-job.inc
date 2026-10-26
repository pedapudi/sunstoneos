/* SPDX-License-Identifier: MIT
 * The first GPU job of the firmware-job variant (fw-start.c built with
 * FWS_JOB), docs/designs/gpu-first-job.md: the fixed numerical compute job,
 * one 32-lane workgroup, composed in the image's own memory as
 * platform/a733/gpu_compute_reserved.c composes it for the GPU service.
 *
 * Memory, all inside the image's reservation and each page checked against
 * its translation before use:
 * - the six firmware objects (compute context, register state, client CCB
 *   control and ring, VM record, timeline) take the six Normal-NC pages after
 *   the firmware's shared resources; the MIPS page table maps them at
 *   0xc0800000 + n * 0x10000, so the firmware reaches only them;
 * - the context programs and the job's programs, control stream, input,
 *   output and border table take 88 write-back pages of image RAM, mapped
 *   once each in the workload GPU VM; programs and inputs are GPU read-only,
 *   and the output page bypasses the GPU's system-level cache so that its
 *   writes reach DRAM;
 * - the workload VM's page tables take write-back image RAM.
 * The job's own GPU addresses lie in the PDS, USC and general heaps that
 * system init names (0xda00000000, 0xe000000000, 0x8000000000). */
#include "pvr_compute_job.h"
#include "pvr_compute_context.h"
#include "a733_gpu_vm_memory.h"
#include "a733_gpu_mapping.h"
#include "aarch64_device_memory.h"

#define JOB_PAYLOAD_PAGES 88u
#define JOB_TABLE_PAGES 32u
#define JOB_OBJECTS 6u
#define JOB_RING_BYTES 1024u
#define JOB_SEED UINT32_C(0x12345678)
#define JOB_FACTOR UINT32_C(0x9e3779b1)
static const uint32_t job_object_bytes[JOB_OBJECTS] = {232, 4, 32, JOB_RING_BYTES, 32, 64};
static const uint64_t job_heap[3] = {UINT64_C(0xda00000000), UINT64_C(0xe000000000), UINT64_C(0x8000000000)};
static unsigned char job_payload[JOB_PAYLOAD_PAGES * 4096u] __attribute__((aligned(4096)));
static unsigned char job_table_memory[JOB_TABLE_PAGES * 4096u] __attribute__((aligned(4096)));
static uint64_t job_payload_pages[JOB_PAYLOAD_PAGES], job_table_pages[JOB_TABLE_PAGES];
static uint64_t job_object_pages[JOB_OBJECTS];
static struct pvr_mips_buffer job_objects[JOB_OBJECTS];
static uint32_t job_object_address[JOB_OBJECTS];
static struct pvr_mips_buffer job_programs_backing[PVR_PROGRAM_REGIONS], job_backing[PVR_JOB_REGIONS];
static struct pvr_gpu_mapping job_maps[JOB_PAYLOAD_PAGES];
static struct pvr_gpu_vm_requirements job_requirements;
static struct a733_gpu_vm_memory job_vm;
static struct pvr_compute_program_config job_program_config;
static struct pvr_compute_program_result job_program_result;
static struct pvr_compute_job_config job_config;
static struct pvr_compute_job_image job_image;
static struct pvr_cccb job_client;
static struct pvr_compute_job job;
static int job_admitted;
/* The cleanup command's return word, and its slot. */
static uint32_t job_cleanup_status = UINT32_MAX;
static struct pvr_ccb_token job_cleanup_token;
static uint32_t job_output_words[4];

static uint64_t job_address(unsigned program, unsigned i)
{
    if (program) return job_heap[i < 2 ? 1 : i < 9 ? 0 : 2] + 0x10000 + i * 0x50000u;
    return job_heap[i == 0 ? 1 : i < 3 ? 0 : 2] + 0x1000000 + i * 0x50000u;
}
/* The builders' admission: only this image's own configuration, while the
 * job is the image's and nothing has failed. */
static int job_admit_programs(void *context, const struct pvr_compute_program_config *c)
{ return context == &job_admitted && c == &job_program_config && job_admitted; }
static int job_admit(void *context, const struct pvr_compute_job_config *c)
{ return context == &job_admitted && c == &job_config && job_admitted; }

/* Appends the six firmware objects to objects[from..], which pvr_mips_prepare
 * then maps. nc is the first free Normal-NC page. Returns 1 when every
 * allocation is placed, translated as required and built. */
static int fws_job_prepare(uintptr_t nc, struct pvr_mips_object *objects)
{
    for (unsigned i = 0; i < JOB_OBJECTS; ++i) {
        job_object_pages[i] = nc + i * 4096u;
        job_objects[i] = (struct pvr_mips_buffer){{(void *)(nc + i * 4096u), 4096}, &job_object_pages[i], 1};
        job_object_address[i] = 0xc0800000u + i * 0x10000u;
        if (!a733_gpu_identity_mapped(nc + i * 4096u, 4096, A733_MAIR_NORMAL_NC, 1)) return 0;
        objects[i] = (struct pvr_mips_object){&job_object_pages[i], 1, job_object_address[i], PVR_MIPS_UNCACHED};
    }
    if (!a733_gpu_identity_mapped((uintptr_t)job_payload, sizeof(job_payload), A733_MAIR_NORMAL_WB, 1) ||
        !a733_gpu_identity_mapped((uintptr_t)job_table_memory, sizeof(job_table_memory), A733_MAIR_NORMAL_WB, 1))
        return 0;
    job_admitted = 1;
    struct pvr_compute_program_config *p = &job_program_config;
    *p = (struct pvr_compute_program_config){.bvnc = PVR_FW_A733_BVNC,
        .pds = {job_heap[0], UINT64_C(1) << 32}, .usc = {job_heap[1], UINT64_C(1) << 32},
        .general = {job_heap[2], UINT64_C(0x2000000000)}, .excluded_count = 5, .execution_cores = 1,
        .admission_token = 1, .admission_context = &job_admitted, .admit = job_admit_programs};
    struct pvr_compute_buffer contexts[PVR_COMPUTE_REGIONS];
    for (unsigned i = 0; i < 5; ++i) {
        p->excluded[i] = (struct pvr_compute_program_span){job_objects[i].storage.cpu, job_object_bytes[i],
                                                           job_object_address[i]};
        contexts[i] = (struct pvr_compute_buffer){job_objects[i].storage.cpu, job_object_bytes[i],
                                                  job_object_address[i]};
    }
    size_t offset = 0;
    unsigned count = 0;
    for (unsigned i = 0; i < PVR_PROGRAM_REGIONS + PVR_JOB_REGIONS; ++i) {
        const int program = i < PVR_PROGRAM_REGIONS;
        const unsigned j = program ? i : i - PVR_PROGRAM_REGIONS;
        if (!program && j == PVR_JOB_UFO) continue;
        const size_t n = program ? pvr_compute_program_bytes[j] : pvr_compute_job_region_bytes(&job_config, j);
        const size_t rounded = (n + 4095) & ~(size_t)4095;
        if (offset + rounded > sizeof(job_payload)) return 0;
        struct pvr_mips_buffer *d = program ? &job_programs_backing[j] : &job_backing[j];
        uint64_t *pages = job_payload_pages + offset / 4096;
        for (unsigned k = 0; k < rounded / 4096; ++k) pages[k] = (uintptr_t)job_payload + offset + k * 4096u;
        *d = (struct pvr_mips_buffer){{job_payload + offset, rounded}, pages, (uint32_t)(rounded / 4096)};
        const uint64_t va = job_address(program, j);
        const struct pvr_compute_program_span span = {d->storage.cpu, n, va};
        if (program) p->buffers[j] = span; else job_config.buffers[j] = span;
        const unsigned flags = (program ? j < PVR_PROGRAM_SHARED_A : j != PVR_JOB_OUTPUT) ? PVR_GPU_VM_READ_ONLY :
            !program && j == PVR_JOB_OUTPUT ? PVR_GPU_VM_SLC_BYPASS : 0;
        for (unsigned k = 0; k < d->page_count; ++k) {
            if (count == JOB_PAYLOAD_PAGES) return 0;
            job_maps[count++] = (struct pvr_gpu_mapping){va + k * 4096u, d->pages[k], 4096, 12, flags};
        }
        offset += rounded;
    }
    if (offset != sizeof(job_payload) || count != JOB_PAYLOAD_PAGES) return 0;
    job_config.buffers[PVR_JOB_UFO] = (struct pvr_compute_program_span){job_objects[5].storage.cpu, 64,
                                                                       job_object_address[5]};
    for (unsigned i = 1; i < count; ++i) {
        struct pvr_gpu_mapping m = job_maps[i];
        unsigned j = i;
        while (j && job_maps[j - 1].address > m.address) { job_maps[j] = job_maps[j - 1]; --j; }
        job_maps[j] = m;
    }
    if (!pvr_gpu_vm_require(job_maps, count, &job_requirements) || !job_requirements.table_pages ||
        job_requirements.table_pages > JOB_TABLE_PAGES) return 0;
    for (unsigned i = 0; i < job_requirements.table_pages; ++i)
        job_table_pages[i] = (uintptr_t)job_table_memory + i * 4096u;
    const struct pvr_gpu_vm_tables tables = {job_table_memory, job_requirements.table_bytes, job_table_pages,
                                             job_requirements.table_pages};
    if (!a733_gpu_vm_memory_init(&job_vm, &tables, contexts[PVR_COMPUTE_VM].cpu, job_object_pages[4]) ||
        a733_gpu_vm_memory_build(&job_vm, job_maps, count) != A733_GPU_VM_MEMORY_OK ||
        !pvr_compute_programs_build(p, &job_program_result)) return 0;
    struct pvr_compute_config context = {.bvnc = PVR_FW_A733_BVNC, .priority = 1, .context_id = 1, .owner_id = 1};
    memcpy(context.switch_registers, job_program_result.switch_registers, sizeof(context.switch_registers));
    if (!pvr_compute_context_build(&context, contexts)) return 0;
    job_config.programs = p; job_config.program_result = &job_program_result;
    job_config.firmware_context = job_object_address[0]; job_config.firmware_ufo = job_object_address[5];
    job_config.seed = JOB_SEED; job_config.factor = JOB_FACTOR; job_config.job_id = 1;
    job_config.admission_context = &job_admitted; job_config.admit = job_admit;
    return pvr_compute_job_build(&job_config, &job_image);
}

/* Client CCB callbacks: the control word in its NC page, commands in the
 * ring page. */
static int job_read(void *context, enum pvr_cccb_region r, uint32_t offset, void *out, size_t n)
{
    (void)context;
    if (r != PVR_CCCB_CONTROL || n != 4 || (offset & 3) || offset + n > job_object_bytes[2]) return 0;
    const uint32_t value = a733_mmio_read32_dsb_sy_after((uintptr_t)job_objects[2].storage.cpu + offset);
    memcpy(out, &value, 4);
    return 1;
}
static int job_write(void *context, enum pvr_cccb_region r, uint32_t offset, const void *in, size_t n)
{
    (void)context;
    const unsigned id = r == PVR_CCCB_CONTROL ? 2 : 3;
    if ((unsigned)r > PVR_CCCB_COMMANDS || !n || offset > job_object_bytes[id] || n > job_object_bytes[id] - offset)
        return 0;
    __asm__ volatile("dsb sy" ::: "memory");
    memcpy((unsigned char *)job_objects[id].storage.cpu + offset, in, n);
    __asm__ volatile("dsb sy" ::: "memory");
    return 1;
}
static int job_barrier(void *context, enum pvr_ccb_barrier kind)
{
    (void)context;
    if ((unsigned)kind > PVR_CCB_DEVICE_COMPLETE) return 0;
    __asm__ volatile("dsb sy" ::: "memory");
    return 1;
}
static int job_read_fence(void *context, uint32_t *out)
{
    (void)context;
    *out = a733_mmio_read32_dsb_sy_after((uintptr_t)job_objects[5].storage.cpu);
    return 1;
}
/* As the service does: the previous timeline value, then the output's
 * guards and the complement of each expected word, cleaned to the point of
 * coherency before the KICK. */
static int job_publish(void *context, const struct pvr_compute_job_config *c, uint64_t deadline)
{
    (void)context; (void)deadline;
    uint32_t previous;
    if (c != &job_config || job_vm.phase != A733_GPU_VM_MEMORY_PUBLISHED || !job_read_fence(NULL, &previous) ||
        previous != job.sequence - 1) return 0;
    uint32_t *words = job_backing[PVR_JOB_OUTPUT].storage.cpu;
    for (unsigned i = 0; i < pvr_compute_job_region_bytes(c, PVR_JOB_OUTPUT) / 4; ++i) words[i] = PVR_COMPUTE_JOB_GUARD;
    for (unsigned i = 0; i < PVR_COMPUTE_JOB_WORDS; ++i) words[16 + i] = ~job_image.expected[i];
    if (!aarch64_device_cache(AARCH64_CACHE_CLEAN_INVALIDATE, (uintptr_t)words, 4096)) return 0;
    __asm__ volatile("dsb sy" ::: "memory");
    return 1;
}
static int job_read_output(void *context, void *out, size_t bytes)
{
    (void)context;
    const struct pvr_mips_buffer *b = &job_backing[PVR_JOB_OUTPUT];
    if (bytes != pvr_compute_job_region_bytes(&job_config, PVR_JOB_OUTPUT) ||
        !aarch64_device_cache(AARCH64_CACHE_INVALIDATE, (uintptr_t)b->storage.cpu, 4096)) return 0;
    const volatile unsigned char *in = b->storage.cpu;
    unsigned char *o = out;
    for (size_t i = 0; i < bytes; ++i) o[i] = in[i];
    __asm__ volatile("dsb sy" ::: "memory");
    return 1;
}
/* Firmware events the job may raise without consequence: a debug dump
 * request, statistics and a core clock rate notice (upstream FWCCB types
 * 106-108). Any other event fails the control. */
static uint32_t job_events;
static int job_event(void *context, const unsigned char command[PVR_CCB_COMMAND_BYTES])
{
    (void)context;
    const uint32_t type = (uint32_t)command[0] | (uint32_t)command[1] << 8 | (uint32_t)command[2] << 16 |
        (uint32_t)command[3] << 24;
    if (type < (PVR_CCB_MAGIC | 106u) || type > (PVR_CCB_MAGIC | 108u)) return 0;
    ++job_events;
    return 1;
}

/* The output as the GPU left it, against the image's expected words and
 * the guards around them. */
static int job_output_matches(void)
{
    const struct pvr_mips_buffer *b = &job_backing[PVR_JOB_OUTPUT];
    if (!aarch64_device_cache(AARCH64_CACHE_INVALIDATE, (uintptr_t)b->storage.cpu, 4096)) return 0;
    const volatile uint32_t *w = b->storage.cpu;
    for (unsigned i = 0; i < 64; ++i)
        if (w[i] != (i >= 16 && i < 48 ? job_image.expected[i - 16] : PVR_COMPUTE_JOB_GUARD)) return 0;
    return 1;
}

/* Bounded steps with the control serviced between them: publication of the
 * workload pages and VM tables, the client CCB format, the job (publish,
 * timeline check, CDM and UPDATE commands, KICK, timeline wait, output
 * check), then the context cleanup. The wait for the timeline and the
 * output check end at deadline without failing the job's transport, which
 * would fail the shared kernel CCB and with it the stop: the job's own
 * deadline is the session's, and the job reads its output only after this
 * function has found it correct. Returns 1 for a complete job. */
static unsigned job_outcome; /* 1 complete, 2 no timeline, 3 output differs, 4 transport */
static int fws_job_run(uint64_t deadline)
{
    struct bb_line line;
    int result = -1;
    unsigned stage = 1;
    for (unsigned i = 0; i < PVR_PROGRAM_REGIONS + PVR_JOB_REGIONS && stage; ++i) {
        const struct pvr_mips_buffer *d = i < PVR_PROGRAM_REGIONS ? &job_programs_backing[i] :
            &job_backing[i - PVR_PROGRAM_REGIONS];
        if (i - PVR_PROGRAM_REGIONS == PVR_JOB_UFO) continue;
        if (!aarch64_device_cache(AARCH64_CACHE_CLEAN_INVALIDATE, (uintptr_t)d->storage.cpu, d->storage.bytes))
            stage = 0;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    if (stage && a733_gpu_vm_memory_publish_begin(&job_vm) == A733_GPU_VM_MEMORY_AGAIN) {
        enum a733_gpu_vm_memory_result r;
        while ((r = a733_gpu_vm_memory_poll(&job_vm)) == A733_GPU_VM_MEMORY_AGAIN)
            if (bb_power_now() >= deadline) break;
        stage = r == A733_GPU_VM_MEMORY_OK ? 2 : 0;
    } else stage = 0;
    static const struct pvr_cccb_ops client_ops = {fws_now, job_read, job_write, job_barrier};
    static const struct pvr_compute_job_ops job_ops = {fws_now, job_publish, job_read_fence, job_read_output};
    if (stage == 2 && pvr_cccb_init(&job_client, &client_ops, NULL, &queue, JOB_RING_BYTES,
                                    job_object_address[0], deadline)) {
        enum pvr_cccb_result r;
        while ((r = pvr_cccb_poll(&job_client)) == PVR_CCCB_AGAIN) {}
        stage = r == PVR_CCCB_OK ? 3 : 0;
    } else stage = 0;
    if (stage == 3 && pvr_compute_job_init(&job, &job_config, &job_image, &job_ops, NULL, &job_client,
                                           bb_power.deadline)) {
        stage = 4;
        job_outcome = 4;
        for (;;) {
            if (pvr_gpu_control_poll(&control, 4) == PVR_GPU_CONTROL_FAULT) break;
            if (job.phase == PVR_JOB_WAIT_FENCE) {
                uint32_t value = 0;
                (void)job_read_fence(NULL, &value);
                if (value == job.sequence && !job_output_matches()) { job_outcome = 3; break; }
                if (value != job.sequence && bb_power_now() >= deadline) { job_outcome = 2; break; }
            } else if (job.phase != PVR_JOB_READ_OUTPUT && bb_power_now() >= deadline) break;
            result = pvr_compute_job_poll(&job);
            if (result) { job_outcome = result == 1 ? 1 : 4; break; }
            if (job.phase == PVR_JOB_WAIT_FENCE || job.phase == PVR_JOB_KICK) vTaskDelay(1);
        }
    }
    uint32_t timeline = 0;
    (void)job_read_fence(NULL, &timeline);
    /* The words the GPU left, read again for the record. */
    const struct pvr_mips_buffer *b = &job_backing[PVR_JOB_OUTPUT];
    if (stage >= 4 && aarch64_device_cache(AARCH64_CACHE_INVALIDATE, (uintptr_t)b->storage.cpu, 4096)) {
        const volatile uint32_t *w = b->storage.cpu;
        job_output_words[0] = w[16]; job_output_words[1] = w[17]; job_output_words[2] = w[47];
        job_output_words[3] = w[15];
    }
    bb_line_start(&line, "GPU FWJ job");
    bb_line_dec(&line, "complete", result == 1); bb_line_dec(&line, "outcome", job_outcome);
    bb_line_dec(&line, "stage", stage);
    bb_line_dec(&line, "phase", job.phase); bb_line_dec(&line, "error", job.error);
    bb_line_dec(&line, "client", job_client.phase); bb_line_dec(&line, "client_error", job_client.error);
    bb_line_hex(&line, "kick_status", job_client.status); bb_line_hex(&line, "timeline", timeline);
    bb_line_end(&line); bb_power_print(&line);
    bb_line_start(&line, "GPU FWJ output");
    bb_line_hex(&line, "w0", job_output_words[0]); bb_line_hex(&line, "w1", job_output_words[1]);
    bb_line_hex(&line, "w31", job_output_words[2]); bb_line_hex(&line, "guard", job_output_words[3]);
    bb_line_hex(&line, "expected0", job_image.expected[0]); bb_line_hex(&line, "expected31", job_image.expected[31]);
    bb_line_end(&line); bb_power_print(&line);
    /* pvr_fw_structure_cleanup: KCCB 106, flags 0, cleanup type 0
     * (FWCOMMONCONTEXT), the context's firmware address; upstream waits for
     * its return word (pvr_fw.c:1127-1158). Bounded by the same deadline. */
    if (stage >= 3 && queue.phase == PVR_CCB_ACTIVE && control.phase == PVR_GPU_CONTROL_READY &&
        (result == 1 || job.phase == PVR_JOB_NEW)) {
        const uint32_t command[PVR_CCB_COMMAND_BYTES / 4] = {PVR_CCB_MAGIC | 106u, 0, 0, job_object_address[0]};
        uint64_t now = bb_power_now();
        if (pvr_ccb_submit(&queue, command, now, deadline, &job_cleanup_token) == PVR_CCB_OK) {
            for (;;) {
                if (pvr_gpu_control_poll(&control, 4) == PVR_GPU_CONTROL_FAULT) break;
                enum pvr_ccb_result r = pvr_ccb_settle(&queue, job_cleanup_token, &job_cleanup_status);
                if (r != PVR_CCB_AGAIN || bb_power_now() >= deadline) break;
                vTaskDelay(1);
            }
        }
    }
    bb_line_start(&line, "GPU FWJ cleanup");
    bb_line_hex(&line, "status", job_cleanup_status); bb_line_dec(&line, "events", job_events);
    bb_line_hex(&line, "kccb_done", *shared_word(PVR_FW_OS_DATA, 568));
    bb_line_hex(&line, "pow_state", *shared_word(PVR_FW_SYSTEM_DATA, 8));
    bb_line_end(&line); bb_power_print(&line);
    return result;
}
