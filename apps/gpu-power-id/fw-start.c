/* SPDX-License-Identifier: MIT
 * The firmware-start variant of the GPU power diagnostic: one ON of the
 * default GPU power path, then the open firmware started as the vendor's
 * host driver starts its own (docs/designs/gpu-firmware-start.md, step 2).
 * It compiles main.c as the power-only variant with its client and entry
 * renamed, as probe-read.c does, and adds its own client and entry.
 */
#define GPU_POWER_ONLY 1
#define kernel_main gpu_power_only_kernel_main
#define gpu_power_id_client gpu_power_only_client
#include "main.c"
#undef kernel_main
#undef gpu_power_id_client
#include "boot.h"
#include "a733_gpu_memory.h"
#include "a733_gpu_opp.h"
#include "pvr_gpu_start.h"
#ifdef FWS_JOB
#ifndef FWS_STOP
#error "the firmware-job variant extends the firmware-stop variant"
#endif
#endif
#ifdef FWS_STOP
#include "pvr_gpu_stop.h"
#endif
#include <string.h>

void kernel_main(void);
void gpu_power_id_client(void *unused);

/* The client, in order (each vendor step cites its source in the design):
 * 1. Prepares the firmware before any power change: code, data and the MIPS
 *    page table in write-back image RAM, the nineteen shared resources in
 *    the image's Normal-NC pool at 0x41c00000, system and OS init with the
 *    1,008 MHz core clock, the queues and the start plan.
 * 2. Powers the GPU on to CORE6 status 8 within 200 ms, as the probe-read
 *    does, and reads the identity and the drm_ctrl and SMC0 words.
 * 3. Starts only while the 1 s start, its 100 ms report and the monitor's
 *    500 ms margin fit before the session and monitor deadlines; otherwise
 *    it returns status 7 with nothing written.
 * 4. Reads RGX_CR_TIMER and writes one timer-correlation record (sequence 1).
 * 5. Writes the secure configuration, SMC0 +0xb0 = 2 and then drm_ctrl = 0,
 *    and never writes either back. Each is read back as an observation:
 *    step 1 on port 3-1 read SMC0 +0xb0 as 0 right after writing 2.
 * 6. Runs the 29 steps of pvr_gpu_start.c, every 64-bit register access as
 *    the vendor's two 32-bit accesses, and waits at most 1 s for the 32-bit
 *    started word.
 * 7. Reports the started word, the firmware's first shared state and the
 *    reset and wrapper registers, and leaves the GPU powered with the
 *    firmware running: the vendor powers the GPU off only after the
 *    firmware's own power-off handshake, which this image does not carry.
 *    The lifetime ends here; a power removal follows.
 * Every GPU, wrapper and SMC0 access is one 32-bit access, made after its
 * progress record and log line.
 *
 * Built with FWS_STOP (the firmware-stop variant), the client does not end
 * at step 7. It then brings the firmware control up (the compatibility
 * record and one health-check command through the kernel CCB), stops the
 * firmware with the forced-idle and power-off requests and the vendor
 * RGXStop register sequence (pvr_gpu_stop.c), powers the GPU off, ends its
 * supply sample and parks the provider, so the lifetime can continue
 * (docs/designs/gpu-firmware-stop.md). The started-word wait is then the
 * vendor's 500 ms. */
#define FWS_ON_US 200000u
#ifdef FWS_STOP
#define FWS_START_US 500000u   /* MAX_HW_TIME_US, rgx_sunxi/sysinfo.h:48 */
#define FWS_CONTROL_US 200000u
#define FWS_STOP_US 300000u
/* Of the stop's 300 ms, the last 100 ms are kept for the stop without the
 * firmware's acknowledgements (pvr_gpu_stop_init_unacknowledged). */
#define FWS_HARD_US 100000u
#else
#define FWS_START_US 1000000u
#define FWS_CONTROL_US 0u
#define FWS_STOP_US 0u
#endif
#ifdef FWS_JOB
#define FWS_JOB_US 200000u
#else
#define FWS_JOB_US 0u
#endif
#define FWS_REPORT_US 100000u
#define MONITOR_DEADLINE_US 2000000u
#define MONITOR_SLACK_US 500000u
#define FWS_REFUSED 7u
#define FWS_MAGIC UINT64_C(0x3130535746555047) /* "GPUFWS01" */
#define WRAPPER_PAGE UINT32_C(0x01880000)
#define SMC0_PAGE UINT32_C(0x0a000000)
#define DRM_CTRL UINT32_C(0x01880014)
#define SMC0_GPU UINT32_C(0x0a0000b0)
#define CR_TIMER UINT32_C(0x160)
#define FW_IMAGE_BYTES 131072u
#define FW_CODE_BYTES 102400u
#define FW_DATA_BYTES 20480u
#define FW_PAGES ((FW_CODE_BYTES + FW_DATA_BYTES + PVR_MIPS_TABLE_BYTES) / 4096u)
/* The firmware's shared resources, then, in the firmware-job variant, the six
 * firmware objects of the compute job. */
#define NC_FW_PAGES 45u
#ifdef FWS_JOB
#define NC_PAGES (NC_FW_PAGES + 6u)
#define FWS_OBJECTS (PVR_FW_RESOURCE_COUNT + 6u)
#else
#define NC_PAGES NC_FW_PAGES
#define FWS_OBJECTS PVR_FW_RESOURCE_COUNT
#endif

enum fws_kind { FWS_READ, FWS_WRITE };
struct fws_row { uint32_t address, value, kind; };
/* Rows 0 to 3 precede the start: the identity, read as the vendor's probe
 * reads it, then drm_ctrl and the SMC0 word. Rows 4 to 7 are
 * sunxi_secure_config (sunxi_platform.c:964-979) in its order, each
 * followed by a read-back. The client reads the table at run time
 * (volatile), so the linked table is the one that runs. */
const volatile struct fws_row fws_rows[8] = {
    {0x01800024, 0, FWS_READ}, {0x01800020, 0, FWS_READ},
    {DRM_CTRL, 0, FWS_READ}, {SMC0_GPU, 0, FWS_READ},
    {SMC0_GPU, 2, FWS_WRITE}, {SMC0_GPU, 0, FWS_READ},
    {DRM_CTRL, 0, FWS_WRITE}, {DRM_CTRL, 0, FWS_READ}};
#define FWS_SECURE 4u
static uint32_t fws_value[8];

/* The record between the result header and the log: n names the access in
 * flight, count the completed ones, step the controller step (0 before the
 * start), value the stored or last read word. */
struct fws_progress { uint64_t magic, n, count, address, value, step, stamp; };
#define FWS_PROGRESS ((volatile struct fws_progress *)(RESULT_BASE + 128))
_Static_assert(sizeof(struct boot_result) == 128 && 128 + sizeof(struct fws_progress) <= LOG_OFFSET,
               "the progress record lies between the result header and the log");

/* struct rogue_fwif_time_corr (upstream pvr_rogue_fwif.h; RGXFWIF_TIME_CORR). */
struct fws_time_corr { uint64_t os_ns, os_mono_ns, cr, k; uint32_t clock_hz, reserved; };
_Static_assert(sizeof(struct fws_time_corr) == 40, "the firmware's timer-correlation record");

static unsigned char fw_memory[FW_PAGES * 4096u] __attribute__((aligned(4096)));
static uint64_t fw_pages[FW_PAGES], nc_pages[NC_FW_PAGES];
static struct pvr_mips_buffer firmware_buffers[3], resources[PVR_FW_RESOURCE_COUNT];
static struct pvr_mips_object mappings[FWS_OBJECTS];
static struct pvr_mips_boot boot;
static struct a733_gpu_memory memory;
static struct pvr_ccb queue;
static struct pvr_gpu_start start;
static unsigned fws_done;
#ifdef FWS_STOP
static struct pvr_gpu_control control;
static struct pvr_gpu_stop stop;
/* The progress record's step: 100 + the stop's register step, or 200 + the
 * control phase while the control or the stop handshake runs. */
#define FWS_STEP_AFTER (stop.phase == PVR_GPU_STOP_REGISTERS ? (stop.unacknowledged ? 150u : 100u) + stop.index : \
                        control.phase ? 200u + control.phase : 0u)
#else
#define FWS_STEP_AFTER 0u
#endif
extern unsigned char __uncached_pool_start[], __uncached_pool_end[];
extern const unsigned char gpu_firmware[], gpu_firmware_end[];

/* One 32-bit access, after its progress record and log line. */
#ifdef FWS_STOP
/* A read that repeats the previous read of the same register is a poll: it
 * is named by the progress record only, and its value is printed only when it
 * changes, so that a poll until a deadline cannot fill the log. */
static uint32_t fws_last_address = UINT32_MAX, fws_last_value;
static int fws_last_read;
#define FWS_QUIET(address, write) (!(write) && fws_last_read && (address) == fws_last_address)
#define FWS_CHANGED(value) ((value) != fws_last_value)
#define FWS_REMEMBER(address, write, value) \
    (fws_last_address = (address), fws_last_read = !(write), fws_last_value = (value))
#else
#define FWS_QUIET(address, write) 0
#define FWS_CHANGED(value) 1
#define FWS_REMEMBER(address, write, value) ((void)0)
#endif
static uint32_t fws_access(uint32_t address, int write, uint32_t value)
{
    const int quiet = FWS_QUIET(address, write);
    FWS_PROGRESS->n = fws_done + 1;
    FWS_PROGRESS->address = address;
    FWS_PROGRESS->value = write ? value : 0;
    FWS_PROGRESS->step = start.phase == PVR_GPU_START_RUNNING ? start.index + 1 : FWS_STEP_AFTER;
    FWS_PROGRESS->stamp = counter();
    __asm__ volatile("dsb sy" ::: "memory");
    struct bb_line line;
    if (!quiet) {
        bb_line_start(&line, write ? "GPU FWS writing" : "GPU FWS reading");
        bb_line_dec(&line, "n", fws_done + 1); bb_line_hex(&line, "a", address);
        if (write) bb_line_hex(&line, "v", value);
        bb_line_end(&line); bb_power_print(&line);
    }
    if (write) a733_mmio_write32_dsb_sy_both(address, value);
    else value = a733_mmio_read32_dsb_sy_both(address);
    FWS_PROGRESS->value = value;
    FWS_PROGRESS->count = ++fws_done;
    __asm__ volatile("dsb sy" ::: "memory");
    const int changed = !quiet || FWS_CHANGED(value);
    FWS_REMEMBER(address, write, value);
    if (!write && changed) {
        bb_line_start(&line, "GPU FWS");
        bb_line_dec(&line, "n", fws_done); bb_line_hex(&line, "v", value);
        bb_line_end(&line); bb_power_print(&line);
    }
    return value;
}

static uint32_t fws_row(unsigned i)
{
    const struct fws_row r = {fws_rows[i].address, fws_rows[i].value, fws_rows[i].kind};
    return fws_value[i] = fws_access(r.address, r.kind == FWS_WRITE, r.value);
}

/* OSReadHWReg64 and OSWriteHWReg64 (osfunc.h:1086-1107): two 32-bit
 * accesses, the high word read first and the low word written first. */
static uint64_t gpu_read64(uint32_t offset)
{
    uint64_t high = fws_access(GPU_BASE + offset + 4, 0, 0);
    return high << 32 | fws_access(GPU_BASE + offset, 0, 0);
}
static void gpu_write64(uint32_t offset, uint64_t value)
{
    (void)fws_access(GPU_BASE + offset, 1, (uint32_t)value);
    (void)fws_access(GPU_BASE + offset + 4, 1, (uint32_t)(value >> 32));
}

/* The register pages the start uses: core control, SLC and AXI, and the
 * secure-bus control. */
static int gpu_page(uint32_t offset, uint32_t width)
{
    uint32_t page = offset >> 12;
    return (width == 4 || width == 8) && ((offset + width - 1) >> 12) == page &&
        (page == 0 || page == 3 || page == 10);
}
/* The callbacks admit only the access of the controller's current step. */
static int current(uint32_t offset, uint32_t width)
{
    const struct pvr_gpu_start_step *s = &start.steps[start.index];
    return start.phase == PVR_GPU_START_RUNNING && start.index < start.count &&
        s->offset == offset && s->width == width && gpu_page(offset, width);
}
static int fws_now(void *context, uint64_t *now) { (void)context; *now = bb_power_now(); return 1; }
static int fws_read(void *context, uint32_t offset, uint32_t width, uint64_t *out)
{
    (void)context;
    if (!current(offset, width)) return 0;
    *out = width == 4 ? fws_access(GPU_BASE + offset, 0, 0) : gpu_read64(offset);
    return 1;
}
static int fws_write(void *context, uint32_t offset, uint32_t width, uint64_t value)
{
    (void)context;
    if (!current(offset, width) || (width == 4 && value > UINT32_MAX)) return 0;
    if (width == 4) (void)fws_access(GPU_BASE + offset, 1, (uint32_t)value);
    else gpu_write64(offset, value);
    return 1;
}
static int fws_started(void *context, uint32_t *value) { (void)context; return a733_gpu_memory_started(&memory, value); }
static int fws_publish(void *context) { (void)context; return a733_gpu_memory_publish(&memory); }
static int fws_kick(void *context, uint32_t offset, uint32_t value)
{
    (void)context;
#ifdef FWS_STOP
    /* __MTSScheduleWrite (rgxfwutils.c:215-234): the GP kick, then a
     * read-back so that the write reaches the register bank. */
    if (offset != 0xb00 || value || memory.phase != A733_GPU_MEMORY_PUBLISHED) return 0;
    (void)fws_access(GPU_BASE + offset, 1, value);
    (void)fws_access(GPU_BASE + offset, 0, 0);
    return 1;
#else
    (void)offset; (void)value;
    return 0; /* No command is submitted. */
#endif
}
static const struct pvr_gpu_start_ops fws_ops = {fws_now, fws_read, fws_write, fws_started, fws_publish};
static const struct pvr_ccb_ops queue_ops = {a733_gpu_memory_read, a733_gpu_memory_write,
                                             a733_gpu_memory_barrier, fws_kick};

static struct pvr_mips_buffer child(uintptr_t base, uint64_t *pages, size_t offset, size_t bytes)
{
    unsigned first = (unsigned)(offset / 4096), count = (unsigned)(bytes / 4096);
    for (unsigned i = 0; i < count; ++i) pages[first + i] = base + offset + i * 4096u;
    return (struct pvr_mips_buffer){{(void *)(base + offset), bytes}, pages + first, count};
}

/* Identity-mapped buffers: the firmware's physical addresses are the CPU's.
 * a733_gpu_memory_init() checks each page's translation and attributes. */
#ifdef FWS_JOB
static int fws_job_prepare(uintptr_t nc, struct pvr_mips_object *objects);
#endif
static int fws_prepare(void)
{
    const uintptr_t nc = (uintptr_t)__uncached_pool_start;
    const size_t bytes = (size_t)(gpu_firmware_end - gpu_firmware);
    struct pvr_fw_plan plan;
    if (nc != UINT32_C(0x41c00000) || (uintptr_t)__uncached_pool_end - nc < NC_PAGES * 4096u ||
        bytes != FW_IMAGE_BYTES || !pvr_fw_requirements(gpu_firmware, bytes, PVR_FW_A733_BVNC, &plan) ||
        plan.allocation_bytes[0] != FW_CODE_BYTES || plan.allocation_bytes[1] != FW_DATA_BYTES) return 0;
    const size_t sizes[3] = {FW_CODE_BYTES, FW_DATA_BYTES, PVR_MIPS_TABLE_BYTES};
    size_t offset = 0;
    for (unsigned i = 0; i < 3; ++i) {
        firmware_buffers[i] = child((uintptr_t)fw_memory, fw_pages, offset, sizes[i]);
        offset += sizes[i];
    }
    offset = 0;
    for (unsigned i = 0; i < PVR_FW_RESOURCE_COUNT; ++i) {
        size_t n = pvr_fw_state_layout((enum pvr_fw_resource)i)->allocation_bytes;
        resources[i] = child(nc, nc_pages, offset, n);
        offset += n;
    }
    if (offset != NC_FW_PAGES * 4096u || !a733_gpu_memory_init(&memory, firmware_buffers, resources)) return 0;
    /* The pool holds whatever the previous image left in that DRAM. */
    memset((void *)nc, 0, NC_PAGES * 4096u);
    const struct pvr_fw_state_config config = {PVR_FW_A733_BVNC, A733_GPU_CORE_HZ, 1, 16};
    return pvr_fw_state_prepare(resources, &config, mappings) &&
#ifdef FWS_JOB
        fws_job_prepare(nc + NC_FW_PAGES * 4096u, mappings + PVR_FW_RESOURCE_COUNT) &&
#endif
        pvr_mips_prepare(gpu_firmware, bytes, firmware_buffers, mappings, FWS_OBJECTS, &boot) &&
        pvr_gpu_start_init(&start, gpu_firmware, bytes, &boot, &fws_ops, NULL) && start.count == 29 &&
        pvr_ccb_init(&queue, &queue_ops, &memory, 1) == PVR_CCB_OK;
}

/* As probe-read.c prints them: the controller's state after the ON, and an
 * identity with its fields. */
static void print_power(void)
{
    const struct a733_gpu_power *p = &gpu.power;
    struct bb_line line;
    bb_line_start(&line, "GPU power");
    bb_line_dec(&line, "phase", p->phase); bb_line_dec(&line, "error", p->error);
    bb_line_dec(&line, "step", p->index); bb_line_hex(&line, "last", p->last_value);
    bb_line_end(&line); bb_power_print(&line);
    bb_line_start(&line, "GPU resources");
    bb_line_dec(&line, "result", gpu.resource_result); bb_line_dec(&line, "error", bb_power.gpu.error);
    bb_line_dec(&line, "stage", bb_power.gpu.stage); bb_line_hex(&line, "last", bb_power.gpu.last_value);
    bb_line_end(&line); bb_power_print(&line);
    core_report();
}
static void print_identity(const char *prefix, uint64_t value)
{
    struct bb_line line;
    bb_line_start(&line, prefix);
    bb_line_hex(&line, "raw", value);
    bb_line_dec(&line, "b", value >> 48); bb_line_dec(&line, "v", (value >> 32) & 0xffff);
    bb_line_dec(&line, "n", (value >> 16) & 0xffff); bb_line_dec(&line, "c", value & 0xffff);
    bb_line_dec(&line, "matches", value == EXPECTED_BVNC);
    bb_line_end(&line); bb_power_print(&line);
}

static volatile uint32_t *shared_word(enum pvr_fw_resource r, size_t offset)
{
    return (volatile uint32_t *)((uintptr_t)resources[r].storage.cpu + offset);
}
#ifdef FWS_JOB
#include "fw-job.inc"
#endif

/* _RGXMakeTimeCorrData (rgxtimecorr.c:271-343): the CR timer, read again
 * when its bit-31 copy in bit 63 differs from bit 31, bits 47:0; then the
 * OS time in ns; K = (256 * 10^6 << 20) / clock kHz (rgxtimecorr.h:105-119).
 * The record goes into slot 1 of 256, then a barrier, then sequence 1. */
static struct fws_time_corr fws_timer;
static unsigned fws_timer_reads;
static void timer_record(void)
{
    uint64_t cr = gpu_read64(CR_TIMER);
    fws_timer_reads = 1;
    if ((cr ^ (cr << 32)) & (UINT64_C(1) << 63)) { cr = gpu_read64(CR_TIMER); fws_timer_reads = 2; }
    const uint64_t os_ns = counter() * 125u / 3u;
    const uint32_t khz = (A733_GPU_CORE_HZ + 500u) / 1000u;
    fws_timer = (struct fws_time_corr){os_ns, 0, cr & UINT64_C(0xffffffffffff),
                                       (UINT64_C(256000000) << 20) / khz, A733_GPU_CORE_HZ, 0};
    volatile struct fws_time_corr *slot = (volatile struct fws_time_corr *)resources[PVR_FW_UTILIZATION].storage.cpu + 1;
    slot->os_ns = fws_timer.os_ns; slot->os_mono_ns = 0; slot->cr = fws_timer.cr;
    slot->k = fws_timer.k; slot->clock_hz = fws_timer.clock_hz; slot->reserved = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    *shared_word(PVR_FW_UTILIZATION, 10240) = 1;
    __asm__ volatile("dsb sy" ::: "memory");
}

static void print_start(enum pvr_gpu_start_result result, uint64_t waited)
{
    struct bb_line line;
    bb_line_start(&line, "GPU FWS start");
    bb_line_dec(&line, "result", result); bb_line_dec(&line, "phase", start.phase);
    bb_line_dec(&line, "error", start.error); bb_line_dec(&line, "step", start.index);
    bb_line_dec(&line, "count", start.count); bb_line_dec(&line, "waited_us", waited);
    bb_line_dec(&line, "memory", memory.phase);
    bb_line_end(&line); bb_power_print(&line);
    /* The firmware's first host-visible state: system init's started word,
     * marker and start timestamp, system data's flags and power state, the
     * connection states and the executed command count. */
    bb_line_start(&line, "GPU FWS shared");
    bb_line_hex(&line, "started", *shared_word(PVR_FW_SYSTEM_INIT, 352));
    bb_line_hex(&line, "marker", *shared_word(PVR_FW_SYSTEM_INIT, 356));
    bb_line_hex(&line, "started_ts", *shared_word(PVR_FW_SYSTEM_INIT, 360));
    bb_line_end(&line); bb_power_print(&line);
    bb_line_start(&line, "GPU FWS state");
    bb_line_hex(&line, "sys_flags", *shared_word(PVR_FW_SYSTEM_DATA, 0));
    bb_line_hex(&line, "pow_state", *shared_word(PVR_FW_SYSTEM_DATA, 8));
    bb_line_hex(&line, "sys_data_flags", *shared_word(PVR_FW_SYSTEM_DATA, 3616));
    bb_line_hex(&line, "conn_fw", *shared_word(PVR_FW_CONNECTION, 0));
    bb_line_hex(&line, "conn_os", *shared_word(PVR_FW_CONNECTION, 4));
    bb_line_hex(&line, "kccb_done", *shared_word(PVR_FW_OS_DATA, 568));
    bb_line_end(&line); bb_power_print(&line);
}

/* SOFT_RESET, then the wrapper registers the start wrote, read back. */
static const uint32_t after_registers[][2] = {
    {0x100, 8}, {0x810, 4}, {0x818, 8}, {0x820, 8}, {0x858, 8}, {0x860, 8}, {0x828, 8},
    {0x830, 8}, {0x838, 8}, {0x840, 8}, {0xb50, 8}, {0x8c8, 4}};
static void read_after(void)
{
    for (unsigned i = 0; i < sizeof(after_registers) / sizeof(*after_registers); ++i) {
        const uint32_t offset = after_registers[i][0];
        const uint64_t value = after_registers[i][1] == 4 ? fws_access(GPU_BASE + offset, 0, 0) : gpu_read64(offset);
        struct bb_line line;
        bb_line_start(&line, "GPU FWS register");
        bb_line_hex(&line, "offset", offset); bb_line_hex(&line, "v", value);
        bb_line_end(&line); bb_power_print(&line);
    }
}

#ifdef FWS_STOP
/* The stop callbacks admit only the access of the stop's current register
 * step, on the core control, BIF status and SLC pages. */
static int stop_current(uint32_t offset, uint32_t width)
{
    const struct pvr_gpu_stop_step *s = &stop.steps[stop.index];
    const uint32_t page = offset >> 12;
    return stop.phase == PVR_GPU_STOP_REGISTERS && stop.index < stop.count && s->offset == offset &&
        s->width == width && (width == 4 || width == 8) && ((offset + width - 1) >> 12) == page &&
        (page == 0 || page == 1 || page == 3);
}
/* The control reads the MIPS wrapper interrupt status and clears its event
 * bit (RGXAckIrqMIPS, rgxinit.c:420-462); no other register. */
static int control_read32(void *context, uint32_t offset, uint32_t *out)
{
    (void)context;
    if (offset != 0x8a8) return 0;
    *out = fws_access(GPU_BASE + offset, 0, 0);
    return 1;
}
static int control_write32(void *context, uint32_t offset, uint32_t value)
{
    (void)context;
    if (offset != 0x8b0 || value != 1) return 0;
    (void)fws_access(GPU_BASE + offset, 1, value);
    return 1;
}
static int control_compat(void *context, uint32_t offset, void *out, size_t bytes)
{ (void)context; return a733_gpu_memory_compat(&memory, offset, out, bytes); }
/* The executed kernel-command count, by which upstream judges a health
 * check (pvr_power.c:150-185): on port 3-1 the firmware counted the health
 * check executed without writing its return word
 * (board-session-20261025-18, s18-gfx-run). */
static int control_executed(void *context, uint32_t *out)
{ (void)context; return a733_gpu_memory_executed(&memory, out); }
static int stop_read(void *context, uint32_t offset, uint32_t width, uint64_t *out)
{
    (void)context;
    if (!stop_current(offset, width)) return 0;
    *out = width == 4 ? fws_access(GPU_BASE + offset, 0, 0) : gpu_read64(offset);
    return 1;
}
static int stop_write(void *context, uint32_t offset, uint32_t width, uint64_t value)
{
    (void)context;
    if (!stop_current(offset, width)) return 0;
    /* The stop without acknowledgements ends with the full soft reset,
     * written as the vendor writes a 64-bit register: low word first. */
    if (stop.unacknowledged && offset == 0x100 && width == 8 && value == stop.steps[stop.index].value &&
        stop.index + 1 == stop.count) {
        gpu_write64(offset, value);
        return 1;
    }
    if (width != 4 ||
        !(((offset == 0xb40 || offset == 0xb30) && !value) || (offset == 0x8b0 && value == 1))) return 0;
    (void)fws_access(GPU_BASE + offset, 1, (uint32_t)value);
    return 1;
}
static int stop_sync_write(void *context, uint32_t value)
{ (void)context; return a733_gpu_memory_sync_write(&memory, value); }
/* The firmware's power state after each acknowledgement: FORCED_IDLE (2)
 * after the forced-idle request, OFF (0) after the power-off request, which
 * RGXPrePowerState requires before it stops the GPU (rgxpower.c:410-428). */
static uint32_t fws_pow_state[2] = {UINT32_MAX, UINT32_MAX};
static int stop_sync_read(void *context, uint32_t *out)
{
    (void)context;
    uint32_t ack, state;
    if (!a733_gpu_memory_sync_read(&memory, &ack)) return 0;
    if (ack) {
        const unsigned off = stop.phase == PVR_GPU_STOP_WAIT_OFF;
        if (ack != 1 || (stop.phase != PVR_GPU_STOP_WAIT_IDLE && !off) ||
            !a733_gpu_memory_power_state(&memory, &state)) return 0;
        fws_pow_state[off] = state;
        if (state != (off ? 0u : 2u)) return 0;
    }
    *out = ack;
    return 1;
}
/* The stop's isolation is the platform OFF of the default path. */
static int stop_isolate(void *context) { (void)context; return a733_gpu_platform_off_begin(&gpu, bb_power.deadline); }
static int stop_isolate_poll(void *context) { (void)context; return a733_gpu_platform_off_poll(&gpu); }

static void print_control(enum pvr_gpu_control_result result, uint64_t waited)
{
    const unsigned char *c = control.compatibility;
    struct bb_line line;
    bb_line_start(&line, "GPU FWS control");
    bb_line_dec(&line, "result", result); bb_line_dec(&line, "phase", control.phase);
    bb_line_dec(&line, "error", control.error); bb_line_dec(&line, "health", control.health_done);
    bb_line_hex(&line, "health_status", control.health_status); bb_line_dec(&line, "waited_us", waited);
    bb_line_end(&line); bb_power_print(&line);
    bb_line_start(&line, "GPU FWS compat");
    bb_line_hex(&line, "w0", (uint32_t)c[0] | (uint32_t)c[1] << 8 | (uint32_t)c[2] << 16 | (uint32_t)c[3] << 24);
    bb_line_hex(&line, "w16", (uint32_t)c[16] | (uint32_t)c[17] << 8 | (uint32_t)c[18] << 16 | (uint32_t)c[19] << 24);
    bb_line_hex(&line, "updated", c[56]);
    bb_line_hex(&line, "kccb_done", *shared_word(PVR_FW_OS_DATA, 568));
    bb_line_hex(&line, "pow_state", *shared_word(PVR_FW_SYSTEM_DATA, 8));
    bb_line_end(&line); bb_power_print(&line);
    /* The kernel CCB as the firmware left it: its read offset, the return
     * word of the health check's slot and the executed count before it. */
    bb_line_start(&line, "GPU FWS kccb");
    bb_line_hex(&line, "write", *shared_word(PVR_FW_HOST_CONTROL, 0));
    bb_line_hex(&line, "read", *shared_word(PVR_FW_HOST_CONTROL, 128));
    bb_line_hex(&line, "rtn0", *shared_word(PVR_FW_RETURNS, 0));
    bb_line_hex(&line, "executed_before", control.executed_before);
    bb_line_hex(&line, "executed_after", control.executed_after);
    bb_line_end(&line); bb_power_print(&line);
}
static void print_stop(enum pvr_gpu_stop_result result, uint64_t waited)
{
    struct bb_line line;
    bb_line_start(&line, "GPU FWS stop");
    bb_line_dec(&line, "result", result); bb_line_dec(&line, "phase", stop.phase);
    bb_line_dec(&line, "error", stop.error); bb_line_dec(&line, "step", stop.index);
    bb_line_dec(&line, "count", stop.count); bb_line_dec(&line, "waited_us", waited);
    bb_line_dec(&line, "unacknowledged", stop.unacknowledged);
    bb_line_end(&line); bb_power_print(&line);
    bb_line_start(&line, "GPU FWS power");
    bb_line_hex(&line, "ack_idle", stop.acknowledgement[0]); bb_line_hex(&line, "ack_off", stop.acknowledgement[1]);
    bb_line_hex(&line, "state_idle", fws_pow_state[0]); bb_line_hex(&line, "state_off", fws_pow_state[1]);
    bb_line_hex(&line, "status_idle", stop.command_status[0]); bb_line_hex(&line, "status_off", stop.command_status[1]);
    bb_line_hex(&line, "irq", stop.irq_status); bb_line_dec(&line, "isolated", stop.isolation_observed);
    bb_line_end(&line); bb_power_print(&line);
    bb_line_start(&line, "GPU FWS after");
    bb_line_hex(&line, "kccb_done", *shared_word(PVR_FW_OS_DATA, 568));
    bb_line_hex(&line, "pow_state", *shared_word(PVR_FW_SYSTEM_DATA, 8));
    bb_line_hex(&line, "kccb_read", *shared_word(PVR_FW_HOST_CONTROL, 128));
    bb_line_hex(&line, "rtn1", *shared_word(PVR_FW_RETURNS, 4));
    bb_line_hex(&line, "rtn2", *shared_word(PVR_FW_RETURNS, 8));
    bb_line_end(&line); bb_power_print(&line);
}
/* After the OFF: the supply end and the provider park of the power-only
 * client, each proved. */
static void fws_park(void)
{
    int result;
    gates_report();
    off_proof();
    while ((result = a733_gpu_platform_supply_end(&gpu)) == 0) gpu_pause("GPU supply end deadline");
    if (result != 1) gpu_fail("GPU supply end");
    bb_power_park();
    off_proof();
    if (!atomic_load_explicit(&bb_power.parked, memory_order_acquire) ||
        !a733_power_service_idle(&bb_power.domains)) bb_power_fail("GPU provider park proof");
    for (unsigned i = 0; i < A733_PMIC_REQUESTS; ++i)
        if (bb_power.pmic.slots[i].state || bb_power.pmic.slots[i].borrow.id)
            bb_power_fail("GPU provider request retained");
    before();
}
static _Noreturn void fws_stop_run(void)
{
#ifdef FWS_JOB
    static const struct pvr_gpu_control_ops control_ops = {fws_now, control_read32, control_write32,
                                                           control_compat, job_event};
#else
    static const struct pvr_gpu_control_ops control_ops = {fws_now, control_read32, control_write32,
                                                           control_compat, NULL};
#endif
    static const struct pvr_gpu_stop_ops stop_ops = {fws_now, stop_read, stop_write, stop_sync_write,
                                                     stop_sync_read, stop_isolate, stop_isolate_poll};
    uint64_t begun = bb_power_now();
    if (!pvr_gpu_control_init(&control, &start, &queue, &control_ops, NULL, begun + FWS_CONTROL_US) ||
        !pvr_gpu_control_count_executed(&control, control_executed))
        bb_power_fail("GPU firmware control refused, GPU left running");
    enum pvr_gpu_control_result c;
    while ((c = pvr_gpu_control_poll(&control, 4)) == PVR_GPU_CONTROL_AGAIN) vTaskDelay(1);
    print_control(c, bb_power_now() - begun);
    if (c != PVR_GPU_CONTROL_OK)
        bb_power_fail("GPU firmware control: compatibility or health check not complete, GPU left running");
#ifdef FWS_JOB
    /* The job, then the stop and OFF whatever its result, so the lifetime
     * survives when the firmware acknowledges its power-off. */
    const int job_result = fws_job_run(bb_power_now() + FWS_JOB_US);
#endif
    begun = bb_power_now();
    const size_t firmware_bytes = (size_t)(gpu_firmware_end - gpu_firmware);
    enum pvr_gpu_stop_result s = PVR_GPU_STOP_INVALID;
    if (pvr_gpu_stop_init(&stop, &control, gpu_firmware, firmware_bytes, &stop_ops, NULL,
                          begun + FWS_STOP_US - FWS_HARD_US)) {
        while ((s = pvr_gpu_stop_poll(&stop)) == PVR_GPU_STOP_AGAIN) vTaskDelay(1);
        print_stop(s, bb_power_now() - begun);
    }
    if (s != PVR_GPU_STOP_OK && !stop.acknowledgement[1] && !stop.isolation_started) {
        /* Neither power-off acknowledgement nor isolation: the control
         * failed, or the firmware did not acknowledge a request. As
         * upstream's hard reset (pvr_power.c:518-531): pvr_fw_stop's
         * register sequence without the requests, then the OFF, admitted
         * only when the control and its queue are latched failed. */
        if (control.phase != PVR_GPU_CONTROL_FAILED) (void)pvr_gpu_control_abort(&control);
        if (!pvr_gpu_stop_init_unacknowledged(&stop, &control, gpu_firmware, firmware_bytes, &stop_ops, NULL,
                                              begun + FWS_STOP_US))
            bb_power_fail("GPU firmware stop refused, GPU left running");
        while ((s = pvr_gpu_stop_poll(&stop)) == PVR_GPU_STOP_AGAIN) vTaskDelay(1);
        print_stop(s, bb_power_now() - begun);
        if (s != PVR_GPU_STOP_OK)
            bb_power_fail("GPU firmware stop: not acknowledged and the GPU not idle, GPU left powered");
        fws_park();
        bb_power_fail("GPU firmware stop: not acknowledged, idle checks met, GPU OFF and provider parked");
    }
    if (s != PVR_GPU_STOP_OK) {
        /* The vendor powers the GPU off even when RGXStop fails after the
         * firmware acknowledged the power-off (RGXFinalisePowerOff,
         * rgxpower.c:333-341); before that acknowledgement the firmware
         * may still run, and the GPU stays on. */
        if (!stop.acknowledgement[1] || stop.isolation_started)
            bb_power_fail("GPU firmware stop: not complete, GPU left powered");
        int result;
        if (!a733_gpu_platform_off_begin(&gpu, bb_power.deadline)) gpu_fail("GPU OFF begin");
        while ((result = a733_gpu_platform_off_poll(&gpu)) == 0) gpu_pause("GPU OFF deadline");
        if (result != 1) gpu_fail("GPU OFF poll");
        fws_park();
        bb_power_fail("GPU firmware stop: idle check not met after the power-off acknowledgement, "
                      "GPU OFF and provider parked");
    }
    fws_park();
#ifdef FWS_JOB
    if (job_result != 1) bb_power_fail("GPU first job: not complete, GPU OFF and provider parked");
    if (job_cleanup_status != 1)
        bb_power_fail("GPU first job: 32 words as computed, context cleanup not confirmed, GPU OFF and provider parked");
    platform_puts("COMPLETE GPU first job: 32 words as computed, context cleaned up, firmware stopped, OFF and "
                  "provider parked\n");
#else
    platform_puts("COMPLETE GPU firmware stop: started, health check executed, forced idle and power off "
                  "acknowledged, idle, OFF and provider parked\n");
#endif
    platform_exit(0);
}
#endif

void gpu_power_id_client(void *unused)
{
    (void)unused;
    bb_power_wait_ready();
    FWS_PROGRESS->magic = FWS_MAGIC;
    FWS_PROGRESS->n = FWS_PROGRESS->count = FWS_PROGRESS->step = 0;
    FWS_PROGRESS->stamp = counter();
    struct bb_line line;
    if (!fws_prepare()) bb_power_fail("GPU firmware start preparation, nothing powered");
    bb_line_start(&line, "GPU FWS prepared");
    bb_line_hex(&line, "code", (uintptr_t)firmware_buffers[0].storage.cpu);
    bb_line_hex(&line, "data", (uintptr_t)firmware_buffers[1].storage.cpu);
    bb_line_hex(&line, "table", (uintptr_t)firmware_buffers[2].storage.cpu);
    bb_line_hex(&line, "shared", (uintptr_t)resources[0].storage.cpu);
    bb_line_end(&line); bb_power_print(&line);
    bb_line_start(&line, "GPU FWS boot");
    bb_line_hex(&line, "code", boot.writes[2].value); bb_line_hex(&line, "data", boot.writes[6].value);
    bb_line_hex(&line, "exceptions", boot.writes[8].value); bb_line_dec(&line, "steps", start.count);
    bb_line_dec(&line, "clock_hz", A733_GPU_CORE_HZ);
    bb_line_end(&line); bb_power_print(&line);
    snapshot(0);
    cycle = 1;
    before();
    const uint64_t on_start = last_now, on_deadline = last_now + FWS_ON_US;
    if (on_deadline + FWS_REPORT_US > bb_power.deadline) bb_power_fail("GPU firmware start time budget");
    if (!a733_gpu_platform_begin(&gpu, bb_power.deadline)) gpu_fail("GPU ON begin");
    int result;
    while ((result = a733_gpu_platform_poll(&gpu)) == 0) {
        if (bb_power_now() >= on_deadline) break;
        vTaskDelay(1);
    }
    before();
    snapshot(1);
    print_power();
    bb_line_start(&line, "GPU FWS core6");
    bb_line_dec(&line, "on", result == 1); bb_line_hex(&line, "status", gpu.power.last_value);
    bb_line_dec(&line, "waited_us", last_now - on_start);
    bb_line_end(&line); bb_power_print(&line);
    if (result < 0) gpu_fail("GPU ON poll");
    if (result != 1) gpu_fail("GPU ON deadline");
    static const struct a733_mapping_rule read_write = {A733_MAPPING_READ_WRITE, 1,
        {{A733_MAIR_DEVICE_NGNRNE, A733_SH_ANY}}, 1, 1};
    static const uintptr_t pages[] = {WRAPPER_PAGE, SMC0_PAGE, GPU_BASE, GPU_BASE + 0x3000, GPU_BASE + 0xa000
#ifdef FWS_STOP
        , GPU_BASE + 0x1000 /* the stop's BIF status registers */
#endif
    };
    before();
    for (unsigned i = 0; i < sizeof(pages) / sizeof(*pages); ++i)
        if (!a733_mapping_check(aarch64_device_translate, pages[i], pages[i], &read_write))
            gpu_fail("GPU firmware start mapping");
    for (unsigned i = 0; i < FWS_SECURE; ++i) (void)fws_row(i);
    const uint64_t identity = (uint64_t)fws_value[0] << 32 | fws_value[1];
    print_identity("GPU FWS identity", identity);
    before();
    if (identity != EXPECTED_BVNC)
        bb_power_fail("GPU firmware start: identity not 36.56.104.183, nothing written, GPU left powered");

    /* The start begins only while its bound, the control and stop bounds of
     * the stop variant and the report fit before the session deadline, and
     * their end keeps the monitor's 500 ms margin. */
    const uint64_t now = bb_power_now(), start_deadline = now + FWS_START_US,
        monitor_deadline = RESULT->start_counter / 24 + MONITOR_DEADLINE_US,
        last_deadline = start_deadline + FWS_CONTROL_US + FWS_JOB_US + FWS_STOP_US;
    if (last_deadline + FWS_REPORT_US > bb_power.deadline || last_deadline + MONITOR_SLACK_US > monitor_deadline) {
        bb_line_start(&line, "GPU FWS refused");
        bb_line_dec(&line, "now_us", now); bb_line_dec(&line, "session_deadline_us", bb_power.deadline);
        bb_line_dec(&line, "monitor_deadline_us", monitor_deadline);
        bb_line_end(&line); bb_power_print(&line);
        platform_puts("FAIL GPU firmware start refused: deadline too near, nothing written\n");
        platform_exit(FWS_REFUSED);
    }
    timer_record();
    bb_line_start(&line, "GPU FWS timer");
    bb_line_hex(&line, "cr", fws_timer.cr); bb_line_dec(&line, "os_ns", fws_timer.os_ns);
    bb_line_dec(&line, "k", fws_timer.k); bb_line_dec(&line, "clock_hz", fws_timer.clock_hz);
    bb_line_dec(&line, "reads", fws_timer_reads);
    bb_line_end(&line); bb_power_print(&line);
    for (unsigned i = FWS_SECURE; i < sizeof(fws_rows) / sizeof(*fws_rows); ++i) (void)fws_row(i);
    bb_line_start(&line, "GPU FWS secure");
    bb_line_hex(&line, "smc0_entry", fws_value[3]); bb_line_hex(&line, "smc0_after", fws_value[5]);
    bb_line_hex(&line, "drm_entry", fws_value[2]); bb_line_hex(&line, "drm_after", fws_value[7]);
    bb_line_end(&line); bb_power_print(&line);

    const uint64_t begun = bb_power_now();
    enum pvr_gpu_start_result r = pvr_gpu_start_begin(&start, start_deadline);
    while (r == PVR_GPU_START_AGAIN) {
        r = pvr_gpu_start_poll(&start);
        /* The started-word wait re-reads for 50 us per poll; the image then
         * yields one tick, as the vendor polls every 50 us. */
        if (r == PVR_GPU_START_AGAIN && start.index + 1 == start.count) vTaskDelay(1);
    }
    const uint64_t ended = bb_power_now();
    print_start(r, ended - begun);
    read_after();
    if (r != PVR_GPU_START_OK)
        bb_power_fail("GPU firmware start: started word not 1, GPU left running");
#ifdef FWS_STOP
    fws_stop_run();
#else
    platform_puts("COMPLETE GPU firmware start: started word 1 after the timer record and the secure "
                  "configuration, GPU left running for the power removal\n");
    platform_exit(0);
#endif
}

void kernel_main(void)
{
    platform_init();
    struct a733_pmic_lifetime life;
    if (!bb_power_snapshot_init(gpu_power_id_client, "gpu-id") ||
        !a733_gpu_platform_native_init(&native, &bb_power.locks.callbacks) ||
        !a733_gpu_platform_init(&gpu, &bb_power.gpu, GPU_CLIENT, &native.operations, &native) ||
        a733_pmic_identity(&bb_power.pmic, &life) != A733_PMIC_READY ||
        !a733_gpu_platform_bind_pmic(&gpu, &bb_power.gpu_native, &bb_power.pmic, &life, BB_POWER_CLIENT_OWNER))
        platform_panic("GPU firmware-start initialization", 0);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
