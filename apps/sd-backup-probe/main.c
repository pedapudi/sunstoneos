/* Read-only microSD backup probe on the native probe ABI (apps/sd-backup-probe/README.md), forked from apps/sd-read-probe.
 * It adds the SD Status read (ACMD13) and captures up to 512 listed sectors into an image-RAM payload; the plan reads one
 * block so the shared flow is unchanged. Everything below is the read probe's.
 *
 * It runs the production SD0 provider with the production clock, pin, rail and
 * PMIC providers as two FreeRTOS tasks, identifies the card in SD profile 2
 * (four-bit high speed, 3.3 V), reads the input's sequential plan with descriptor
 * DMA, times every read, stores a few single sectors for the card layout, closes
 * the card and returns a binary report. The SD core is built read-only
 * (SUNXI_MMC_READ_ONLY): no write or erase command can reach the card. The
 * structure follows apps/storage-read-returning, whose private EL3 return wrapper
 * the probe ABI replaces with sunstone_probe_quiesce(). */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "platform.h"
#include "boot.h"
#include "a733_range.h"
#include "a733_counter_clock.h"
#include "a733_platform_locks.h"
#include "a733_clock_leases.h"
#include "a733_pins_native.h"
#include "a733_gpu_resources_native.h"
#include "a733_rail_leases.h"
#include "a733_pmic_native.h"
#include "a733_mmc_platform_native.h"
#include "a733_mapping_check.h"
#include "a733_el3_mbus.h"
#include "aarch64_device_memory.h"
#include "sunxi_smhc.h"
#include "sunstone_probe.h"
#include "probe.h"
#include "fat32_ro.h"
#include "region.h"
#include "a733_storage_digest.h"
#include <stdatomic.h>
#include <string.h>

struct sdp_input sunstone_probe_input __attribute__((section(".data.sdp_probe_input")));
struct sdp_report sunstone_probe_payload;       /* the report is large (512 captured sectors): it lives in image RAM, not at 0x42000100 */
SUNSTONE_PROBE_IMAGE_FLAGS((uintptr_t)&sunstone_probe_payload, sizeof(struct sdp_report), (uintptr_t)&sunstone_probe_input,
    sizeof(sunstone_probe_input), 90, SUNSTONE_PROBE_RETURNABLE | SUNSTONE_PROBE_STORAGE_READ_ONLY);
#define REPORT ((volatile struct sdp_report *)&sunstone_probe_payload)

/* Deadlines in microseconds after the start of kernel_main(); the session allows 20 s. */
#define IDENTIFY_US 3000000u
#define READ_US 200000u
/* The plan window and the lifetime grow when the probe loads a file (setup sets them). */
static uint32_t plan_end_us = 12000000u, lifetime_us = 14000000u, end_us = 15000000u;
#define PLAN_END_US plan_end_us
#define LIFETIME_US lifetime_us
#define END_US end_us
#define PMIC_OWNER 2u

/* Three 128 KiB DMA buffers: the plan and the loader's two (double buffering), and one for the
 * FAT reader's synchronous reads, which must not touch a buffer that a request or the sink holds. */
#define DMA_BUFFER (SUNXI_MMC_MAX_DMA_BLOCKS * 512u)
static uint8_t buffer[3 * DMA_BUFFER] __attribute__((aligned(4096)));
#define DESCRIPTORS ((volatile struct sunxi_mmc_dma_descriptor *)(uintptr_t)UINT64_C(0x42100000))
static StaticTask_t tasks[2];
static StackType_t stacks[2][4096];

enum { CONSTRUCT, IDENTIFY, CARD, STATUS, READ, POLL, TAKE, CAPTURE, FAT, REGION };
static uint32_t pf_ready[2];             /* GPIO_POW_VAL and GPIO_POW_VAL_SET_CTL when the card was identified */
static uint8_t status_copy[64];
static uint64_t status_us, status_result;
static struct {
    struct a733_counter_clock counter;
    struct a733_platform_locks locks;
    struct a733_clock_leases clocks;
    struct a733_pins_native pin_native;
    struct a733_pin_handoff handoff;
    struct a733_pins pins;
    struct a733_gpu_resource_native gpu_native;
    struct a733_gpu_resource_manager gpu;
    struct a733_rail_leases rails;
    struct a733_pmic_native pmic_native;
    struct a733_pmic_config pmic_config;
    struct a733_pmic pmic;
    struct a733_mmc_platform_native native;
    struct a733_mmc_platform_config config;
    struct a733_mmc_platform platform;
    struct sunxi_mmc_card card;
    struct sunxi_mmc_result result;
    struct sdp_input input;
    atomic_uint failed, pmic_stop, pmic_parked;
    uint64_t start, end, started, first_start, last_done, identified_us;
    unsigned stage, done, captured, reason, card_valid, clock_off, closed;
    uint32_t baseline[16];
} s;
static uint32_t elapsed[SDP_MAX_READS], crcs[SDP_MAX_READS];
static struct sdp_timing timing[SDP_TIMED];
static uint8_t sectors[SDP_MAX_CAPTURE][512];
static struct sdp_fat fat;
static struct sdp_region region;
static struct region_state region_state;
extern int platform_upper_mapped;
static uint8_t fat_scratch[256 * 512] __attribute__((aligned(64)));

/* Words that the lifetime must leave as it found them (the restoration guard of
 * apps/storage-read-returning/return.c, read at EL1 here): SDC0 module and bus/reset,
 * PF0..6 mux, drive and pull, the PF supply selection, S_TWI0 and PL0/1, APBS clocks. */
static const uintptr_t baseline_address[16] = {
    0x02002d00, 0x02002d0c, 0x02000300, 0x02000320, 0x02000330, 0x02000040, 0x02000048, 0x02000070,
    0x0701019c, 0x07025000, 0x07025014, 0x07025024, 0x0701000c, 0x07010010, 0x07083014, 0x0701019c};
static const uint32_t baseline_mask[16] = {
    0x87001f1f, 0x10001, 0x0fffffff, 0x0fffffff, 0x3fff, 3u << 10, 3u << 10, 1, 0x10001, 0xff, 0xff, 0xf,
    0x0700001f, 0x0700001f, 0x1ff, 0x10001};
static void sample(uint32_t out[16])
{
    for (unsigned i = 0; i < 16; ++i) out[i] = *(volatile uint32_t *)baseline_address[i] & baseline_mask[i];
}

static uint64_t now_us(void)
{
    uint64_t v;
    return a733_counter_clock_now(&s.counter, &v) == 1 ? v : UINT64_MAX;
}
static int live(void *unused) { (void)unused; return !atomic_load_explicit(&s.failed, memory_order_acquire) && now_us() < s.end; }
static void fail(void *unused) { (void)unused; atomic_store_explicit(&s.failed, 1, memory_order_release); }
static int enter(void *unused, uint64_t deadline)
{ (void)unused; return live(NULL) && a733_platform_lock_enter(&s.locks, A733_PLATFORM_REGISTER_LOCK, deadline); }
static void leave(void *unused) { (void)unused; a733_platform_lock_leave(&s.locks, A733_PLATFORM_REGISTER_LOCK); }
static int clock_now(void *unused, uint64_t *v) { (void)unused; return a733_counter_clock_now(&s.counter, v); }

static int memory(uintptr_t a, size_t bytes, uint8_t attributes, uint8_t shareability)
{
    if (!bytes || bytes > UINTPTR_MAX - a || shareability > 3) return 0;
    const struct a733_mapping_rule rule = {A733_MAPPING_WRITE, 1, {{attributes, (uint8_t)(1u << shareability)}}, 0, 1};
    return a733_mapping_check_pages(aarch64_device_translate, a, a, bytes, &rule);
}
static int mappings(void)
{
    static const uintptr_t words[] = {0x02002d00, 0x02002d0c, 0x02002500, 0x02002580, 0x02002588,
        0x020000f0, 0x02000344, 0x03400118, 0x03400218, 0x03400318};
    if (!memory(0x04020000, 0x304, 0, 2)) return 0;
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); ++i) if (!memory(words[i], 1, 0, 2)) return 0;
    return memory((uintptr_t)buffer, sizeof(buffer), 0xff, 3) &&
        memory((uintptr_t)DESCRIPTORS, SUNXI_MMC_DMA_DESCRIPTORS * sizeof(*DESCRIPTORS), 0x44, 2);
}
static int admit(void *cookie, const struct a733_mmc_platform_config *c, const struct a733_clock_token *t)
{
    return cookie == &s && live(NULL) && mappings() && !memcmp(c, &s.config, sizeof(*c)) &&
        t->owner == s.config.owner && a733_clock_token_status(&s.clocks, t) == A733_CLOCK_OK;
}
static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = UINT32_MAX;
    for (size_t i = 0; i < n; i += 8) {
        uint64_t v; memcpy(&v, p + i, 8);
        __asm__(".arch_extension crc\n crc32x %w0, %w0, %x1" : "+r"(c) : "r"(v));
    }
    return ~c;
}
static int dma_sync(void *cookie, uint8_t *b, size_t n, unsigned phase)
{
    (void)cookie;
    uintptr_t start = (uintptr_t)b, base = (uintptr_t)buffer;
    if ((phase != SUNXI_MMC_DMA_BEFORE && phase != SUNXI_MMC_DMA_AFTER) ||
        start < base || start > base + sizeof(buffer) || n > base + sizeof(buffer) - start) return 0;
    return aarch64_device_cache(phase == SUNXI_MMC_DMA_BEFORE ? AARCH64_CACHE_CLEAN : AARCH64_CACHE_INVALIDATE,
                                (uintptr_t)b, n) == 1;
}

/* The three CCU words the SD0 provider checks (the fabric), as apps/storage-read-returning
 * receives them from a FEL read: each under its mask, with the MBUS word's fields at the
 * ROM value replaced by boot0's, as the monitor's MBUS restore does. */
static int fabric(struct a733_mmc_platform_config *c)
{
    static const uintptr_t address[3] = {0x02002500, 0x02002580, 0x02002588};
    static const uint32_t mask[3] = {0x0300001f, 0x8700001f, 0x8700001f};
    static const uint32_t parent_mhz[3][8] = {{24, 0, 0, 600, 0, 0, 0, 0}, {24, 0, 800, 600, 480, 0, 0, 0},
                                              {24, 600, 0, 480, 400, 0, 0, 0}};
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t w = *(volatile uint32_t *)address[i];
        if (i == 2 && (w & A733_MBUS_FIELDS) == A733_MBUS_ROM) w = (w & ~A733_MBUS_FIELDS) | A733_MBUS_BOOT0;
        uint32_t v = w & mask[i], mhz = parent_mhz[i][(v >> 24) & 7], div = (v & 31) + 1;
        if (!mhz || (mhz * 1000000u) % div || (i && !(v >> 31))) return 0;
        c->fabric[i] = v; c->fabric_hz[i] = mhz * 1000000u / div;
    }
    return 1;
}

static int setup(void)
{
    const volatile struct sdp_input *in = &sunstone_probe_input;
    for (size_t i = 0; i < sizeof s.input; ++i) ((uint8_t *)&s.input)[i] = ((const volatile uint8_t *)in)[i];
    const struct sdp_input *c = &s.input;
    if (c->magic != SDP_INPUT_MAGIC || !c->reads || c->reads > SDP_MAX_READS || !c->blocks ||
        c->blocks > sizeof(buffer) / 512 || c->capture_count > SDP_MAX_CAPTURE) return 0;
    volatile const struct sunstone_probe_session *session = (const void *)(uintptr_t)SUNSTONE_PROBE_SESSION_ADDRESS;
    if (c->region_bytes && (c->region_flags & SDP_REGION_LOAD)) { plan_end_us = 70000000u; lifetime_us = 80000000u; end_us = 85000000u; }
    uint64_t generation = session->generation | 1, owner = session->epoch | 1, handoff = session->lease | 1;
    if (owner == PMIC_OWNER || !generation) return 0;
    atomic_init(&s.failed, 0); atomic_init(&s.pmic_stop, 0); atomic_init(&s.pmic_parked, 0);
    if (!a733_counter_clock_init(&s.counter, 24000000) ||
        !a733_platform_locks_init(&s.locks, a733_counter_clock_now, &s.counter) ||
        !a733_clock_leases_init(&s.clocks, a733_counter_clock_now, &s.counter)) return 0;
    s.start = now_us();
    if (s.start == UINT64_MAX || !a733_deadline_after(s.start, END_US, &s.end)) return 0;
    s.handoff.authorized[A733_PIN_L] = 3;      /* PL0/PL1: the PMIC's S_TWI0. */
    s.handoff.authorized[A733_PIN_F] = 0x7f;   /* PF0..PF6: SD0 and its card detect. */
    const struct a733_pins_locks pl = {enter, leave, clock_now, NULL};
    if (!a733_pins_native_init(&s.pin_native, &pl) ||
        !a733_pins_init_banks(&s.pins, &s.pin_native.operations, &s.pin_native, &s.handoff) ||
        !a733_gpu_resources_native_init(&s.gpu_native, &s.locks.callbacks)) return 0;
    s.gpu_native.operations.now = clock_now;
    if (!a733_gpu_resources_init(&s.gpu, &s.gpu_native.operations, &s.gpu_native)) return 0;
    const struct a733_rail_binding rb = {&s.clocks, &s.gpu, generation, a733_counter_clock_now, &s.counter};
    if (!a733_rail_leases_init(&s.rails, &rb)) return 0;
    s.pmic_config = (struct a733_pmic_config){.owner = PMIC_OWNER, .identity = generation,
        .startup_deadline = s.end, .pins = {{2, 1, 1}, {2, 1, 1}}};
    s.config = (struct a733_mmc_platform_config){.owner = owner, .handoff = handoff, .topology = generation ^ owner,
        .deadline_us = s.start + IDENTIFY_US, .lifetime_deadline_us = s.start + LIFETIME_US,
        .kind = SUNXI_MMC_SD, .voltage_mask = 0x00300000, .sd_profile = SUNXI_MMC_SD_HIGH_SPEED4, .dma = DESCRIPTORS,
        .observe_s18 = 1};          /* ACMD41 carries S18R and the card's S18A is kept in the OCR; CMD11 is never sent */
    if (!s.config.topology) return 0;
    if (c->uhs_mode > 2) return 0;
    if (c->uhs_mode) {              /* a UHS-I lifetime: the profile that moves the bank, SDR25 only, no S18A observation */
        s.config.sd_profile = SUNXI_MMC_SD_UHS_SDR104; s.config.observe_s18 = 0; s.config.sdr25_only = 1;
        s.config.start_1v8 = c->uhs_mode == 2;
    }
    /* Establish entry state, do not require it: the pins carry whatever the boot ROM and the runner left
     * (the provider accepts the passive and the ROM JTAG patterns). The inherited fields are read here,
     * recorded in the report's restoration words, and handed to the provider as the state to replace.
     * SD0 and its card detect then take function 2, pull-up, drive 1; PF6 stays an input. */
    for (unsigned i = 0; i < 7; ++i) {
        struct a733_pin_fields f;
        if (!a733_pin_fields_banked(A733_PIN_F, i, &f)) return 0;
        s.config.expected[i] = (struct a733_pin_config){
            (uint8_t)((*(volatile uint32_t *)f.mux_address >> f.mux_shift) & 15u),
            (uint8_t)((*(volatile uint32_t *)f.pull_address >> f.pull_shift) & 3u),
            (uint8_t)((*(volatile uint32_t *)f.drive_address >> f.drive_shift) & 15u)};
    }
    for (unsigned i = 0; i < 6; ++i) s.config.desired[i] = (struct a733_pin_config){2, 1, 1};
    s.config.desired[6] = (struct a733_pin_config){0, 1, 1};
    if (!fabric(&s.config)) return 0;
    a733_clock_mask_t scopes;
    return a733_mmc_platform_scopes(&s.config, &scopes);
}
static int bind(TaskHandle_t pmic)
{
    return pmic && a733_pmic_native_init(&s.pmic_native, &s.locks, pmic) &&
        a733_pmic_native_bind(&s.pmic, &s.pmic_config, &s.pmic_native, &s.clocks, &s.rails, &s.pins) &&
        a733_gpu_resources_native_use_pmic(&s.gpu_native, &s.gpu, &s.pmic);
}


/* Synchronous sector reads for the FAT32 reader: one request into the DMA buffer, polled to completion. */
static int fat_read(void *unused, uint64_t sector, uint32_t count, uint8_t *out)
{
    (void)unused;
    if (!count || count > SUNXI_MMC_MAX_DMA_BLOCKS) return 0;
    sunstone_probe_progress();
    enum a733_mmc_platform_result r;
    uint8_t *dma = buffer + 2 * DMA_BUFFER;
    do { r = a733_mmc_platform_read(&s.platform, sector, count, dma, count * 512u, now_us() + READ_US); }
    while (r == A733_MMC_BUSY && live(NULL));
    if (r != A733_MMC_PENDING) return 0;
    do { r = a733_mmc_platform_poll(&s.platform); } while ((r == A733_MMC_PENDING || r == A733_MMC_BUSY) && live(NULL));
    if (r != A733_MMC_COMPLETE) return 0;
    do { r = a733_mmc_platform_take(&s.platform, &s.result, s.config.lifetime_deadline_us); } while (r == A733_MMC_BUSY);
    if (r != A733_MMC_READY || s.result.error != SUNXI_MMC_OK || s.result.received != count * 512u) return 0;
    memcpy(out, dma, count * 512u);
    return 1;
}
/* The volume's free space and its root directory, read only. A failed read stops the probe; any
 * other refusal of the reader is a result and is reported. */
static struct fat32 f;
static int fat_mounted;
static int inspect_fat(void)
{
    struct fat32_device dev = {NULL, fat_read, s.card.sectors};
    fat.partition_first = s.input.fat_first; fat.partition_sectors = s.input.fat_sectors;
    fat.mount_status = fat32_mount(&f, &dev, s.input.fat_first, s.input.fat_sectors);
    if (fat.mount_status == FAT32_IO) return 0;
    if (fat.mount_status) return 1;
    fat_mounted = 1;
    fat.sectors_per_cluster = f.sectors_per_cluster; fat.reserved_sectors = f.reserved; fat.fats = f.fats;
    fat.fat_sectors = f.fat_sectors; fat.root_cluster = f.root_cluster; fat.clusters = f.clusters;
    fat.volume_id = f.volume_id; fat.hint_free = f.fsinfo_free; fat.hint_next = f.fsinfo_next;
    fat.data_start = f.data_start;
    fat.free_status = fat32_count_free(&f, fat_scratch, 256, &fat.counted_free);
    if (fat.free_status == FAT32_IO) return 0;
    struct fat32_dir d; struct fat32_entry e; int r;
    fat32_dir_open(&f, &d, f.root_cluster);
    while ((r = fat32_dir_next(&f, &d, &e)) == 1) {
        if (fat.entries_seen++ >= SDP_FAT_ENTRIES) continue;
        struct sdp_fat_entry *o = &fat.entry[fat.entry_count++];
        size_t n = strlen(e.name);
        o->truncated = n > 63; if (n > 63) n = 63;
        memcpy(o->name, e.name, n);
        o->size = e.size; o->first_cluster = e.first_cluster; o->attributes = e.attributes; o->long_name = e.long_name;
    }
    fat.list_status = r;
    return r != FAT32_IO;
}

/* The loader's device: one request in flight on the SD platform. */
static uint32_t async_blocks;
static int async_submit(void *unused, uint64_t sector, uint32_t count, uint8_t *dma)
{
    (void)unused;
    enum a733_mmc_platform_result r;
    do { r = a733_mmc_platform_read(&s.platform, sector, count, dma, count * 512u, now_us() + READ_US); }
    while (r == A733_MMC_BUSY && live(NULL));
    async_blocks = count;
    ++region.load_requests;
    return r == A733_MMC_PENDING;
}
static int async_poll(void *unused)
{
    (void)unused;
    enum a733_mmc_platform_result r = a733_mmc_platform_poll(&s.platform);
    if (r == A733_MMC_PENDING || r == A733_MMC_BUSY) return live(NULL) ? 0 : -1;
    if (r != A733_MMC_COMPLETE) return -1;
    do { r = a733_mmc_platform_take(&s.platform, &s.result, s.config.lifetime_deadline_us); } while (r == A733_MMC_BUSY);
    return r == A733_MMC_READY && s.result.error == SUNXI_MMC_OK && s.result.received == async_blocks * 512u ? 1 : -1;
}
static struct storage_digest load_digest;
static int load_consume(void *unused, const uint8_t *data, uint64_t bytes, uint64_t offset)
{
    (void)unused;
    uint64_t *dst = (uint64_t *)(uintptr_t)(region_state.base + offset);
    const uint64_t *src = (const uint64_t *)(const void *)data;
    for (uint64_t i = 0; i < bytes / 8; ++i) dst[i] = src[i];
    for (uint64_t i = bytes & ~7ull; i < bytes; ++i) ((uint8_t *)dst)[i] = data[i];
    storage_digest_add(&load_digest, data, bytes);
    sunstone_probe_progress();
    return 1;
}
/* Take the region, load the named file into it and exercise it. Returns 0 only when a device read failed. */
static int run_region(void)
{
    struct sdp_region *r = &region;
    memcpy(r->expected, s.input.file_sha256, 32);
    if (!platform_upper_mapped) { r->status = 20; return 1; }
    r->flags |= SDP_RT_UPPER_MAPPED;
    if (region_prepare(&region_state, r, s.input.region_bytes)) return 1;
    if (s.input.region_flags & SDP_REGION_LOAD) {
        struct fat32_entry e; struct fat32_file file;
        char name[48]; memcpy(name, s.input.file_name, 48); name[47] = 0;
        r->file_status = fat_mounted ? fat32_lookup(&f, name, &e) : FAT32_BAD_BOOT;
        if (r->file_status == FAT32_OK) r->file_status = fat32_open(&f, &e, &file);
        if (r->file_status == FAT32_NOT_FOUND) r->flags |= SDP_RT_FILE_ABSENT;
        else if (r->file_status == FAT32_IO) return 0;
        else if (r->file_status == FAT32_OK) {
            r->file_bytes = file.size;
            if (file.size > region_state.bytes) r->load_status = FAT32_INVALID;
            else {
                struct fat32_async a = {NULL, async_submit, async_poll};
                struct fat32_sink sink = {NULL, load_consume};
                storage_digest_init(&load_digest);
                uint64_t t0 = counter();
                r->load_status = fat32_stream(&f, &file, &a, buffer, buffer + DMA_BUFFER, SUNXI_MMC_MAX_DMA_BLOCKS, &sink);
                r->load_ticks = counter() - t0;
                if (r->load_status == FAT32_IO) return 0;
                if (r->load_status == FAT32_OK) {
                    r->flags |= SDP_RT_LOADED;
                    storage_digest_end(&load_digest, r->digest);
                    if (!memcmp(r->digest, r->expected, 32)) r->flags |= SDP_RT_DIGEST_OK;
                }
            }
        }
    }
    r->status = region_exercise(&region_state, r, 0);
    return 1;
}

static int stop(unsigned reason) { s.reason = reason; fail(NULL); return -1; }
/* One step of the SD task: 2 progressed, continue at once; 0 wait one tick; 1 done; -1 stopped. */
static int step(void)
{
    enum a733_mmc_platform_result r;
    if (!live(NULL)) return stop(1);
    switch (s.stage) {
    case CONSTRUCT:
        if (!a733_mmc_platform_native_init_selected(&s.native, &s.locks, xTaskGetCurrentTaskHandle(),
                SUNXI_MMC_SD, A733_MMC_EMMC_SDC2, admit, &s)) return stop(2);
        s.native.operations.dma_sync = dma_sync;
        if (!a733_mmc_platform_init(&s.platform, &s.config, &s.native.operations, &s.native,
                &s.clocks, &s.pins, &s.rails, &s.pmic)) return stop(2);
        s.stage = IDENTIFY; return 2;
    case IDENTIFY: {
        uint64_t wake = counter();
        for (;;) {
            r = a733_mmc_platform_poll(&s.platform);
            if (r == A733_MMC_READY) { s.stage = CARD; return 2; }
            if (r != A733_MMC_PENDING && r != A733_MMC_BUSY) return stop(3);
            if ((counter() - wake) / 24 >= 1000 || !live(NULL)) { sunstone_probe_progress(); return 0; }
        }
    }
    case CARD:
        r = a733_mmc_platform_card(&s.platform, &s.card, s.config.deadline_us);
        if (r == A733_MMC_BUSY) return 0;
        if (r != A733_MMC_READY) return stop(4);
        s.card_valid = 1; s.identified_us = now_us() - s.start; s.stage = STATUS;
        pf_ready[0] = *(volatile uint32_t *)0x02000048; pf_ready[1] = *(volatile uint32_t *)0x02000070; return 2;
    case STATUS: {
        /* ACMD13 (CMD55, then CMD13 with a 64-byte block): a read, in the read-only command set. */
        static uint8_t st[SUNXI_MMC_SD_STATUS_BYTES] __attribute__((aligned(64)));
        uint64_t t0 = counter();
        r = a733_mmc_platform_sd_status(&s.platform, st, now_us() + READ_US);
        if (r == A733_MMC_BUSY) return 0;
        if (r != A733_MMC_PENDING) return stop(16);
        for (;;) {
            r = a733_mmc_platform_poll(&s.platform);
            if (r == A733_MMC_COMPLETE) break;
            if (r != A733_MMC_PENDING && r != A733_MMC_BUSY) return stop(16);
        }
        do { r = a733_mmc_platform_take(&s.platform, &s.result, s.config.lifetime_deadline_us); } while (r == A733_MMC_BUSY);
        if (r != A733_MMC_READY || s.result.error != SUNXI_MMC_OK || s.result.received != SUNXI_MMC_SD_STATUS_BYTES) return stop(16);
        memcpy(status_copy, st, sizeof status_copy);
        status_us = (counter() - t0) / 24; status_result = 1;
        s.stage = READ; return 2;
    }
    case READ: {
        if (s.done == s.input.reads) { s.stage = CAPTURE; return 2; }
        uint64_t now = now_us();
        if (now >= s.start + PLAN_END_US) { s.stage = CAPTURE; return 2; }
        sunstone_probe_progress();
        uint64_t first = (uint64_t)s.input.first + (uint64_t)s.done * s.input.blocks;
        if (first + s.input.blocks > s.card.sectors) return stop(5);
        s.started = counter();
        if (!s.done) s.first_start = s.started;
        r = a733_mmc_platform_read(&s.platform, first, s.input.blocks, buffer, s.input.blocks * 512u, now + READ_US);
        if (r == A733_MMC_BUSY) return 0;
        if (r != A733_MMC_PENDING) return stop(5);
        s.stage = POLL; return 2;
    }
    case POLL:
        for (;;) {
            r = a733_mmc_platform_poll(&s.platform);
            if (r == A733_MMC_COMPLETE) {
                s.last_done = counter();
                uint64_t us = (s.last_done - s.started) / 24;
                elapsed[s.done] = us > UINT32_MAX ? UINT32_MAX : (uint32_t)us; s.stage = TAKE; return 2;
            }
            if (r != A733_MMC_PENDING && r != A733_MMC_BUSY) return stop(6);
        }
    case TAKE:
        r = a733_mmc_platform_take(&s.platform, &s.result, s.config.lifetime_deadline_us);
        if (r == A733_MMC_BUSY) return 0;
        if (r != A733_MMC_READY || s.result.error != SUNXI_MMC_OK || s.result.received != s.input.blocks * 512u) return stop(7);
        crcs[s.done] = crc32(buffer, s.input.blocks * 512u);
        if (s.done < SDP_TIMED)
            timing[s.done] = (struct sdp_timing){s.result.command_us, s.result.first_byte_us, s.result.done_us,
                s.result.clean_us, s.result.invalidate_us, s.result.polls};
        s.done++; s.stage = READ; return 2;
    case CAPTURE: {
        /* One sector at a time, outside the timed plan; the card layout. */
        if (s.captured == s.input.capture_count) { s.stage = FAT; return 2; }
        uint64_t now = now_us();
        uint32_t sector = s.input.capture[s.captured];
        if (sector >= s.card.sectors) return stop(8);
        r = a733_mmc_platform_read(&s.platform, sector, 1, buffer, 512, now + READ_US);
        if (r == A733_MMC_BUSY) return 0;
        if (r != A733_MMC_PENDING) return stop(8);
        for (;;) {
            r = a733_mmc_platform_poll(&s.platform);
            if (r == A733_MMC_COMPLETE) break;
            if (r != A733_MMC_PENDING && r != A733_MMC_BUSY) return stop(8);
        }
        do { r = a733_mmc_platform_take(&s.platform, &s.result, s.config.lifetime_deadline_us); } while (r == A733_MMC_BUSY);
        if (r != A733_MMC_READY || s.result.error != SUNXI_MMC_OK || s.result.received != 512) return stop(8);
        memcpy(sectors[s.captured], buffer, 512);
        s.captured++; return 2;
    }
    case FAT:
        if (!s.input.fat_sectors) return 1;
        if (!inspect_fat()) return stop(14);
        if (!s.input.region_bytes) return 1;
        s.stage = REGION; return 2;
    case REGION:
        return run_region() ? 1 : stop(15);
    default: return stop(9);
    }
}

static int quiet(void)
{
    const struct a733_pmic *p = &s.pmic;
    if (!atomic_load_explicit(&s.pmic_parked, memory_order_acquire) || p->held || p->retained || p->failed ||
        p->active != A733_PMIC_REQUESTS || atomic_load_explicit(&s.locks.retained, memory_order_acquire)) return 0;
    for (unsigned i = 0; i < A733_PMIC_REQUESTS; ++i) if (p->slots[i].state || p->slots[i].borrow.id) return 0;
    return !xSemaphoreGetMutexHolder(s.locks.mutex[0]) && !xSemaphoreGetMutexHolder(s.locks.mutex[1]);
}
static int close_card(void)
{
    if (!s.done || s.done != s.input.reads || !quiet() || atomic_load_explicit(&s.failed, memory_order_acquire)) return 0;
    enum a733_mmc_platform_result r;
    do {
        if (now_us() >= s.config.lifetime_deadline_us) return 0;
        r = a733_mmc_platform_close(&s.platform, s.config.lifetime_deadline_us);
        if (r == A733_MMC_BUSY || r == A733_MMC_PENDING) vTaskDelay(1);
    } while (r == A733_MMC_BUSY || r == A733_MMC_PENDING);
    if (r != A733_MMC_CLOSED) return 0;
    s.clock_off = 1; return 1;
}

static void report(int outcome)
{
    volatile struct sdp_report *o = REPORT;
    memset((void *)o, 0, sizeof *o);
    o->magic = SDP_REPORT_MAGIC; o->version = 2;
    o->stage = s.stage; o->reason = outcome ? s.reason : 0; o->done = s.done; o->planned = s.input.reads;
    o->identified_us = s.identified_us; o->elapsed_us = now_us() - s.start;
    if (s.done) { o->read_window_ticks = s.last_done - s.first_start; o->bytes = (uint64_t)s.done * s.input.blocks * 512u; }
    const struct sunxi_mmc_card *c = &s.card;
    if (s.card_valid) {
        for (unsigned i = 0; i < 4; ++i) { o->cid[i] = c->cid[i]; o->csd[i] = c->csd[i]; }
        o->ocr = c->ocr; o->rca = c->rca; o->sectors = c->sectors;
        for (unsigned i = 0; i < 8; ++i) o->scr[i / 4] |= (uint64_t)c->scr[i] << (8 * (i % 4));
        uint32_t divisor = (c->module_divisor & 0x7fu) | ((c->module_divisor & SUNXI_MMC_MODULE_300M) ? 0x80u : 0u);
        o->mode = (uint64_t)s.config.sd_profile | ((uint64_t)c->bus_width << 8) | ((uint64_t)c->timing << 16) |
            ((uint64_t)divisor << 24);
        o->card_hz = c->card_hz;
    }
    for (unsigned i = 0; i < 16; ++i) o->baseline_start[i] = s.baseline[i];
    uint32_t end[16]; sample(end);
    for (unsigned i = 0; i < 16; ++i) o->baseline_end[i] = end[i];
    o->clock_off = s.clock_off; o->pmic_parked = atomic_load_explicit(&s.pmic_parked, memory_order_acquire);
    o->capture_done = s.captured;
    for (unsigned i = 0; i < s.done; ++i) { o->elapsed[i] = elapsed[i]; o->crc[i] = crcs[i]; }
    for (unsigned i = 0; i < SDP_TIMED; ++i) o->timing[i] = timing[i];
    memcpy((void *)o->sector, sectors, sizeof sectors);
    memcpy((void *)&o->fat, &fat, sizeof fat);
    memcpy((void *)&o->region, &region, sizeof region);
    memcpy((void *)o->sd_status, status_copy, sizeof status_copy);
    if (s.card_valid) memcpy((void *)o->switch_status, c->switch_status, sizeof o->switch_status);
    o->uhs[0] = s.input.uhs_mode; o->uhs[1] = s.card_valid ? c->signal_1v8 : 0; o->uhs[2] = s.card_valid ? c->uhs_outcome : 0;
    o->uhs[3] = s.card_valid ? c->reentered : 0; o->uhs[4] = pf_ready[0]; o->uhs[5] = pf_ready[1];
    o->uhs[6] = *(volatile uint32_t *)0x02000048; o->uhs[7] = *(volatile uint32_t *)0x02000070;
    o->status_result = status_result; o->status_us = status_us;
    if (status_result) { o->au_sectors = sunxi_mmc_au_sectors(status_copy, 0); o->au_sectors_uhs = sunxi_mmc_au_sectors(status_copy, 1); }
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Called by the probe at completion, before the watchdog is retired: the card is closed
 * and the words that restoration must return equal their values at start. */
uint64_t sunstone_probe_quiesce(void)
{
    /* A failure before the SD platform exists leaves every word unchanged and may return its report. */
    if (!s.closed && s.platform.initialized) return 0;
    uint32_t now[16]; sample(now);
    for (unsigned i = 0; i < 16; ++i) if (now[i] != s.baseline[i]) return 0;
    return SUNSTONE_PROBE_READY;
}

static void pmic_task(void *unused)
{
    (void)unused;
    for (;;) {
        if (atomic_load_explicit(&s.pmic_stop, memory_order_acquire)) {
            atomic_store_explicit(&s.pmic_parked, 1, memory_order_release); for (;;) vTaskDelay(1);
        }
        enum a733_pmic_status r = a733_pmic_poll(&s.pmic);
        if (r != A733_PMIC_READY && r != A733_PMIC_COMPLETE && r != A733_PMIC_PENDING && r != A733_PMIC_BUSY) fail(NULL);
        vTaskDelay(1);
    }
}
static void sd_task(void *unused)
{
    (void)unused;
    int r;
    for (;;) {
        r = step();
        if (r == 2) continue;
        if (r) break;
        vTaskDelay(1);
    }
    if (r < 0 && s.platform.initialized) (void)a733_mmc_platform_retain(&s.platform);
    atomic_store_explicit(&s.pmic_stop, 1, memory_order_release);
    while (!atomic_load_explicit(&s.pmic_parked, memory_order_acquire) && now_us() < s.end) vTaskDelay(1);
    if (r > 0 && !quiet()) { s.reason = 10; r = -1; }
    if (r > 0 && !close_card()) { s.reason = 11; r = -1; }
    s.closed = r > 0;
    report(r > 0 ? 0 : 1);
    /* With the platform constructed and not closed, the lifetime cannot return. The fault record then
     * carries the stop: the line field is the reason, the detail is platform fault, platform stage, task
     * stage and completed reads. */
    if (r <= 0 && s.platform.initialized)
        platform_panic((const char *)(uintptr_t)(((uint64_t)s.platform.fault << 24) | ((uint64_t)s.platform.stage << 16) |
                       ((uint64_t)s.stage << 8) | (s.done & 255)), s.reason);
    platform_exit(r > 0 ? 0 : 1);
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name) { (void)task; platform_panic(name, 0); }
void kernel_main(void)
{
    platform_init();
    sample(s.baseline);
    if (!setup()) { s.reason = 12; report(1); platform_exit(1); }
    TaskHandle_t pmic = xTaskCreateStatic(pmic_task, "sd-pmic", 4096, NULL, 2, stacks[0], &tasks[0]);
    TaskHandle_t sd = xTaskCreateStatic(sd_task, "sd-read", 4096, NULL, 3, stacks[1], &tasks[1]);
    if (!pmic || !sd || !bind(pmic)) { s.reason = 13; report(1); platform_exit(1); }
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
