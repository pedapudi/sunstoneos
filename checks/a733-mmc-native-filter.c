/* Runs the native adapter's real register filter (drivers/storage/a733_mmc_platform_native.c, built with
 * A733_MMC_NATIVE_HOST_TEST: the AArch64 task test and the register accesses go through hooks below) against every
 * CMD25 and write-threshold rule of the bounded writer. Built twice by checks/a733-mmc-native-filter.py: for a
 * write-capable image, and with SUNXI_MMC_READ_ONLY, where CMD25 and the threshold write must be refused whatever the
 * binding says. */
#include "a733_platform_locks.h"
#include "a733_mmc_platform_native.h"
#include "a733_storage_bounds.h"
#include "a733_storage_digest.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Hooks the filter build calls. */
static struct { uintptr_t address; uint32_t value; } regs[64];
static unsigned reg_count, cmd_stores, thld_stores;
static int task_ok = 1;
static uintptr_t cur_base = 0x04020000u;           /* the selected controller's register base */
uint32_t a733_native_host_load(uintptr_t a)
{ for (unsigned i = 0; i < reg_count; ++i) if (regs[i].address == a) return regs[i].value; return 0; }
void a733_native_host_store(uintptr_t a, uint32_t v)
{
    if (a == cur_base + SUNXI_SMHC_CMD) ++cmd_stores;
    if (a == cur_base + SUNXI_SMHC_THLD) ++thld_stores;
    for (unsigned i = 0; i < reg_count; ++i) if (regs[i].address == a) { regs[i].value = v; return; }
    assert(reg_count < 64); regs[reg_count].address = a; regs[reg_count++].value = v;
}
int a733_native_host_current(void *n) { (void)n; return task_ok; }
/* Locks and the update service are linked but not exercised here. */
int a733_platform_lock_enter(struct a733_platform_locks *l, enum a733_platform_lock k, uint64_t d) { (void)l; (void)k; (void)d; return 1; }
void a733_platform_lock_leave(struct a733_platform_locks *l, enum a733_platform_lock k) { (void)l; (void)k; }
int a733_platform_lock_retain(struct a733_platform_locks *l, enum a733_platform_lock k) { (void)l; (void)k; return 1; }
_Noreturn void platform_panic(const char *file, unsigned line) { fprintf(stderr, "platform panic %s:%u\n", file, line); abort(); }
int a733_storage_update_mmio(const struct a733_storage_update *u, uintptr_t a, uint32_t v) { (void)u; (void)a; (void)v; return 0; }
int a733_storage_update_fifo(const struct a733_storage_update *u, uintptr_t a, unsigned c) { (void)u; (void)a; (void)c; return 0; }
int a733_storage_update_owned(const struct a733_storage_update *u, const struct a733_mmc_platform *p) { (void)u; (void)p; return 0; }

#define EXACT ((1u << 31) | (1u << 13) | (1u << 12) | (1u << 10) | (1u << 9) | (1u << 8) | (1u << 6) | 25u)

static uint8_t blob[48 + 16 * 5];
static uint8_t card_byte(uint64_t sector, unsigned i) { return (uint8_t)(sector * 13 + i * 3 + 1); }
static int read_card(void *c, uint64_t sector, uint8_t *out) { (void)c; for (unsigned i = 0; i < 512; ++i) out[i] = card_byte(sector, i); return 1; }
static void put64(uint8_t *p, uint64_t v) { for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static void make_blob(void)
{
    static const uint64_t write[3][2] = {{1000, 100}, {2000, 256}, {2256, 10}}, backup[2][2] = {{0, 1}, {1000, 2}};
    memcpy(blob, A733_BOUNDS_MAGIC, 8); blob[8] = 3; blob[12] = 2;
    struct storage_digest d; storage_digest_init(&d);
    for (unsigned r = 0; r < 2; ++r)
        for (uint64_t s = backup[r][0]; s < backup[r][0] + backup[r][1]; ++s) {
            uint8_t rec[520]; put64(rec, s); read_card(NULL, s, rec + 8); storage_digest_add(&d, rec, sizeof rec);
        }
    storage_digest_end(&d, blob + 16);
    for (unsigned i = 0; i < 3; ++i) { put64(blob + 48 + 16 * i, write[i][0]); put64(blob + 56 + 16 * i, write[i][1]); }
    for (unsigned i = 0; i < 2; ++i) { put64(blob + 48 + 48 + 16 * i, backup[i][0]); put64(blob + 56 + 48 + 16 * i, backup[i][1]); }
}

static struct a733_platform_locks locks;
static struct a733_mmc_platform_native n;
static struct a733_storage_bounds bounds;
static struct a733_mmc_platform platform;                    /* only the fields the binding reads */
static void card(unsigned sector_addressed, uint64_t sectors, enum sunxi_mmc_kind kind)
{
    memset(&platform, 0, sizeof platform);
    platform.cookie = &n; platform.config.kind = kind;
    platform.core.card.sectors = sectors; platform.core.card.sector_addressed = (uint8_t)sector_addressed;
}
static int admission(void *c, const struct a733_mmc_platform_config *cfg, const struct a733_clock_token *t) { (void)c; (void)cfg; (void)t; return 1; }

static void fresh(enum sunxi_mmc_kind kind, enum sunxi_mmc_sd_profile profile, int verified)
{
    memset(regs, 0, sizeof regs); reg_count = cmd_stores = thld_stores = 0; task_ok = 1;
    memset(&locks, 0, sizeof locks); locks.initialized = 1;
    memset(&n, 0, sizeof n); memset(&bounds, 0, sizeof bounds);
    cur_base = kind == SUNXI_MMC_SD ? 0x04020000u : 0x04022000u;
    static int dummy_task;
    assert(a733_mmc_platform_native_init_selected(&n, &locks, &dummy_task, kind, A733_MMC_EMMC_SDC2, admission, NULL));
    n.sd_profile = profile;
    assert(n.operations.enter(&n, 1000));
    card(1, 1u << 20, kind);
    assert(a733_storage_bounds_parse(&bounds, blob, sizeof blob));
    if (verified) assert(a733_storage_bounds_verify(&bounds, read_card, NULL));
}
static int w(uintptr_t offset, uint32_t value) { return n.operations.write(&n, cur_base + offset, value); }
static int command(uint32_t word, uint32_t arg, uint32_t blksz, uint32_t bcnt)
{
    assert(w(SUNXI_SMHC_ARG, arg) && w(SUNXI_SMHC_BLKSZ, blksz) && w(SUNXI_SMHC_BCNT, bcnt));
    unsigned before = cmd_stores;
    int r = w(SUNXI_SMHC_CMD, word);
    assert((cmd_stores - before) == (r ? 1u : 0u));                    /* a refused word never reaches the register */
    return r;
}

int main(void)
{
    make_blob();
    /* The PF selector: UHS-I profile only; 1.8 V (value 0) then, after that write and with the card clock off, 3.3 V (value 1). */
    fresh(SUNXI_MMC_SD, SUNXI_MMC_SD_UHS_SDR104, 0);
    assert(!n.operations.write(&n, 0x02000070, 1) && !n.operations.write(&n, 0x02000070, 2));          /* no restore before the switch */
    assert(n.operations.write(&n, 0x02000070, 0) && !n.operations.write(&n, 0x02000070, 0));           /* 1.8 V once */
    a733_native_host_store(cur_base + SUNXI_SMHC_CLKCR, 1u << 16);
    assert(!n.operations.write(&n, 0x02000070, 1));                                                    /* the card clock is running */
    a733_native_host_store(cur_base + SUNXI_SMHC_CLKCR, 0);
    task_ok = 0; assert(!n.operations.write(&n, 0x02000070, 1)); task_ok = 1;                          /* not the owning task */
    assert(n.operations.write(&n, 0x02000070, 1) && !n.operations.write(&n, 0x02000070, 1));           /* 3.3 V once */
    for (unsigned profile = SUNXI_MMC_SD_LEGACY; profile < SUNXI_MMC_SD_UHS_SDR104; ++profile) {
        fresh(SUNXI_MMC_SD, (enum sunxi_mmc_sd_profile)profile, 0);
        assert(!n.operations.write(&n, 0x02000070, 0) && !n.operations.write(&n, 0x02000070, 1));      /* other profiles never write it */
    }
    fresh(SUNXI_MMC_EMMC, SUNXI_MMC_SD_LEGACY, 0);
    assert(!n.operations.write(&n, 0x02000070, 0) && !n.operations.write(&n, 0x02000070, 1));
#if !defined(SUNXI_MMC_READ_ONLY) && defined(A733_MMC_BOUNDED_WRITER)
    /* Write-capable build. */
    fresh(SUNXI_MMC_SD, SUNXI_MMC_SD_HIGH_SPEED4, 1);
    assert(!command(EXACT, 1000, 512, 512 * 100));                      /* unbound */
    assert(!w(SUNXI_SMHC_THLD, 0x02000004u) && !thld_stores);
    assert(a733_mmc_platform_native_bind_bounds(&n, &platform, &bounds));
    assert(!a733_mmc_platform_native_bind_bounds(&n, &platform, &bounds));         /* once */
    /* The accepted cases and the edges. */
    assert(command(EXACT, 1000, 512, 512 * 100));
    assert(command(EXACT, 1099, 512, 512));
    assert(command(EXACT, 2000, 512, 512 * 256));
    assert(command(EXACT, 2256, 512, 512 * 10));
    assert(!command(EXACT, 999, 512, 512) && !command(EXACT, 1100, 512, 512));                    /* just outside */
    assert(!command(EXACT, 1000, 512, 512 * 101) && !command(EXACT, 1099, 512, 1024));           /* straddle the end */
    assert(!command(EXACT, 999, 512, 1024));                                                      /* straddle the start */
    assert(!command(EXACT, 2255, 512, 1024));                                                     /* spans two adjacent ranges */
    assert(!command(EXACT, 2256, 512, 512 * 11) && !command(EXACT, 2000, 512, 512 * 257));
    assert(!command(EXACT, 0, 512, 512) && !command(EXACT, 0xffffffffu, 512, 512));
    /* Block size and count. */
    assert(!command(EXACT, 1000, 256, 512) && !command(EXACT, 1000, 1024, 1024) && !command(EXACT, 1000, 0, 512));
    assert(!command(EXACT, 1000, 512, 0) && !command(EXACT, 1000, 512, 513) && !command(EXACT, 1000, 512, 511));
    assert(!command(EXACT, 1000, 512, 512 * 257) && !command(EXACT, 1000, 512, 0xffffffffu));
    /* Every bit of the command word. */
    for (unsigned bit = 0; bit < 32; ++bit) assert(!command(EXACT ^ (1u << bit), 1000, 512, 512));
    /* The clock-update form of index 25 is not a way around the rule. */
    assert(!command(SUNXI_SMHC_UPDATE | 25u, 1000, 512, 512) && !command(EXACT | SUNXI_SMHC_CMD_UPDATE_CLOCK, 1000, 512, 512));
    assert(command(SUNXI_SMHC_UPDATE, 0, 0, 0));                         /* the real clock update still works */
    /* Other commands: reads pass as before, CMD24 and CMD12-with-write do not. */
    assert(command(SUNXI_SMHC_CMD_START | SUNXI_SMHC_CMD_RESP | SUNXI_SMHC_CMD_CRC | SUNXI_SMHC_CMD_WAIT | 13u, 0, 0, 0));
    assert(!command(SUNXI_SMHC_CMD_START | SUNXI_SMHC_CMD_RESP | SUNXI_SMHC_CMD_CRC | SUNXI_SMHC_CMD_DATA | SUNXI_SMHC_CMD_WRITE | SUNXI_SMHC_CMD_WAIT | 24u, 1000, 512, 512));
    assert(!command(SUNXI_SMHC_CMD_START | SUNXI_SMHC_CMD_RESP | SUNXI_SMHC_CMD_CRC | SUNXI_SMHC_CMD_DATA | SUNXI_SMHC_CMD_WAIT | 38u, 1000, 0, 0));
    /* The threshold register is writable by a verified bounded writer, in a non-UHS profile too. */
    assert(w(SUNXI_SMHC_THLD, 0x02000004u) && thld_stores == 1);
    /* Not the task that owns the binding: nothing passes. */
    task_ok = 0; assert(!w(SUNXI_SMHC_ARG, 1000)); task_ok = 1;
    /* A closed object (state changed after the bind) stops both rules at once. */
    bounds.state = A733_BOUNDS_CLOSED;
    assert(!command(EXACT, 1000, 512, 512) && !w(SUNXI_SMHC_THLD, 4));
    /* Binding refusals: unverified, closed, eMMC, wrong task. */
    fresh(SUNXI_MMC_SD, SUNXI_MMC_SD_HIGH_SPEED4, 0);
    assert(!a733_mmc_platform_native_bind_bounds(&n, &platform, &bounds) && !command(EXACT, 1000, 512, 512) && !w(SUNXI_SMHC_THLD, 4));
    fresh(SUNXI_MMC_EMMC, SUNXI_MMC_SD_LEGACY, 1);
    assert(!a733_mmc_platform_native_bind_bounds(&n, &platform, &bounds) && !command(EXACT, 1000, 512, 512));
    n.bounds = &bounds;                                                  /* even a forced binding is refused for eMMC */
    assert(!command(EXACT, 1000, 512, 512) && !w(SUNXI_SMHC_THLD, 4));
    fresh(SUNXI_MMC_SD, SUNXI_MMC_SD_HIGH_SPEED4, 1);
    task_ok = 0; assert(!a733_mmc_platform_native_bind_bounds(&n, &platform, &bounds)); task_ok = 1;
    assert(a733_mmc_platform_native_bind_bounds(&n, &platform, &bounds));
    /* The card test the image applies before binding. */
    assert(a733_storage_bounds_card_ok(&bounds, 1, 1));
    assert(!a733_storage_bounds_card_ok(&bounds, 1, 0));                 /* byte-addressed card */
    assert(!a733_storage_bounds_card_ok(&bounds, 0, 1));                 /* eMMC */
    /* The binding makes the card test itself: byte-addressed, unidentified, eMMC and foreign-platform cards are refused. */
    fresh(SUNXI_MMC_SD, SUNXI_MMC_SD_HIGH_SPEED4, 1);
    card(0, 1u << 20, SUNXI_MMC_SD);
    assert(!a733_mmc_platform_native_bind_bounds(&n, &platform, &bounds) && !n.bounds && !command(EXACT, 1000, 512, 512));
    assert(!w(SUNXI_SMHC_THLD, 4));
    card(1, 0, SUNXI_MMC_SD);                                            /* no card identified yet */
    assert(!a733_mmc_platform_native_bind_bounds(&n, &platform, &bounds));
    card(1, 1u << 20, SUNXI_MMC_EMMC);
    assert(!a733_mmc_platform_native_bind_bounds(&n, &platform, &bounds));
    card(1, 1u << 20, SUNXI_MMC_SD); platform.cookie = &locks;           /* a platform that does not own this adapter */
    assert(!a733_mmc_platform_native_bind_bounds(&n, &platform, &bounds));
    assert(!a733_mmc_platform_native_bind_bounds(&n, NULL, &bounds));
    card(1, 1u << 20, SUNXI_MMC_SD);
    assert(a733_mmc_platform_native_bind_bounds(&n, &platform, &bounds) && command(EXACT, 1000, 512, 512));   /* and the sector-addressed card binds */
    struct a733_storage_bounds raw = {0};
    assert(!a733_storage_bounds_card_ok(&raw, 1, 1) && !a733_storage_bounds_card_ok(NULL, 1, 1));
    puts("PASS: write-capable filter: CMD25 accepted only for the exact word, 512-byte blocks, 1 to 256 blocks inside one admitted range; "
         "32 single-bit changes, block size and count, edges, straddles, clock-update form, unbound, unverified, closed, eMMC and foreign task refused; "
         "threshold writable only when verified; byte-addressed card refused");
#else
    /* Read-only build, or a build without the bounded writer: nothing makes CMD25 or the threshold writable, even a forced binding to a verified object. */
    fresh(SUNXI_MMC_SD, SUNXI_MMC_SD_HIGH_SPEED4, 1);
    n.bounds = &bounds;
    assert(!command(EXACT, 1000, 512, 512 * 100) && !command(EXACT, 2000, 512, 512 * 256));
    assert(!w(SUNXI_SMHC_THLD, 0x02000004u) && !thld_stores);
    for (unsigned bit = 0; bit < 32; ++bit) assert(!command(EXACT ^ (1u << bit), 1000, 512, 512));
    assert(!command(SUNXI_SMHC_CMD_START | SUNXI_SMHC_CMD_RESP | SUNXI_SMHC_CMD_CRC | SUNXI_SMHC_CMD_DATA | SUNXI_SMHC_CMD_WRITE | SUNXI_SMHC_CMD_WAIT | 24u, 1000, 512, 512));
    assert(command(SUNXI_SMHC_CMD_START | SUNXI_SMHC_CMD_RESP | SUNXI_SMHC_CMD_CRC | SUNXI_SMHC_CMD_WAIT | 13u, 0, 0, 0));   /* reads still work */
    assert(command(SUNXI_SMHC_CMD_START | SUNXI_SMHC_CMD_RESP | SUNXI_SMHC_CMD_CRC | SUNXI_SMHC_CMD_DATA | SUNXI_SMHC_CMD_WAIT | 18u, 1000, 512, 512));
    puts("PASS: read-only or writer-less filter refuses CMD25 (the exact word, 32 single-bit changes) and the threshold write even with a forced verified binding; reads and CMD13 still pass");
#endif
    return 0;
}
