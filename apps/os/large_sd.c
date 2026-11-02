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
#include <stdlib.h>
#include <string.h>
#ifdef OS_SD_WRITE
#include "sd_hw.h"
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

/* True when the image declares large regions: the PF bank must then be authorized at boot. */
extern const struct ss_large_region __start_sunstone_large_regions[] __attribute__((weak));
extern const struct ss_large_region __stop_sunstone_large_regions[] __attribute__((weak));
int os_large_sd_declared(void)
{
#ifdef OS_SD_WRITE
    return 1;                           /* a write-capable image always uses the card */
#else
    return __start_sunstone_large_regions && __stop_sunstone_large_regions > __start_sunstone_large_regions;
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
#ifdef OS_SD_WRITE
static enum a733_mmc_platform_result drive_platform(uint64_t deadline);
#endif
static int read_blocking(uint64_t sector, uint32_t count, uint8_t *dma)
{
    enum a733_mmc_platform_result r;
    do { r = a733_mmc_platform_read(&platform, sector, count, dma, count * 512u, now_us() + READ_US); }
    while (r == A733_MMC_BUSY && board_live(NULL));
    if (r != A733_MMC_PENDING) return 0;
#ifdef OS_SD_WRITE
    r = drive_platform(now_us() + READ_US);
#else
    do { r = a733_mmc_platform_poll(&platform); } while ((r == A733_MMC_PENDING || r == A733_MMC_BUSY) && board_live(NULL));
#endif
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
#ifdef OS_SD_WRITE
const struct os_large_device *os_sd_open_owner_device(void)
#else
const struct os_large_device *os_large_device(void)
#endif
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
    if (r != A733_MMC_CLOSED) (void)a733_mmc_platform_retain(&platform);
    opened = 0;
    if (descriptor_lease.pool) (void)device_memory_release(a733_memory_uncached_init(), descriptor_lease);
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
void os_sd_irq(void)
{
    if (!irq_live) return;
    uint32_t mista = *(volatile uint32_t *)(uintptr_t)(SMHC0 + 0x34);
    if (!mista) return;
    atomic_fetch_or_explicit(&irq_bits, mista, memory_order_release);
    *(volatile uint32_t *)(uintptr_t)(SMHC0 + 0x38) = mista;
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(owner_task, &woken);
    portYIELD_FROM_ISR(woken);
}
static uint32_t irq_taken(void *unused) { (void)unused; return atomic_exchange_explicit(&irq_bits, 0, memory_order_acquire); }

/* The wait loop: poll, then sleep on the interrupt or a one-shot timer, as the core's hint says. */
struct drive { enum a733_mmc_platform_result r; int (*poll)(void); };
static int dr_poll(void *c)
{
    struct drive *d = c;
    if (d->poll) return d->poll();
    d->r = a733_mmc_platform_poll(&platform);
    return d->r != A733_MMC_PENDING && d->r != A733_MMC_BUSY;
}
static enum sunxi_mmc_wait dr_hint(void *c, uint32_t *us) { (void)c; return sunxi_mmc_wait_hint(&platform.core, us); }
static uint64_t dr_now(void *c) { (void)c; return now_us(); }
static _Atomic uint64_t sleep_us, sleep_count;
static void dr_sleep(void *c, uint64_t until)
{
    (void)c;
    uint64_t from = now_us();
    (void)sunstone_deadline_wait_until(until);
    atomic_fetch_add(&sleep_us, now_us() - from);
    atomic_fetch_add(&sleep_count, 1);
}
static uint64_t hw_owner_sleep_us(void) { return atomic_load(&sleep_us); }
static uint64_t hw_owner_wakes(void) { return atomic_load(&sleep_count); }
static int dr_live(void *c) { (void)c; return board_live(NULL); }
static enum a733_mmc_platform_result drive_platform(uint64_t deadline)
{
    struct drive d = {A733_MMC_PENDING, NULL};
    const struct storage_wait_ops ops = {&d, dr_poll, dr_hint, dr_now, dr_sleep, dr_live};
    (void)storage_wait_run(&ops, deadline);
    return d.r;
}
static int async_value;
static int async_poll(void) { async_value = sd_poll(NULL); return async_value != 0; }
static int hw_wait_async(void)
{
    struct drive d = {A733_MMC_PENDING, async_poll};
    const struct storage_wait_ops ops = {&d, dr_poll, dr_hint, dr_now, dr_sleep, dr_live};
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
    r = drive_platform(now_us() + READ_US);
    if (r != A733_MMC_COMPLETE) return 0;
    do { r = a733_mmc_platform_take(&platform, &result, config.lifetime_deadline_us); } while (r == A733_MMC_BUSY);
    if (r != A733_MMC_READY || result.error != SUNXI_MMC_OK || result.received != SUNXI_MMC_SD_STATUS_BYTES) return 0;
    *sectors = sunxi_mmc_au_sectors(status, platform.core.card.signal_1v8 != 0);
    return 1;
}
static int bounds_read(void *unused, uint64_t sector, uint8_t *out) { (void)unused; return sd_read(NULL, sector, 1, out); }
static int hw_enable_writes(const uint8_t *blob, size_t bytes)
{
    /* All kernel data is writable at EL0 in this OS, so no mapping protects the blob. The runner's input is copied once into
     * private memory and parsed there; the bounds object re-hashes the copy on every use (a733_storage_bounds_allows). */
    if (!blob || !bytes || blob_copy || !card.sector_addressed) return 0;
    blob_copy = malloc(bytes);                       /* the OS heap; the only refusal is that it has no room */
    if (!blob_copy) return 0;
    memcpy(blob_copy, blob, bytes);
    return a733_storage_bounds_parse(&bounds, blob_copy, bytes) && a733_storage_bounds_verify(&bounds, bounds_read, NULL) &&
        a733_mmc_platform_native_bind_bounds(&native, &platform, &bounds);
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
static int hw_write_piece(uint64_t sector, uint32_t count, const uint8_t *src)
{
    if (!count || count > OS_SD_WRITE_PIECE_SECTORS) return 0;
    size_t bytes = (size_t)count * 512u;
    const uint8_t *dma = src;
    if (!in_region(src, bytes) || ((uintptr_t)src & 63u)) { memcpy(write_buffer, src, bytes); dma = write_buffer; }   /* metadata and small pieces */
    struct sunxi_mmc_write_authority authority;
    a733_storage_bounds_authority(&bounds, &authority);
    enum a733_mmc_platform_result r;
    do { r = a733_mmc_platform_write_dma(&platform, &authority, A733_BOUNDED_WRITE_TOKEN, sector, count, dma, now_us() + WRITE_US); }
    while (r == A733_MMC_BUSY && board_live(NULL));
    if (r != A733_MMC_PENDING) return 0;
    r = drive_platform(now_us() + WRITE_US);
    if (r != A733_MMC_COMPLETE) return 0;
    do { r = a733_mmc_platform_take(&platform, &result, config.lifetime_deadline_us); } while (r == A733_MMC_BUSY);
    return r == A733_MMC_READY && result.error == SUNXI_MMC_OK && result.confirmed_blocks == count;
}
static struct os_sd_hw hw;
const struct os_sd_hw *os_sd_open_owner(void)
{
    const struct os_large_device *d = os_sd_open_owner_device();
    if (!d) return 0;
    /* The owner task sleeps on the controller interrupt and on one-shot timers (sunstone_deadline), never on the tick. */
    owner_task = xTaskGetCurrentTaskHandle();
    if (sunstone_deadline_register() != 1 || !sunxi_mmc_set_irq(&platform.core, irq_taken, NULL)) return 0;
    irq_live = 1;
    hw = (struct os_sd_hw){d, card.sectors, card.sector_addressed, hw_allocation_unit, hw_enable_writes, hw_write_piece, hw_live,
                           hw_set_write_region, hw_wait_async, hw_owner_sleep_us, hw_owner_wakes};
    return &hw;
}
void os_sd_close_owner(void) { irq_live = 0; os_sd_close_owner_device(); }
#endif
