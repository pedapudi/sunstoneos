/* Stage-2 slot loader for the cold-entry monitor; docs/boot-slots.md.
 * It reads two fixed slot headers through the polled SD core, copies and
 * hashes the newest valid slot in place and publishes a report. It never
 * writes storage or enables caches, and it never enters the loaded image:
 * no cold launch contract or remote-writer exclusion exists yet. Every
 * reported outcome requests the ROM forced-FEL return of the RAM monitor. */
#include "sunxi_mmc.h"
#include "a733_storage_digest.h"
#include <stddef.h>
#include <string.h>

#define SLOT_MAGIC UINT64_C(0x41533733534c4f54)
#define REPORT_MAGIC UINT64_C(0x4153373353544732)
#define SLOT_VERSION 1u
#define SLOT_EXTENTS 4u
#define EXTENT_IMAGE 1u
#define EXTENT_ASSET 2u
#define BOOT_AREA_SECTORS UINT64_C(40960) /* boot0 and the TOC1 container. */
#define LOAD_BASE UINT64_C(0x41000000)
#define LOAD_LIMIT UINT64_C(0x47000000)   /* The monitor reservation begins here. */
#define SDC0 UINT64_C(0x04020000)
#define CHUNK_BYTES (SUNXI_MMC_MAX_BLOCKS * SUNXI_MMC_BLOCK_BYTES)
#define OPEN_US 2000000u
#define READ_US 500000u /* 4 KiB at the 200 kHz 1-bit identification clock. */

enum { LOADED = 1, CARD = 2, NO_SLOT = 3, LOAD_FAILED = 4 };
enum { HEADER_UNREAD, HEADER_VALID, HEADER_READ, HEADER_ABSENT, HEADER_DIGEST,
       HEADER_FIELDS };
enum { LOAD_NONE, LOAD_DONE, LOAD_READ, LOAD_DIGEST };

struct slot_extent {
    uint64_t first_sector, bytes, load;
    uint32_t kind, reserved;
    uint8_t sha256[32];
};
struct slot_header {
    uint64_t magic;
    uint32_t version, slot;
    uint64_t generation, inverse_generation;
    uint32_t count, flags;
    uint64_t entry, reserved0[2];
    struct slot_extent extent[SLOT_EXTENTS];
    uint8_t reserved1[160], sha256[32]; /* SHA-256 of bytes 0..479. */
};
_Static_assert(sizeof(struct slot_extent) == 64, "slot extent ABI");
_Static_assert(offsetof(struct slot_header, extent) == 64, "slot header ABI");
_Static_assert(offsetof(struct slot_header, sha256) == 480, "slot header ABI");
_Static_assert(sizeof(struct slot_header) == SUNXI_MMC_BLOCK_BYTES, "slot header ABI");

struct stage2_report {
    uint64_t magic, version, bytes, status, detail, selected, generation;
    uint64_t card_sectors, start_counter, end_counter;
    uint64_t mmc_error, mmc_interrupts, mmc_status, mmc_response, reserved0[2];
    struct { uint64_t header, generation, load, extent; } slot[2];
    uint64_t reserved1[40];
};
_Static_assert(sizeof(struct stage2_report) == 512, "stage-2 report ABI");
_Static_assert(offsetof(struct stage2_report, slot) == 128, "stage-2 report ABI");

/* Header sectors sit at 4 MiB and 8 MiB, in allocation units separate from
 * boot0, the unpopulated backup TOC1 at 12 MiB and the TOC1 at 16 MiB. */
static const uint64_t header_sector[2] = {8192, 16384};

__attribute__((section(".bss.stage2_report"))) volatile struct stage2_report a733_stage2_report;
static struct sunxi_mmc card;
static struct slot_header headers[2];
static _Alignas(16) uint8_t chunk[CHUNK_BYTES];
static int admitted;

static uint64_t counter(void)
{
    uint64_t n;
    __asm__ volatile("isb\n mrs %0, cntpct_el0" : "=r"(n) :: "memory");
    return n;
}

/* Separate unit: admission evidence for a cold SD controller (admission.c). */
int a733_stage2_card_admission(uint32_t module_hz);

static int admit(void *context, uintptr_t token, enum sunxi_mmc_kind kind, uint32_t module_hz)
{
    (void)context;
    if (token != (uintptr_t)&card || kind != SUNXI_MMC_SD) return 0;
    admitted = a733_stage2_card_admission(module_hz) == 1;
    return admitted;
}
/* One owner and no scheduler; only a granted admission is retained. */
static int retained(void *context, uintptr_t token)
{
    (void)context;
    return admitted && token == (uintptr_t)&card;
}
static int read32(void *context, uint32_t offset, uint32_t *value)
{
    (void)context;
    if (!value || offset > 0x300 || (offset & 3)) return 0;
    __asm__ volatile("dsb sy" ::: "memory");
    *value = *(volatile const uint32_t *)(uintptr_t)(SDC0 + offset);
    __asm__ volatile("dsb sy" ::: "memory");
    return 1;
}
/* Read-only identification and transfer commands; no write direction or FIFO
 * write. Clock-update commands (bit 21) carry no card command. */
static int write32(void *context, uint32_t offset, uint32_t value)
{
    (void)context;
    if (offset > 0x300 || (offset & 3) || offset == 0x200) return 0;
    if (offset == 0x18 && !(value & (1u << 21))) {
        if (value & (1u << 10)) return 0;
        switch (value & 63u) {
        case 0: case 2: case 3: case 7: case 8: case 9: case 13: case 16:
        case 17: case 18: case 41: case 55: break;
        default: return 0;
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)(uintptr_t)(SDC0 + offset) = value;
    __asm__ volatile("dsb sy" ::: "memory");
    return 1;
}
static int barrier(void *context)
{
    (void)context;
    __asm__ volatile("dsb sy" ::: "memory");
    return 1;
}
/* CNTFRQ_EL0 reads zero after boot0; the board counter runs at 24 MHz. */
static int now_us(void *context, uint64_t *value)
{
    (void)context;
    if (!value) return 0;
    *value = counter() / 24u;
    return 1;
}

static void record(void)
{
    struct sunxi_mmc_result r;
    if (!sunxi_mmc_take(&card, &r)) return;
    a733_stage2_report.mmc_error = r.error;
    a733_stage2_report.mmc_interrupts = r.interrupts;
    a733_stage2_report.mmc_status = r.status;
    a733_stage2_report.mmc_response = r.response;
}
static int card_open(void)
{
    struct sunxi_mmc_config config = {
        .ops = {read32, write32, barrier, now_us, admit, retained, 0},
        .token = (uintptr_t)&card, .kind = SUNXI_MMC_SD,
        .module_hz = 400000, .voltage_mask = 0x00300000,
    };
    uint64_t now;
    if (!sunxi_mmc_init(&card, &config) || !now_us(0, &now) ||
        !sunxi_mmc_open(&card, now + OPEN_US)) return 0;
    while (card.state == SUNXI_MMC_IDENTIFY) sunxi_mmc_poll(&card);
    if (card.state != SUNXI_MMC_READY) { record(); return 0; }
    a733_stage2_report.card_sectors = card.card.sectors;
    return 1;
}
static int card_read(uint64_t sector, uint32_t blocks, void *buffer)
{
    uint64_t now;
    if (card.state != SUNXI_MMC_READY || !now_us(0, &now) ||
        !sunxi_mmc_read(&card, sector, blocks, buffer, blocks * SUNXI_MMC_BLOCK_BYTES,
                        now + READ_US)) return 0;
    while (card.state == SUNXI_MMC_READING) sunxi_mmc_poll(&card);
    int done = card.state == SUNXI_MMC_DONE;
    record();
    return done && card.state == SUNXI_MMC_READY;
}

static int zero(const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; ++i) if (b[i]) return 0;
    return 1;
}
static int disjoint(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{ return a + an <= b || b + bn <= a; }
static int extent_valid(const struct slot_header *h, unsigned i)
{
    const struct slot_extent *e = &h->extent[i];
    uint64_t sectors = (e->bytes + SUNXI_MMC_BLOCK_BYTES - 1) / SUNXI_MMC_BLOCK_BYTES;
    uint64_t total = card.card.sectors;
    if (e->kind != (i ? EXTENT_ASSET : EXTENT_IMAGE) || e->reserved || !e->bytes ||
        e->bytes > LOAD_LIMIT - LOAD_BASE || e->first_sector < BOOT_AREA_SECTORS ||
        e->first_sector > total || sectors > total - e->first_sector ||
        e->load < LOAD_BASE || e->load > LOAD_LIMIT || e->bytes > LOAD_LIMIT - e->load)
        return 0;
    for (unsigned j = 0; j < i; ++j) {
        const struct slot_extent *o = &h->extent[j];
        uint64_t other = (o->bytes + SUNXI_MMC_BLOCK_BYTES - 1) / SUNXI_MMC_BLOCK_BYTES;
        if (!disjoint(e->load, e->bytes, o->load, o->bytes) ||
            !disjoint(e->first_sector, sectors, o->first_sector, other)) return 0;
    }
    return 1;
}
static unsigned header_check(const struct slot_header *h, unsigned slot)
{
    uint8_t digest[32];
    if (h->magic != SLOT_MAGIC) return HEADER_ABSENT;
    storage_digest_bytes(h, offsetof(struct slot_header, sha256), digest);
    if (memcmp(digest, h->sha256, sizeof(digest))) return HEADER_DIGEST;
    if (h->version != SLOT_VERSION || h->slot != slot || !h->generation ||
        h->inverse_generation != ~h->generation || !h->count || h->count > SLOT_EXTENTS ||
        h->flags || h->reserved0[0] || h->reserved0[1] || !zero(h->reserved1, sizeof(h->reserved1)))
        return HEADER_FIELDS;
    for (unsigned i = 0; i < SLOT_EXTENTS; ++i)
        if (i < h->count ? !extent_valid(h, i) : !zero(&h->extent[i], sizeof(h->extent[i])))
            return HEADER_FIELDS;
    const struct slot_extent *image = &h->extent[0];
    if ((h->entry & 3) || h->entry < image->load || h->entry - image->load >= image->bytes)
        return HEADER_FIELDS;
    return HEADER_VALID;
}
/* Copy each extent to its load address, then hash those destination bytes;
 * with the data cache disabled the digest covers memory, not a cached copy. */
static unsigned load(const struct slot_header *h, uint64_t *failed)
{
    for (unsigned i = 0; i < h->count; ++i) {
        const struct slot_extent *e = &h->extent[i];
        uint8_t *target = (uint8_t *)(uintptr_t)e->load, digest[32];
        struct storage_digest d;
        storage_digest_init(&d);
        for (uint64_t done = 0; done < e->bytes; done += CHUNK_BYTES) {
            uint64_t n = e->bytes - done < CHUNK_BYTES ? e->bytes - done : CHUNK_BYTES;
            uint32_t blocks = (uint32_t)((n + SUNXI_MMC_BLOCK_BYTES - 1) / SUNXI_MMC_BLOCK_BYTES);
            if (!card_read(e->first_sector + done / SUNXI_MMC_BLOCK_BYTES, blocks, chunk)) {
                *failed = i;
                return LOAD_READ;
            }
            memcpy(target + done, chunk, (size_t)n);
            __asm__ volatile("dsb sy" ::: "memory");
            (void)storage_digest_add(&d, target + done, (size_t)n);
        }
        storage_digest_end(&d, digest);
        if (memcmp(digest, e->sha256, sizeof(digest))) {
            *failed = i;
            return LOAD_DIGEST;
        }
    }
    return LOAD_DONE;
}

/* Same sequence as monitor_reset, without the cache clean: the data cache
 * has stayed disabled since cold entry. ROM entry 0x20 requests FEL. */
static _Noreturn void rom_return(void)
{
    __asm__ volatile("ic iallu\n dsb sy\n isb" ::: "memory");
    *(volatile uint32_t *)UINT64_C(0x08001004) = 0x20;
    *(volatile uint32_t *)UINT64_C(0x08001008) = 0;
    __asm__ volatile("dsb sy\n msr rmr_el3, %0\n isb" :: "r"(UINT64_C(2)) : "memory");
    for (;;) __asm__ volatile("wfi");
}
static _Noreturn void finish(uint64_t status, uint64_t detail)
{
    a733_stage2_report.status = status;
    a733_stage2_report.detail = detail;
    a733_stage2_report.end_counter = counter();
    __asm__ volatile("dsb sy" ::: "memory");
    a733_stage2_report.magic = REPORT_MAGIC;
    __asm__ volatile("dsb sy" ::: "memory");
    rom_return();
}

/* Entered from cold-entry.S after an admitted capture, on a fresh stack with
 * zeroed stage-2 storage. Faults use the cold exception record and park. */
_Noreturn void a733_stage2_main(void)
{
    a733_stage2_report.version = 1;
    a733_stage2_report.bytes = sizeof(a733_stage2_report);
    a733_stage2_report.selected = UINT64_MAX;
    a733_stage2_report.start_counter = counter();
    if (!card_open()) finish(CARD, a733_stage2_report.mmc_error);
    unsigned valid = 0;
    for (unsigned s = 0; s < 2; ++s) {
        unsigned r = card_read(header_sector[s], 1, &headers[s]) ?
            header_check(&headers[s], s) : HEADER_READ;
        a733_stage2_report.slot[s].header = r;
        if (r == HEADER_VALID) {
            a733_stage2_report.slot[s].generation = headers[s].generation;
            valid |= 1u << s;
        }
    }
    if (!valid) finish(NO_SLOT, a733_stage2_report.mmc_error);
    /* Newest generation first; equal generations try slot A first. */
    unsigned first = valid == 2 || (valid == 3 && headers[1].generation > headers[0].generation);
    for (unsigned n = 0, s = first; n < 2; ++n, s ^= 1) {
        if (!(valid & (1u << s))) continue;
        uint64_t extent = 0;
        unsigned r = load(&headers[s], &extent);
        a733_stage2_report.slot[s].load = r;
        a733_stage2_report.slot[s].extent = extent;
        if (r == LOAD_DONE) {
            a733_stage2_report.selected = s;
            a733_stage2_report.generation = headers[s].generation;
            /* Boundary: entry needs a cold launch contract and remote-writer
             * exclusion; neither exists, so the verified image is not entered. */
            finish(LOADED, headers[s].entry);
        }
    }
    finish(LOAD_FAILED, a733_stage2_report.mmc_error);
}
