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
#include <stdatomic.h>
#include <string.h>

#define IDENTIFY_US 5000000u
#define LIFETIME_US 180000000u
#define READ_US 500000u
#define DMA_BUFFER (SUNXI_MMC_MAX_DMA_BLOCKS * 512u)

/* True when the image declares large regions: the PF bank must then be authorized at boot. */
extern const struct ss_large_region __start_sunstone_large_regions[] __attribute__((weak));
extern const struct ss_large_region __stop_sunstone_large_regions[] __attribute__((weak));
int os_large_sd_declared(void)
{ return __start_sunstone_large_regions && __stop_sunstone_large_regions > __start_sunstone_large_regions; }
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
    uintptr_t start = (uintptr_t)b, base = (uintptr_t)buffers;
    if ((phase != SUNXI_MMC_DMA_BEFORE && phase != SUNXI_MMC_DMA_AFTER) || start < base || start > base + sizeof buffers ||
        n > base + sizeof buffers - start) return 0;
    return aarch64_device_cache(phase == SUNXI_MMC_DMA_BEFORE ? AARCH64_CACHE_CLEAN : AARCH64_CACHE_INVALIDATE, (uintptr_t)b, n) == 1;
}

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

static int configure(void)
{
    const struct a733_board_session *s = &board.session;
    uint64_t owner = (s->lease ^ UINT64_C(0x5344303052454144)) | 1, generation = s->generation | 1;
    /* A distinct owner identity: not the PMIC's, the USB owners' or the platform's. */
    while (owner == s->pmic_owner || owner == s->usb0_owner || owner == s->typec_owner || owner == s->usb2_owner) owner += 2;
    uint64_t start = now_us();
    if (start == UINT64_MAX) return 0;
    config = (struct a733_mmc_platform_config){.owner = owner, .handoff = s->lease | 1, .topology = generation ^ owner,
        .deadline_us = start + IDENTIFY_US, .lifetime_deadline_us = start + LIFETIME_US, .kind = SUNXI_MMC_SD,
        .voltage_mask = 0x00300000, .sd_profile = SUNXI_MMC_SD_HIGH_SPEED4, .dma = descriptors};
    if (!config.topology || !fabric()) return 0;
    /* The pins carry whatever the ROM and the runner left; the provider replaces that state. */
    for (unsigned i = 0; i < 7; ++i) {
        struct a733_pin_fields f;
        if (!a733_pin_fields_banked(A733_PIN_F, i, &f)) return 0;
        config.expected[i] = (struct a733_pin_config){(uint8_t)((*(volatile uint32_t *)f.mux_address >> f.mux_shift) & 15u),
            (uint8_t)((*(volatile uint32_t *)f.pull_address >> f.pull_shift) & 3u),
            (uint8_t)((*(volatile uint32_t *)f.drive_address >> f.drive_shift) & 15u)};
    }
    for (unsigned i = 0; i < 6; ++i) config.desired[i] = (struct a733_pin_config){2, 1, 1};
    config.desired[6] = (struct a733_pin_config){0, 1, 1};
    a733_clock_mask_t scopes;
    return a733_mmc_platform_scopes(&config, &scopes);
}

/* One request at a time into dma, to completion or failure; the loader's metadata reads. */
static int read_blocking(uint64_t sector, uint32_t count, uint8_t *dma)
{
    enum a733_mmc_platform_result r;
    do { r = a733_mmc_platform_read(&platform, sector, count, dma, count * 512u, now_us() + READ_US); }
    while (r == A733_MMC_BUSY && board_live(NULL));
    if (r != A733_MMC_PENDING) return 0;
    do { r = a733_mmc_platform_poll(&platform); } while ((r == A733_MMC_PENDING || r == A733_MMC_BUSY) && board_live(NULL));
    if (r != A733_MMC_COMPLETE) return 0;
    do { r = a733_mmc_platform_take(&platform, &result, config.lifetime_deadline_us); } while (r == A733_MMC_BUSY);
    return r == A733_MMC_READY && result.error == SUNXI_MMC_OK && result.received == count * 512u;
}
static int sd_read(void *unused, uint64_t sector, uint32_t count, uint8_t *out)
{
    (void)unused;
    if (!count || count > SUNXI_MMC_MAX_DMA_BLOCKS || sector + count > card.sectors) return 0;
    uint8_t *dma = buffers + 2 * DMA_BUFFER;
    if (!read_blocking(sector, count, dma)) return 0;
    memcpy(out, dma, (size_t)count * 512u);
    return 1;
}
static int sd_submit(void *unused, uint64_t sector, uint32_t count, uint8_t *dma)
{
    (void)unused;
    if (!count || count > SUNXI_MMC_MAX_DMA_BLOCKS || sector + count > card.sectors) return 0;
    enum a733_mmc_platform_result r;
    do { r = a733_mmc_platform_read(&platform, sector, count, dma, count * 512u, now_us() + READ_US); }
    while (r == A733_MMC_BUSY && board_live(NULL));
    blocks_in_flight = count;
    return r == A733_MMC_PENDING;
}
static int sd_poll(void *unused)
{
    (void)unused;
    enum a733_mmc_platform_result r = a733_mmc_platform_poll(&platform);
    if (r == A733_MMC_PENDING || r == A733_MMC_BUSY) return board_live(NULL) ? 0 : -1;
    if (r != A733_MMC_COMPLETE) return -1;
    do { r = a733_mmc_platform_take(&platform, &result, config.lifetime_deadline_us); } while (r == A733_MMC_BUSY);
    return r == A733_MMC_READY && result.error == SUNXI_MMC_OK && result.received == blocks_in_flight * 512u ? 1 : -1;
}

/* Runs in the loader task, which becomes the platform's owner task. */
const struct os_large_device *os_large_device(void)
{
    if (!os_large_sd_declared() || opened) return 0;
    /* The PMIC service must be up before the SD rail can be leased. */
    for (unsigned waited = 0; !atomic_load_explicit(&providers.shared.pmic_ready, memory_order_acquire); ++waited) {
        if (waited > 10000 || !board_live(NULL)) return 0;
        vTaskDelay(1);
    }
    struct device_memory_pool *pool = a733_memory_uncached_init();
    struct device_memory_extent extent;
    if (!pool || device_memory_acquire(pool, UINT64_C(0x53443044), 4096, &descriptor_lease, &extent) != 1) return 0;
    descriptors = (volatile struct sunxi_mmc_dma_descriptor *)extent.cpu;
    if (!configure()) return 0;
    if (!a733_mmc_platform_native_init_selected(&native, &providers.shared.locks, xTaskGetCurrentTaskHandle(), SUNXI_MMC_SD,
            A733_MMC_EMMC_SDC2, admit, &platform)) return 0;
    native.operations.dma_sync = dma_sync;
    if (!a733_mmc_platform_init(&platform, &config, &native.operations, &native, &providers.shared.clocks,
            &providers.shared.pins, &providers.shared.rails, &providers.shared.pmic)) return 0;
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
            return 0;
        }
        vTaskDelay(1);
    }
    enum a733_mmc_platform_result r;
    do { r = a733_mmc_platform_card(&platform, &card, config.deadline_us); if (r == A733_MMC_BUSY) vTaskDelay(1); }
    while (r == A733_MMC_BUSY && board_live(NULL));
    if (r != A733_MMC_READY || !card.sectors) { (void)a733_mmc_platform_retain(&platform); return 0; }
    device = (struct os_large_device){{NULL, sd_read, card.sectors}, {NULL, sd_submit, sd_poll}, 0, 0, buffers};
    return &device;
}

/* Restore the clock, the pins and the rail when the last region has loaded. */
void os_large_device_close(void)
{
    if (!opened) return;
    enum a733_mmc_platform_result r;
    do {
        r = a733_mmc_platform_close(&platform, config.lifetime_deadline_us);
        if (r == A733_MMC_BUSY || r == A733_MMC_PENDING) vTaskDelay(1);
    } while ((r == A733_MMC_BUSY || r == A733_MMC_PENDING) && now_us() < config.lifetime_deadline_us);
    if (r != A733_MMC_CLOSED) (void)a733_mmc_platform_retain(&platform);
    opened = 0;
    if (descriptor_lease.pool) (void)device_memory_release(a733_memory_uncached_init(), descriptor_lease);
}
