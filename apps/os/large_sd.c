/* The read-only SD0 binding of the OS image for the large-region loader (docs/designs/large-app-regions.md).
 *
 * It uses the OS's own shared providers (providers.shared: counter, locks, clock leases, pins, rails, the PMIC that
 * the board-pmic task serves) and the read-only build of the SD core (SUNXI_MMC_READ_ONLY: no write or erase command
 * is reachable, checks/sd-read-only.py). The loader task owns the platform: os_large_device() runs in it, identifies
 * the card in profile 2 (four-bit high speed, 3.3 V) and returns the device; os_large_device_close() restores the
 * clock, pins and rail leases when the last region has loaded. A failure of any step retains the platform (the
 * provider keeps ownership of what it touched) and returns no device, so every region fails as unloadable and the
 * application is told.
 *
 * The same sequence runs as apps/sd-read-probe on a board, where it identified the card, read it at 21.9 MB/s per
 * request and restored every register; this file differs in using the OS's providers and in running as the
 * loader task. Not yet run in an OS image on a board. */
#include "FreeRTOS.h"
#include "task.h"
#include "app_large.h"
#include "../board/application.h"
#include "a733_mmc_platform_native.h"
#include "a733_mapping_check.h"
#include "a733_memory_uncached.h"
#include "a733_el3_mbus.h"
#include "aarch64_device_memory.h"
#include "device_memory_pool.h"
#include "sunxi_smhc.h"
#include "sd_note.h"
#include "quiesce.h"
#include "cpu-return.h"
#include "sunstone_crumb.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#ifdef OS_SD_WRITE
#include "sd_hw.h"
#include "gicv3_spi.h"
#include "a733_storage_bounds.h"
#include "storage_wait.h"
#include "sunstone_deadline.h"
/* A write-capable image: the read binding below is the shared setup of both images. sd_owner.c runs the one owner task,
 * so the two entry points get other names here and sd_owner.c supplies os_large_device() as a proxy for the loader. */
#endif

#define IDENTIFY_US 5000000u
#define LIFETIME_US 180000000u
#define READ_US 500000u
#define DMA_BUFFER (SUNXI_MMC_MAX_DMA_BLOCKS * 512u)

/* True when the image declares a large region that the card loads: the PF bank must then be authorized at boot. */
extern const struct ss_large_region __start_sunstone_large_regions[] __attribute__((weak));
extern const struct ss_large_region __stop_sunstone_large_regions[] __attribute__((weak));
int os_large_sd_declared(void)
{
#ifdef OS_SD_WRITE
    return 1;                           /* a write-capable image always uses the card */
#else
    if (!__start_sunstone_large_regions) return 0;
    for (const struct ss_large_region *r = __start_sunstone_large_regions; r < __stop_sunstone_large_regions; ++r)
        if (!(r->flags & SS_LARGE_HEAP)) return 1;      /* the heap alone reads nothing from the card */
    return 0;
#endif
}
/* PF0 to PF6 are SD0 and its card detect (apps/sd-read-probe/main.c). */
int os_large_sd_pins(struct a733_pin_handoff *pins)
{
    if (os_large_sd_declared()) pins->authorized[A733_PIN_F] |= UINT32_C(0x7f);
    return 1;
}

static uint8_t buffers[3 * DMA_BUFFER] __attribute__((aligned(4096)));
static struct a733_mmc_platform_native native;
static struct a733_mmc_platform_config config;
static struct a733_mmc_platform platform;
static struct sunxi_mmc_card card;
static struct sunxi_mmc_result result;
static volatile struct sunxi_mmc_dma_descriptor *descriptors;
static struct device_memory_lease descriptor_lease;
static struct os_large_device device;
static unsigned blocks_in_flight;
static int opened;
static atomic_uint close_proven;
#ifndef OS_SD_WRITE
enum { SD_UNOPENED, SD_OPENING, SD_OPEN, SD_CLOSED, SD_FAILED };
static atomic_uint storage_state;
static enum os_quiesce_result storage_quiesce_step(struct os_quiesce *q)
{
    unsigned state=SD_UNOPENED;
    if(atomic_compare_exchange_strong_explicit(&storage_state,&state,SD_CLOSED,memory_order_acq_rel,memory_order_acquire))
        return OS_QUIESCE_DONE;
    if(state==SD_CLOSED)return OS_QUIESCE_DONE;
    if(state==SD_FAILED){
        (void)os_quiesce_fault(q,SUNSTONE_CRUMB_FAIL_BOARD<<12|0x911,0,SUNSTONE_CRUMB_HERE());
        return OS_QUIESCE_FAULT;
    }
    return os_quiesce_result(q);             /* the loader finishes its current read and closes */
}
static struct os_quiesce storage_quiesce={.owner=OS_QUIESCE_STORAGE,
    .order=OS_QUIESCE_DEVICES,.step=storage_quiesce_step};
int os_sd_quiesce_register(void)
{return !os_large_sd_declared()||os_quiesce_register(&storage_quiesce);}
#endif
#ifdef OS_SD_WRITE
static uint8_t write_buffer[OS_SD_WRITE_BYTES] __attribute__((aligned(4096)));
static struct a733_storage_bounds bounds;
static uint8_t *write_region; static size_t write_region_bytes;
#endif

static uint64_t now_us(void)
{
    uint64_t v;
    return a733_counter_clock_now(&providers.shared.counter, &v) == 1 ? v : UINT64_MAX;
}
static int memory_ok(uintptr_t a, size_t bytes, uint8_t attributes, uint8_t shareability)
{
    if (!bytes || bytes > UINTPTR_MAX - a || shareability > 3) return 0;
    const struct a733_mapping_rule rule = {A733_MAPPING_WRITE, 1, {{attributes, (uint8_t)(1u << shareability)}}, 0, 1};
    return a733_mapping_check_pages(aarch64_device_translate, a, a, bytes, &rule);
}
static int mappings(void)
{
    static const uintptr_t words[] = {0x02002d00, 0x02002d0c, 0x02002500, 0x02002580, 0x02002588,
        0x020000f0, 0x02000344, 0x03400118, 0x03400218, 0x03400318};
    if (!memory_ok(0x04020000, 0x304, 0, 2)) return 0;
    for (unsigned i = 0; i < sizeof words / sizeof words[0]; ++i) if (!memory_ok(words[i], 1, 0, 2)) return 0;
    return memory_ok((uintptr_t)buffers, sizeof buffers, 0xff, 3) &&
#ifdef OS_SD_WRITE
        memory_ok((uintptr_t)write_buffer, sizeof write_buffer, 0xff, 3) &&
#endif
        memory_ok((uintptr_t)descriptors, SUNXI_MMC_DMA_DESCRIPTORS * sizeof *descriptors, 0x44, 2);
}
static int admit(void *cookie, const struct a733_mmc_platform_config *c, const struct a733_clock_token *t)
{
    return cookie == &platform && board_live(NULL) && mappings() && !memcmp(c, &config, sizeof *c) &&
        t->owner == config.owner && a733_clock_token_status(&providers.shared.clocks, t) == A733_CLOCK_OK;
}
static int dma_sync(void *cookie, uint8_t *b, size_t n, unsigned phase)
{
    (void)cookie;
    uintptr_t start = (uintptr_t)b, base = (uintptr_t)buffers, size = sizeof buffers;
#ifdef OS_SD_WRITE
    if (start >= (uintptr_t)write_buffer && start <= (uintptr_t)write_buffer + sizeof write_buffer) {
        base = (uintptr_t)write_buffer; size = sizeof write_buffer;
    } else if (write_region && start >= (uintptr_t)write_region && start <= (uintptr_t)write_region + write_region_bytes) {
        base = (uintptr_t)write_region; size = write_region_bytes;
    }
#endif
    if ((phase != SUNXI_MMC_DMA_BEFORE && phase != SUNXI_MMC_DMA_AFTER) || start < base || start > base + size ||
        n > base + size - start) return 0;
    return aarch64_device_cache(phase == SUNXI_MMC_DMA_BEFORE ? AARCH64_CACHE_CLEAN : AARCH64_CACHE_INVALIDATE, (uintptr_t)b, n) == 1;
}

/* Where an open or an enable stopped, for the application's report (ss_storage_diagnostic): [0] the stage, [1] a detail, [2] the first
 * failed bounds read (sector + 1), [3] the bounds reads made. For an open that stopped, [2] is INTID 193's state before the owner masked it
 * (bit 0 enabled, 1 pending, 2 active; stray handler entries << 8) and [3] its state after the mask settled. Stages: 1 declared or already open, 2 PMIC service, 3 descriptor memory,
 * 4 configure, 5 native binding, 6 platform init, 7 identification (detail: result), 8 card query (result), 9 interrupt and timer
 * registration or irq arming (detail 1 platform, 2 or 3 GIC rearm), 10 owner task creation (sd_owner.c), 11 owner wait (sd_owner.c), 12 GIC mask before the baseline, 37 to 42 the recovery of a failed read (37 restart refused, 38 identification again, 39 card query, 40 interrupt re-armed) or the re-opening of a card found at 1.8 V (41 its read, 42 its close), 30 to 36 a refused read, write or status request (detail: result | platform stage << 8 | platform fault << 16 | core error << 24), 20 enable preconditions, 21 blob copy, 22 blob parse, 23 bounds verify (detail: object state), 24 write binding. */
uint64_t os_sd_diag[4];
static inline int diag_set(uint64_t stage, uint64_t detail)
{
    if (!os_sd_diag[0]) { os_sd_diag[0] = stage; os_sd_diag[1] = detail; }     /* the first stop is the one that matters */
    return 0;
}
#define DIAG(stage, detail) diag_set((stage), (uint64_t)(detail))
/* A platform result with where the platform stood: result | stage << 8 | fault << 16. */
#define PLAT(r) ((uint64_t)(r) | (uint64_t)platform.stage << 8 | (uint64_t)platform.fault << 16)
/* For a refused request (stages 30 to 36): result in bits 2:0, the reason timely() refused in 5:3, the source line of the platform's fault in
 * 16:6, the first refused register (SMHC offset / 4 + 1, 0xff elsewhere, 0 none) in 24:17, and the fault flag in 25. */
#define PLATX(r) ((uint64_t)(r) & 7u | (uint64_t)(platform.fault_why & 7u) << 3 | (uint64_t)(platform.fault_line & 0x7ffu) << 6 | \
                  (uint64_t)(platform.refused & 0xffu) << 17 | (uint64_t)(platform.fault ? 1u : 0u) << 25)
static const struct os_large_device *open_refused(unsigned stage,uint64_t detail)
{
    (void)DIAG(stage,detail);
#ifndef OS_SD_WRITE
    atomic_store_explicit(&storage_state,SD_FAILED,memory_order_release);
    (void)os_quiesce_fault(&storage_quiesce,SUNSTONE_CRUMB_FAIL_BOARD<<12|0x912,
        stage|((uint32_t)detail<<8),SUNSTONE_CRUMB_HERE());
#endif
    return NULL;
}
#define DIAGP(stage, detail) open_refused((stage),(uint64_t)(detail))
/* The three CCU words that the provider checks, and the rates they imply (apps/sd-read-probe/main.c). */
static int fabric(void)
{
    static const uintptr_t address[3] = {0x02002500, 0x02002580, 0x02002588};
    static const uint32_t mask[3] = {0x0300001f, 0x8700001f, 0x8700001f};
    static const uint32_t parent_mhz[3][8] = {{24, 0, 0, 600, 0, 0, 0, 0}, {24, 0, 800, 600, 480, 0, 0, 0}, {24, 600, 0, 480, 400, 0, 0, 0}};
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t w = *(volatile uint32_t *)address[i];
        if (i == 2 && (w & A733_MBUS_FIELDS) == A733_MBUS_ROM) w = (w & ~A733_MBUS_FIELDS) | A733_MBUS_BOOT0;
        uint32_t v = w & mask[i], mhz = parent_mhz[i][(v >> 24) & 7], div = (v & 31) + 1;
        if (!mhz || (mhz * 1000000u) % div || (i && !(v >> 31))) return 0;
        config.fabric[i] = v; config.fabric_hz[i] = mhz * 1000000u / div;
    }
    return 1;
}

static int configure(unsigned start18)
{
    const struct a733_board_session *s = &board.session;
    uint64_t owner = (s->lease ^ UINT64_C(0x5344303052454144)) | 1, generation = s->generation | 1;
    /* A distinct owner identity: not the PMIC's, the USB owners' or the platform's. */
    while (owner == s->pmic_owner || owner == s->usb0_owner || owner == s->typec_owner || owner == s->usb2_owner) owner += 2;
    uint64_t start = now_us();
    if (start == UINT64_MAX) return 0;
    config = (struct a733_mmc_platform_config){.owner = owner, .handoff = s->lease | 1, .topology = generation ^ owner,
        .deadline_us = start + IDENTIFY_US, .lifetime_deadline_us = start + LIFETIME_US, .kind = SUNXI_MMC_SD,
        .voltage_mask = 0x00300000, .sd_profile = SUNXI_MMC_SD_UHS_SDR104, .start_1v8 = (uint8_t)start18, .dma = descriptors};
    if (!config.topology || !fabric()) return 0;
    /* The pins carry whatever the ROM and the runner left; the provider replaces that state. */
    for (unsigned i = 0; i < 7; ++i) {
        struct a733_pin_fields f;
        if (!a733_pin_fields_banked(A733_PIN_F, i, &f)) return 0;
        config.expected[i] = (struct a733_pin_config){(uint8_t)((*(volatile uint32_t *)f.mux_address >> f.mux_shift) & 15u),
            (uint8_t)((*(volatile uint32_t *)f.pull_address >> f.pull_shift) & 3u),
            (uint8_t)((*(volatile uint32_t *)f.drive_address >> f.drive_shift) & 15u)};
    }
    /* 20 mA (code 1) on all six pins. The board kernel's strengths (sun60iw2p1.dtsi sdc0_pins_a/b and _f/_g: the clock at 30 mA, command and data at 10 mA) were tried on 3-3 and did
     * not change the write fault (docs/sd-card.md); the 3.3 V high-speed writes that pass on 3-1 were made with these. */
    for (unsigned i = 0; i < 6; ++i) config.desired[i] = (struct a733_pin_config){2, 1, 1};
    config.desired[6] = (struct a733_pin_config){0, 1, 1};
    a733_clock_mask_t scopes;
    return a733_mmc_platform_scopes(&config, &scopes);
}

/* The bus speed mode the card runs and every step the owner took, for the application's report (ss_storage_mode). [0] bit 0 valid, 7:4 the card timing
 * (3 SDR104, 2 SDR50, 1 SDR25 or high speed, 0 SDR12 or default speed), 8 the card signals 1.8 V, 15:12 the UHS-I outcome (sunxi_mmc_uhs_outcome), 23:20 the mode
 * steps taken (all, including the restarts of the recovery), 31:28 the ceiling, 32 the card was re-entered at 1.8 V without CMD11, 36 the owner opened the platform
 * a second time because the card was found at 1.8 V, 47:40 the recovery restarts; [1] the card clock in Hz; [2] bits 7:0 the sample delay, 15:8 the window start,
 * 23:16 its width, 31:24 the group 1 support bits, 32 S18A; [3] the first four mode log entries, one byte each (from | to << 2 | reason << 4); [4] the first two faults the recovery acted on (os_sd_fault_events: error, protocol step, interrupt status). */
uint64_t os_sd_mode[5];
static unsigned reopened, restarts, busy_total;
static void record_mode(void)
{
    const struct sunxi_mmc_card *c = &platform.core.card;
    uint64_t log = 0;
    for (unsigned i = 0; i < 4 && i < c->mode_steps && i < SUNXI_MMC_MODE_LOG; ++i)
        log |= (uint64_t)((c->mode_log[i].from & 3u) | (c->mode_log[i].to & 3u) << 2 | (c->mode_log[i].reason & 15u) << 4) << (8 * i);
    os_sd_mode[1] = c->card_hz;
    os_sd_mode[2] = (uint64_t)c->sample_delay | (uint64_t)c->tuning_start << 8 | (uint64_t)c->tuning_width << 16 | (uint64_t)c->group1_support << 24 |
                    (uint64_t)(c->ocr >> 24 & 1u) << 32 | os_sd_busy_field(busy_total);
    os_sd_mode[3] = log | os_sd_fault_blocks(c);
    os_sd_mode[4] = os_sd_fault_events(c);
    os_sd_mode[0] = 1u | (uint64_t)(c->timing & 3u) << 4 | (uint64_t)(c->signal_1v8 ? 1u : 0u) << 8 | (uint64_t)(c->uhs_outcome & 15u) << 12 |
                    (uint64_t)(c->mode_steps & 15u) << 20 | (uint64_t)(c->ceiling & 15u) << 28 | (uint64_t)(c->reentered ? 1u : 0u) << 32 |
                    (uint64_t)(reopened & 1u) << 36 | (uint64_t)(restarts & 255u) << 40;
}
/* The rule of docs/sd-card.md for a card that was identified at 3.3 V: S18A clear with SDR50 or SDR104 in the group 1 support field (bits 2 and 3
 * of status byte 13) is a card that already signals 1.8 V, as the 3.3 V host in front of 3-3's card measured; a card without UHS-I reports functions 2 to 4 as 0. */
static int found_at_1v8(void)
{
    return sunxi_mmc_card_found_at_1v8(&platform.core.card);
}
#define RECOVER_US 3000000u
static unsigned saved_aux; static uint64_t saved_where;   /* where the last write fault stopped, kept across the restart that discards the core's result */
static uint64_t first_fault_detail; static unsigned first_fault_piece, first_fault_idst;       /* the core's error, protocol step and interrupt status at the first failed request (apps/os/sd_note.h), kept for the failure record */
static uint64_t last_sector; static uint32_t last_count; static uint8_t *last_dma;
#ifdef OS_SD_WRITE
static void irq_suspend(void);
static int irq_resume(void);
#endif
/* A recovery that did not complete (stages 37 to 40) leaves the fault that began it: os_sd_diag[1] is the core's error, protocol step and interrupt status at the first failed
 * request, and os_sd_diag[2] the platform's result in 2:0, its fault flag and the rest as sd_note.h os_sd_recover_aux lays them out, for the record's aux field. */
static int recover_fail(unsigned stage, enum a733_mmc_platform_result r)
{
    int rc = DIAG(stage, first_fault_detail);
    os_sd_diag[2] = os_sd_recover_aux((unsigned)r, platform.fault ? 1u : 0u, (board_live_refusal() >> 28) & 7u, now_us() >= config.lifetime_deadline_us ? 1u : 0u,
                                      first_fault_piece, first_fault_idst, r != A733_MMC_PENDING && r != A733_MMC_BUSY);
    return rc;
}
/* A failed read the platform can recover (A733_MMC_ERROR): identify again, which tunes again, or step the mode down (a733_mmc_platform_recover). Blocking, in the owner. */
static int recover_blocking(void)
{
    enum a733_mmc_platform_result r;
    if (!first_fault_detail) { first_fault_detail = os_sd_first_fault(&platform.core); first_fault_piece = sunxi_mmc_piece_class(platform.core.write_request.blocks); first_fault_idst = sunxi_mmc_idst4(platform.core.result.dma_status); }
    saved_where = os_sd_where_detail(&platform.core, platform.core.card.timing); saved_aux = (unsigned)os_sd_where_aux(&platform.core);
#ifdef OS_SD_WRITE
    irq_suspend();
#endif
    do { r = a733_mmc_platform_recover(&platform, now_us() + RECOVER_US); if (r == A733_MMC_BUSY) vTaskDelay(1); } while (r == A733_MMC_BUSY && board_live(NULL));
    if (r != A733_MMC_PENDING) return recover_fail(37, r);
    ++restarts;
    for (;;) {
        uint64_t wake = now_us();
        for (;;) {
            r = a733_mmc_platform_poll(&platform);
            if (r != A733_MMC_PENDING && r != A733_MMC_BUSY) break;
            if (now_us() - wake >= 1000 || !board_live(NULL)) break;
        }
        if (r == A733_MMC_READY) break;
        if ((r != A733_MMC_PENDING && r != A733_MMC_BUSY) || !board_live(NULL) || now_us() >= config.lifetime_deadline_us) return recover_fail(38, r);
        vTaskDelay(1);
    }
    do { r = a733_mmc_platform_card(&platform, &card, config.lifetime_deadline_us); if (r == A733_MMC_BUSY) vTaskDelay(1); } while (r == A733_MMC_BUSY && board_live(NULL));
    if (r != A733_MMC_READY) return recover_fail(39, r);
    record_mode();
#ifdef OS_SD_WRITE
    if (!irq_resume()) return recover_fail(40, r);
#endif
    return 1;
}
/* One request at a time into dma, to completion or failure; the loader's metadata reads. */
#ifdef OS_SD_WRITE
static enum a733_mmc_platform_result drive_platform(uint64_t deadline, uint32_t recheck_us);
static int sd_irq_mask_settled(void);
static uint32_t recheck_for(uint32_t blocks, int write);
#endif
static int read_blocking(uint64_t sector, uint32_t count, uint8_t *dma)
{
    enum a733_mmc_platform_result r;
    for (unsigned attempt = 0;; ++attempt) {
        do { r = a733_mmc_platform_read(&platform, sector, count, dma, count * 512u, now_us() + READ_US); }
        while (r == A733_MMC_BUSY && board_live(NULL));
        if (r != A733_MMC_PENDING) return DIAG(30, PLATX(r));
#ifdef OS_SD_WRITE
        r = drive_platform(now_us() + READ_US, recheck_for(count, 0));
#else
        do { r = a733_mmc_platform_poll(&platform); } while ((r == A733_MMC_PENDING || r == A733_MMC_BUSY) && board_live(NULL));
#endif
        if (r == A733_MMC_ERROR && attempt < A733_MMC_RECOVERIES && recover_blocking()) continue;      /* identify again or step down, then the same request again */
        break;
    }
    if (r != A733_MMC_COMPLETE) return DIAG(31, PLATX(r));
    do { r = a733_mmc_platform_take(&platform, &result, config.lifetime_deadline_us); } while (r == A733_MMC_BUSY);
    if (r == A733_MMC_READY && result.error == SUNXI_MMC_OK && result.received == count * 512u) return 1;
    return DIAG(32, PLATX(r));
}
static int sd_read(void *unused, uint64_t sector, uint32_t count, uint8_t *out)
{
    (void)unused;
#ifndef OS_SD_WRITE
    if(os_cpu_return_requested())return 0;
#endif
    if (!count || count > SUNXI_MMC_MAX_DMA_BLOCKS || sector + count > card.sectors) return 0;
    uint8_t *dma = buffers + 2 * DMA_BUFFER;
    if (!read_blocking(sector, count, dma)) return 0;
    memcpy(out, dma, (size_t)count * 512u);
    return 1;
}
static int sd_submit(void *unused, uint64_t sector, uint32_t count, uint8_t *dma)
{
    (void)unused;
#ifndef OS_SD_WRITE
    if(os_cpu_return_requested())return 0;
#endif
    if (!count || count > SUNXI_MMC_MAX_DMA_BLOCKS || sector + count > card.sectors) return 0;
    enum a733_mmc_platform_result r;
    do { r = a733_mmc_platform_read(&platform, sector, count, dma, count * 512u, now_us() + READ_US); }
    while (r == A733_MMC_BUSY && board_live(NULL));
    blocks_in_flight = count; last_sector = sector; last_count = count; last_dma = dma;
    if (r != A733_MMC_PENDING) return 0;
#ifndef OS_SD_WRITE
    /* The request is accepted but the command is issued by the polls that follow, and the loader hashes the previous piece
     * before it polls: without these steps the card would sit idle for that whole time. Step the controller until the
     * command is issued (command_step 2 is the wait for it to finish), so that the data moves while the caller works. A
     * step that fails or completes is seen again by sd_poll. */
    for (unsigned steps = 0; steps < 64 && platform.core.command_step < 2; ++steps) {
        r = a733_mmc_platform_poll(&platform);
        if (r != A733_MMC_PENDING && r != A733_MMC_BUSY) break;
    }
#endif
    return 1;
}
static int sd_poll(void *unused)
{
    (void)unused;
    enum a733_mmc_platform_result r = a733_mmc_platform_poll(&platform);
    if (r == A733_MMC_PENDING || r == A733_MMC_BUSY) return board_live(NULL) ? 0 : -1;
    if (r == A733_MMC_ERROR) {
        /* A failed read: identify again (or step down) and submit the same request again; the caller keeps waiting. */
        if (!recover_blocking()) return -1;
        do { r = a733_mmc_platform_read(&platform, last_sector, last_count, last_dma, last_count * 512u, now_us() + READ_US); }
        while (r == A733_MMC_BUSY && board_live(NULL));
        return r == A733_MMC_PENDING ? 0 : -1;
    }
    if (r != A733_MMC_COMPLETE) return -1;
    do { r = a733_mmc_platform_take(&platform, &result, config.lifetime_deadline_us); } while (r == A733_MMC_BUSY);
    return r == A733_MMC_READY && result.error == SUNXI_MMC_OK && result.received == blocks_in_flight * 512u ? 1 : -1;
}

/* Runs in the loader task, which becomes the platform's owner task. */
#ifdef OS_SD_WRITE
const struct os_large_device *os_sd_open_owner_device(void)
#else
const struct os_large_device *os_large_device(void)
#endif
{
#ifndef OS_SD_WRITE
    unsigned state=SD_UNOPENED;
    if(os_cpu_return_requested()||!atomic_compare_exchange_strong_explicit(&storage_state,&state,SD_OPENING,memory_order_acq_rel,memory_order_acquire))return NULL;
#endif
    if (!os_large_sd_declared() || opened) return DIAGP(1, opened);
    /* The PMIC service must be up before the SD rail can be leased. */
    for (unsigned waited = 0; !atomic_load_explicit(&providers.shared.pmic_ready, memory_order_acquire); ++waited) {
        if (waited > 10000 || !board_live(NULL)) return DIAGP(2, waited);
        vTaskDelay(1);
    }
    struct device_memory_pool *pool = a733_memory_uncached_init();
    struct device_memory_extent extent;
    if (!pool || device_memory_acquire(pool, UINT64_C(0x53443044), 4096, &descriptor_lease, &extent) != 1) return DIAGP(3, 0);
    descriptors = (volatile struct sunxi_mmc_dma_descriptor *)extent.cpu;
#ifdef OS_SD_WRITE
    if (!sd_irq_mask_settled()) return DIAGP(12, 0);        /* the route table enabled INTID 193 at boot; the platform's baseline wants it masked */
#endif
    for (unsigned start18 = 0;; start18 = 1) {
        if (!configure(start18)) return DIAGP(4, 0);
        if (!a733_mmc_platform_native_init_selected(&native, &providers.shared.locks, xTaskGetCurrentTaskHandle(), SUNXI_MMC_SD,
                A733_MMC_EMMC_SDC2, admit, &platform)) return DIAGP(5, 0);
        native.operations.dma_sync = dma_sync;
        if (!a733_mmc_platform_init(&platform, &config, &native.operations, &native, &providers.shared.clocks,
                &providers.shared.pins, &providers.shared.rails, &providers.shared.pmic)) return DIAGP(6, 0);
        opened = 1;
        /* Identification polls for up to a millisecond per wake and yields so that the PMIC task can answer. */
        for (;;) {
            uint64_t wake = now_us();
            enum a733_mmc_platform_result r;
            for (;;) {
                r = a733_mmc_platform_poll(&platform);
                if (r != A733_MMC_PENDING && r != A733_MMC_BUSY) break;
                if (now_us() - wake >= 1000 || !board_live(NULL)) break;
            }
            if (r == A733_MMC_READY) break;
            if ((r != A733_MMC_PENDING && r != A733_MMC_BUSY) || !board_live(NULL) || now_us() >= config.deadline_us) {
                (void)a733_mmc_platform_retain(&platform);
                return DIAGP(7, PLAT(r));
            }
            vTaskDelay(1);
        }
        enum a733_mmc_platform_result r;
        do { r = a733_mmc_platform_card(&platform, &card, config.deadline_us); if (r == A733_MMC_BUSY) vTaskDelay(1); }
        while (r == A733_MMC_BUSY && board_live(NULL));
        if (r != A733_MMC_READY || !card.sectors) { (void)a733_mmc_platform_retain(&platform); return DIAGP(8, PLAT(r)); }
        if (start18 || !found_at_1v8()) break;
        /* The card was identified at 3.3 V but already signals 1.8 V (the rule above): one read so that the platform may close, close it with the bank at 3.3 V,
         * and open it again with the bank moved to 1.8 V first and the card re-entered with no CMD11, the lifetime proven on 3-3. */
        static uint8_t scratch[512];
        if (!sd_read(NULL, 0, 1, scratch)) { (void)a733_mmc_platform_retain(&platform); return DIAGP(41, 0); }
        enum a733_mmc_platform_result c;
        do { c = a733_mmc_platform_close(&platform, config.lifetime_deadline_us); if (c == A733_MMC_BUSY || c == A733_MMC_PENDING) vTaskDelay(1); }
        while ((c == A733_MMC_BUSY || c == A733_MMC_PENDING) && now_us() < config.lifetime_deadline_us);
        if (c != A733_MMC_CLOSED) { (void)a733_mmc_platform_retain(&platform); return DIAGP(42, PLAT(c)); }
        memset(&platform, 0, sizeof platform); memset(&native, 0, sizeof native); memset(&card, 0, sizeof card);
        opened = 0; blocks_in_flight = 0; reopened = 1;
    }
    record_mode();
#ifndef OS_SD_WRITE
    atomic_store_explicit(&storage_state,SD_OPEN,memory_order_release);
#endif
    device = (struct os_large_device){{NULL, sd_read, card.sectors}, {NULL, sd_submit, sd_poll}, 0, 0, buffers};
    return &device;
}

/* Restore the clock, the pins and the rail when the last region has loaded. */
#ifdef OS_SD_WRITE
void os_sd_close_owner_device(void)
#else
void os_large_device_close(void)
#endif
{
    if (!opened) return;
    enum a733_mmc_platform_result r;
    do {
        r = a733_mmc_platform_close(&platform, config.lifetime_deadline_us);
        if (r == A733_MMC_BUSY || r == A733_MMC_PENDING) vTaskDelay(1);
    } while ((r == A733_MMC_BUSY || r == A733_MMC_PENDING) && now_us() < config.lifetime_deadline_us);
    if (r != A733_MMC_CLOSED) {
        (void)a733_mmc_platform_retain(&platform);
#ifndef OS_SD_WRITE
        atomic_store_explicit(&storage_state,SD_FAILED,memory_order_release);
        (void)os_quiesce_fault(&storage_quiesce,SUNSTONE_CRUMB_FAIL_BOARD<<12|0x913,
            (uint32_t)PLATX(r),SUNSTONE_CRUMB_HERE());
#endif
        return;                             /* uncertain DMA retains its descriptors */
    }
    if (descriptor_lease.pool && device_memory_release(a733_memory_uncached_init(), descriptor_lease)!=1) {
#ifndef OS_SD_WRITE
        atomic_store_explicit(&storage_state,SD_FAILED,memory_order_release);
        (void)os_quiesce_fault(&storage_quiesce,SUNSTONE_CRUMB_FAIL_BOARD<<12|0x914,
            0,SUNSTONE_CRUMB_HERE());
#endif
        return;
    }
    descriptor_lease=(struct device_memory_lease){0};opened=0;
    atomic_store_explicit(&close_proven,1,memory_order_release);
#ifndef OS_SD_WRITE
    atomic_store_explicit(&storage_state,SD_CLOSED,memory_order_release);
    os_quiesce_complete(&storage_quiesce);
#endif
}

#ifdef OS_SD_WRITE
#define WRITE_US 2000000u
#define SMHC0 UINT32_C(0x04020000)
static uint8_t *blob_copy;                       /* sized from the input at enable time, kept for the image's lifetime */

/* --- completion by interrupt ------------------------------------------------------------------------------------------ */
/* SMHC0 is GIC SPI 161, INTID 193, level high (board kernel sun60iw2p1.dtsi, sdc0). The route is emitted with the image's
 * routes (tools/sunstone-manifest.py). The handler follows the board kernel's sunxi_mmc_irq (sunxi-mmc.c:1271): read the
 * masked status, accumulate it, write the same bits back to the raw status to acknowledge, wake the waiter. The core arms
 * the mask per command, so nothing asserts while the card is idle; IDST stays with the core (the IDMAC interrupts are off). */
static TaskHandle_t owner_task;
static _Atomic uint32_t irq_bits;
static volatile int irq_live;
static _Atomic uint32_t irq_entry_count[8], irq_stray_count;
/* SMHC0 is GIC SPI 161 (INTID 193). The platform core wants it masked from the open on (a733_mmc_platform.c: masked()), so the owner masks
 * it before the platform's baseline check and unmasks it only once irq completion is on; the route table enables it at boot. */
static const struct gicv3_spi sd_gic = {0x03400000u, 193u};
void os_sd_irq(void)
{
    atomic_fetch_add_explicit(&irq_entry_count[portGET_CORE_ID() & 7u], 1, memory_order_relaxed);
    if (!irq_live) {                                       /* not ours to serve yet: mask the source, never leave a level line unserved */
        atomic_fetch_add_explicit(&irq_stray_count, 1, memory_order_relaxed);
        (void)gicv3_spi_mask(&sd_gic);
        return;
    }
    uint32_t mista = *(volatile uint32_t *)(uintptr_t)(SMHC0 + 0x34);
    if (!mista) return;
    atomic_fetch_or_explicit(&irq_bits, mista, memory_order_release);
    *(volatile uint32_t *)(uintptr_t)(SMHC0 + 0x38) = mista;
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(owner_task, &woken);
    portYIELD_FROM_ISR(woken);
}
/* Mask INTID 193 in the distributor and wait, bounded, until it reports masked. */
/* INTID 193 as the distributor holds it: bit 0 enabled, bit 1 pending, bit 2 active (words 6 of GICD_ISENABLER, ISPENDR, ISACTIVER). */
static uint32_t sd_irq_state(void)
{
    const uintptr_t gicd = 0x03400000u;
    uint32_t e = *(volatile uint32_t *)(gicd + 0x118), p = *(volatile uint32_t *)(gicd + 0x218), a = *(volatile uint32_t *)(gicd + 0x318);
    return (e >> 1 & 1u) | (p >> 1 & 1u) << 1 | (a >> 1 & 1u) << 2;
}
static int sd_irq_mask_settled(void)
{
    if (!os_sd_diag[0]) os_sd_diag[2] = sd_irq_state() | (uint64_t)atomic_load_explicit(&irq_stray_count, memory_order_relaxed) << 8;     /* before the mask */
    if (!gicv3_spi_mask(&sd_gic)) return 0;
    for (unsigned tries = 0; tries < 100; ++tries) {
        int r = gicv3_spi_masked(&sd_gic);
        if (r) {
            /* A level interrupt that fired while the source was unserved leaves its pending bit set; the platform wants none it cannot
             * explain. With the source masked the owner may clear a stale one (GICD_ICPENDR, word 6); a line still asserted stays pending
             * and the platform's baseline then refuses, which the note shows. */
            if (r > 0 && (sd_irq_state() & 2u)) *(volatile uint32_t *)(0x03400000u + 0x298) = 2u;
            if (!os_sd_diag[0]) os_sd_diag[3] = sd_irq_state();                                 /* after the mask has settled */
            return r > 0;
        }
        vTaskDelay(1);
    }
    return 0;
}
static uint32_t irq_taken(void *unused) { (void)unused; return atomic_exchange_explicit(&irq_bits, 0, memory_order_acquire); }
static int irq_owner_on;       /* the owner has switched on completion by interrupt (os_sd_open_owner) */
/* A restart needs the interrupt masked and the platform out of irq mode; both are put back after it. */
static void irq_suspend(void)
{
    if (!irq_owner_on) return;
    irq_live = 0; (void)sd_irq_mask_settled(); (void)a733_mmc_platform_irq(&platform, 0);
}
static int irq_resume(void)
{
    if (!irq_owner_on) return 1;
    if (!a733_mmc_platform_irq(&platform, 1)) return 0;
    irq_live = 1;
    for (unsigned tries = 0; tries < 100; ++tries) {
        int r = gicv3_spi_rearm(&sd_gic);
        if (r < 0) { irq_live = 0; return 0; }
        if (r) return 1;
        vTaskDelay(1);
    }
    irq_live = 0; return 0;
}

/* The wait loop: poll, then sleep on the interrupt or a one-shot timer, as the core's hint says. */
struct drive { enum a733_mmc_platform_result r; int (*poll)(void); };
static int dr_poll(void *c)
{
    struct drive *d = c;
    if (d->poll) return d->poll();
    d->r = a733_mmc_platform_poll(&platform);
    return d->r != A733_MMC_PENDING && d->r != A733_MMC_BUSY;
}
/* How long an interrupt wait may sleep before it re-polls: twice the expected completion, with a 20 ms floor (storage_wait.h). The expected completion is the
 * data at a conservative 10 MB/s (the bus measured 21.7 MB/s for reads and 14.7 MB/s for writes), plus for a write the card's busy allowance of 250 ms
 * (SD Physical Layer 4.6.2.2). A recheck that fires is a miss and is counted. */
static uint32_t recheck_for(uint32_t blocks, int write)
{
    uint64_t us = (uint64_t)blocks * 512u * 1000000u / 10000000u + (write ? 250000u : 0u);
    return (uint32_t)(2 * us);
}
static volatile enum sunxi_mmc_wait last_hint;
static _Atomic uint64_t recheck_count;
static enum sunxi_mmc_wait dr_hint(void *c, uint32_t *us) { (void)c; return last_hint = sunxi_mmc_wait_hint(&platform.core, us); }
static uint64_t dr_now(void *c) { (void)c; return now_us(); }
static _Atomic uint64_t sleep_us, sleep_count;
static void dr_sleep(void *c, uint64_t until)
{
    (void)c;
    uint64_t from = now_us();
    int woke = sunstone_deadline_wait_until(until);
    if (last_hint == SUNXI_MMC_WAIT_IRQ && woke != SUNSTONE_DEADLINE_NOTIFIED) atomic_fetch_add(&recheck_count, 1);   /* the timer ended an interrupt wait */
    atomic_fetch_add(&sleep_us, now_us() - from);
    atomic_fetch_add(&sleep_count, 1);
}
static uint64_t hw_owner_sleep_us(void) { return atomic_load(&sleep_us); }
static uint64_t hw_owner_wakes(void) { return atomic_load(&sleep_count); }
static void hw_irq_counts(uint32_t entries[8], uint32_t *stray, uint32_t *recheck)
{
    *recheck = (uint32_t)atomic_load(&recheck_count);
    for (unsigned c = 0; c < 8; ++c) entries[c] = atomic_load(&irq_entry_count[c]);
    *stray = atomic_load(&irq_stray_count);
}
static int dr_live(void *c) { (void)c; return board_live(NULL); }
static enum a733_mmc_platform_result drive_platform(uint64_t deadline, uint32_t recheck_us)
{
    struct drive d = {A733_MMC_PENDING, NULL};
    const struct storage_wait_ops ops = {&d, dr_poll, dr_hint, dr_now, dr_sleep, dr_live, recheck_us};
    (void)storage_wait_run(&ops, deadline);
    return d.r;
}
static int async_value;
static int async_poll(void) { async_value = sd_poll(NULL); return async_value != 0; }
static int hw_wait_async(void)
{
    struct drive d = {A733_MMC_PENDING, async_poll};
    const struct storage_wait_ops ops = {&d, dr_poll, dr_hint, dr_now, dr_sleep, dr_live, recheck_for(blocks_in_flight, 0)};
    async_value = 0;
    (void)storage_wait_run(&ops, now_us() + READ_US);
    return async_value == 1;
}

static int hw_live(void) { return board_live(NULL); }
/* ACMD13 in the owner task, as a read: complete with poll and take like any other request. The unit is read from AU_SIZE
 * when the card is on the 3.3 V bus modes (this image runs profile 2: high speed, 3.3 V, so no UHS-I mode), and from
 * UHS_AU_SIZE when it signals at 1.8 V, which is the condition for a UHS-I bus speed mode (SD Physical Layer 9.10,
 * section 4.10.2.9). */
static int hw_allocation_unit(uint32_t *sectors)
{
    static uint8_t status[SUNXI_MMC_SD_STATUS_BYTES] __attribute__((aligned(64)));
    enum a733_mmc_platform_result r;
    do { r = a733_mmc_platform_sd_status(&platform, status, now_us() + READ_US); } while (r == A733_MMC_BUSY && board_live(NULL));
    if (r != A733_MMC_PENDING) return 0;
    r = drive_platform(now_us() + READ_US, recheck_for(1, 0));
    if (r != A733_MMC_COMPLETE) return DIAG(35, PLATX(r));
    do { r = a733_mmc_platform_take(&platform, &result, config.lifetime_deadline_us); } while (r == A733_MMC_BUSY);
    if (r != A733_MMC_READY || result.error != SUNXI_MMC_OK || result.received != SUNXI_MMC_SD_STATUS_BYTES) return DIAG(36, PLATX(r));
    *sectors = sunxi_mmc_au_sectors(status, platform.core.card.signal_1v8 != 0);
    return 1;
}
static int bounds_read(void *unused, uint64_t sector, uint8_t *out)
{
    (void)unused;
    ++os_sd_diag[3];
    int ok = sd_read(NULL, sector, 1, out);
    if (!ok && !os_sd_diag[2]) os_sd_diag[2] = sector + 1;                /* the first read that failed (sector + 1) */
    return ok;
}
static int hw_enable_writes(const uint8_t *blob, size_t bytes)
{
    /* All kernel data is writable at EL0 in this OS, so no mapping protects the blob. The runner's input is copied once into
     * private memory and parsed there; the bounds object re-hashes the copy on every use (a733_storage_bounds_allows). */
    if (!blob || !bytes || blob_copy || !card.sector_addressed) return DIAG(20, card.sector_addressed);
    blob_copy = malloc(bytes);                       /* the OS heap; the only refusal is that it has no room */
    if (!blob_copy) return DIAG(21, bytes);
    memcpy(blob_copy, blob, bytes);
    if (!a733_storage_bounds_parse(&bounds, blob_copy, bytes)) return DIAG(22, bytes);
    os_sd_diag[3] = 0;                               /* count the verify reads */
    if (!a733_storage_bounds_verify(&bounds, bounds_read, NULL)) return DIAG(23, bounds.state);
    if (!a733_storage_bounds_card_is(&bounds, card.cid)) return DIAG(25, card.cid[3]);          /* the bounds were made for another card: nothing is writable */
    if (!a733_mmc_platform_native_bind_bounds(&native, &platform, &bounds)) return DIAG(24, 0);
    return 1;
}
/* The application's flush buffer: the FAT32 service flushes from here straight into the controller (one crossing of
 * memory per byte). It must be cacheable DRAM that the controller can reach, 64-byte aligned, and apart from the image's
 * own buffers. */
static int hw_set_write_region(uint8_t *base, size_t bytes)
{
    uintptr_t a = (uintptr_t)base, e = a + bytes;
    if (write_region || !base || (a & 63u) || bytes < 4096 || (bytes & 511u) || e < a || a < 0x42200000u || e > 0x48000000u) return 0;
    uintptr_t own[3][2] = {{(uintptr_t)buffers, sizeof buffers}, {(uintptr_t)write_buffer, sizeof write_buffer},
                           {(uintptr_t)descriptors, SUNXI_MMC_DMA_DESCRIPTORS * sizeof *descriptors}};
    for (unsigned i = 0; i < 3; ++i) if (a < own[i][0] + own[i][1] && own[i][0] < e) return 0;
    if (!memory_ok(a, bytes, 0xff, 3)) return 0;
    write_region = base; write_region_bytes = bytes;
    return 1;
}
static int in_region(const uint8_t *p, size_t bytes)
{
    uintptr_t a = (uintptr_t)p;
    return write_region && a >= (uintptr_t)write_region && a - (uintptr_t)write_region <= write_region_bytes && bytes <= write_region_bytes - (a - (uintptr_t)write_region);
}
/* Where a write stopped, for the failure record that survives the watchdog reset (the report does not): stage 33 or 34, the platform fields as for a refused request,
 * and what the report would have said. os_sd_diag[1] carries the core's error (3:0), its protocol step (9:4) and the low 16 bits of the controller's raw interrupt
 * status at the fault (25:10); os_sd_diag[2] the pieces written so far (12:0) and the bus timing in force (14:13; 3 SDR104, 2 SDR50, 1 SDR25 or high speed, 0 SDR12 or
 * default) for the record's 15-bit aux field (sd_owner.c); os_sd_diag[3] the failing piece's first sector. os_sd_write_pieces and os_sd_write_sectors count the pieces and
 * sectors that completed. The mode log (os_sd_mode) holds every step the recovery took. */
uint64_t os_sd_write_pieces, os_sd_write_sectors;
static int write_fail(unsigned stage, enum a733_mmc_platform_result r, uint64_t sector, uint32_t count)
{
    (void)count;
    const struct sunxi_mmc *c = &platform.core;
    int live = c->state == SUNXI_MMC_FAULT;       /* else the restart has replaced the core: the snapshot taken before it */
    int rc = DIAG(stage, live ? os_sd_where_detail(c, c->card.timing) : saved_where);
    if (os_sd_diag[0] == stage) os_sd_diag[2] = live ? os_sd_where_aux(c) : (uint64_t)saved_aux;     /* the first stop's record keeps its own aux: a recovery that failed wrote it, and this call runs after it */
    if (!os_sd_diag[3]) os_sd_diag[3] = sector;
    (void)r;
    return rc;
}
static int hw_write_piece(uint64_t sector, uint32_t count, const uint8_t *src)
{
    if (!count || count > OS_SD_WRITE_PIECE_SECTORS) return 0;
    size_t bytes = (size_t)count * 512u;
    const uint8_t *dma = src;
    if (!in_region(src, bytes) || ((uintptr_t)src & 63u)) { memcpy(write_buffer, src, bytes); dma = write_buffer; }   /* metadata and small pieces */
    struct sunxi_mmc_write_authority authority;
    a733_storage_bounds_authority(&bounds, &authority);
    enum a733_mmc_platform_result r;
    /* A recoverable error (a controller, card or timeout error, a CRC status error or a busy timeout among them) identifies the card again, which tunes again, and on
     * the second one in a row steps the mode down (a733_mmc_platform_recover); the same piece, the same sectors and data, is then written again, inside the same bounds. */
    for (unsigned attempt = 0;; ++attempt) {
        do { r = a733_mmc_platform_write_dma(&platform, &authority, A733_BOUNDED_WRITE_TOKEN, sector, count, dma, now_us() + WRITE_US); }
        while (r == A733_MMC_BUSY && board_live(NULL));
        if (r != A733_MMC_PENDING) return write_fail(33, r, sector, count);
        r = drive_platform(now_us() + WRITE_US, recheck_for(count, 1));
        if (r == A733_MMC_ERROR && attempt < A733_MMC_RECOVERIES && recover_blocking()) continue;
        break;
    }
    if (r != A733_MMC_COMPLETE) return write_fail(34, r, sector, count);
    do { r = a733_mmc_platform_take(&platform, &result, config.lifetime_deadline_us); } while (r == A733_MMC_BUSY);
    int ok = r == A733_MMC_READY && result.error == SUNXI_MMC_OK && result.confirmed_blocks == count;
    if (ok) { ++os_sd_write_pieces; os_sd_write_sectors += count; }
    if (result.busy_timeouts) { busy_total += result.busy_timeouts; os_sd_mode[2] = (os_sd_mode[2] & ~(UINT64_C(255) << 40)) | os_sd_busy_field(busy_total); }
    return ok;
}
void os_sd_close_owner(void);
static int hw_closed(void){return atomic_load_explicit(&close_proven,memory_order_acquire)!=0;}
static struct os_sd_hw hw;
const struct os_sd_hw *os_sd_open_owner(void)
{
    const struct os_large_device *d = os_sd_open_owner_device();
    if (!d) return 0;
    /* The owner task sleeps on the controller interrupt and on one-shot timers (sunstone_deadline), never on the tick. */
    owner_task = xTaskGetCurrentTaskHandle();
    if (sunstone_deadline_register() != 1 || !sunxi_mmc_set_irq(&platform.core, irq_taken, NULL)) { (void)DIAG(9, 0); return 0; }
    if (!a733_mmc_platform_irq(&platform, 1)) { (void)DIAG(9, 1); return 0; }
    irq_live = 1; irq_owner_on = 1;
    int armed = 0;
    for (unsigned tries = 0; tries < 100 && !armed; ++tries) {
        int r = gicv3_spi_rearm(&sd_gic);
        if (r < 0) { irq_live = 0; (void)DIAG(9, 2); return 0; }
        if (r) armed = 1; else vTaskDelay(1);
    }
    if (!armed) { irq_live = 0; (void)DIAG(9, 3); return 0; }
    hw = (struct os_sd_hw){d, card.sectors, card.sector_addressed, hw_allocation_unit, hw_enable_writes, hw_write_piece, hw_live,
                           hw_set_write_region, hw_wait_async, hw_owner_sleep_us, hw_owner_wakes, hw_irq_counts, os_sd_close_owner, hw_closed};
    return &hw;
}
void os_sd_close_owner(void)
{
    irq_live = 0; irq_owner_on = 0;
    if(!sd_irq_mask_settled()||!a733_mmc_platform_irq(&platform,0))return;
    os_sd_close_owner_device();
}
#endif
