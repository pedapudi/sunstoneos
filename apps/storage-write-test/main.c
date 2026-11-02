/* storage-write-test: creates /sunstone-test/sunstone-region-test.bin (512 MiB, the counter-mode SHA-256 stream of
 * tools/region_test_file.py) on the card through the FAT32 write service, and measures the write path.
 *
 * It runs only in an image built with SUNSTONE_A733_OS_SD_WRITE, on the runner's bounded-write admission: the first input is the
 * admitted bounds blob (tools/storage_bounds.py), which the OS verifies against the card before any write. The second input
 * selects a mode (a little-endian u32, then a u64):
 *   0  create the file, sync, close, unmount and report (no read-back); a time guard (GUARD_US) stops the write, syncs and
 *      closes, and reports E_TIMEOUT with the bytes written;
 *   4  open the card, enable the writes and report (the QEMU boot check; no mount, no write);
 *   3  read the whole file back against the stream and report (a separate run; the file must be FILE_BYTES long);
 *   1  interruption test, lifetime 1: write with a sync every 8 MiB, and after `cut_bytes` stop reporting progress so the
 *      watchdog resets the board in the middle of a flush cycle;
 *   2  interruption test, lifetime 2: mount (recovery runs), read back what the volume holds against the stream, report.
 * The result (512 bytes) carries the write rate, the flush latency percentiles, the wear counters, the owner task's busy
 * time, and the measured CPU clock. Rates are figures only for an image at the approved top operating points. */
#include "storage_write-app.h"
#include "fat32_rw.h"
#include <string.h>

#define MIB (UINT64_C(1) << 20)
#ifndef FILE_BYTES                                /* the host guard check shrinks the file and the guard */
#define FILE_BYTES (512 * MIB)
#endif
#define PATH "/sunstone-test/sunstone-region-test.bin"
#define CHUNK 65536u
#define SYNC_EVERY (8 * MIB)
#define MAX_FLUSHES 4096u
#define TAG UINT64_C(0x5357544553544f4b)          /* "SWTESTOK" */
#ifndef GUARD_US
#define GUARD_US UINT64_C(40000000)               /* the write stops after this long and closes cleanly */
#endif
#define COUNTER_HZ UINT64_C(24000000)

/* One SHA-256 compression of a 64-byte block from the standard initial state (sha256_ce.S; ARMv8 Cryptographic Extension). */
void sha256_block_ce(const uint8_t block[64], uint8_t digest[32]);
static int use_ce;                                /* ID_AA64ISAR0_EL1.SHA2 != 0 on the core that generates the stream */

/* --- the stream: block i is SHA-256("sunstone-region-test-v1" || i as 8 bytes little endian), 32 bytes -------------- */
static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static const char PREFIX[] = "sunstone-region-test-v1";      /* 23 bytes */

static void stream_block(uint64_t index, uint8_t out[32])
{
    if (use_ce) {
        uint8_t blk[64] = {0};
        memcpy(blk, PREFIX, 23);
        for (unsigned i = 0; i < 8; ++i) blk[23 + i] = (uint8_t)(index >> (8 * i));
        blk[31] = 0x80; blk[63] = 248;
        sha256_block_ce(blk, out);
        return;
    }
    uint32_t w[64], a, b, c, d, e, f, g, h;
    uint8_t m[64] = {0};
    memcpy(m, PREFIX, 23);
    for (unsigned i = 0; i < 8; ++i) m[23 + i] = (uint8_t)(index >> (8 * i));
    m[31] = 0x80; m[62] = 0x00; m[63] = 248;                     /* 31 message bytes, 248 bits */
    for (unsigned i = 0; i < 16; ++i) w[i] = (uint32_t)m[4 * i] << 24 | (uint32_t)m[4 * i + 1] << 16 | (uint32_t)m[4 * i + 2] << 8 | m[4 * i + 3];
    for (unsigned i = 16; i < 64; ++i) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = 0x6a09e667; b = 0xbb67ae85; c = 0x3c6ef372; d = 0xa54ff53a; e = 0x510e527f; f = 0x9b05688c; g = 0x1f83d9ab; h = 0x5be0cd19;
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    uint32_t v[8] = {a + 0x6a09e667, b + 0xbb67ae85, c + 0x3c6ef372, d + 0xa54ff53a, e + 0x510e527f, f + 0x9b05688c, g + 0x1f83d9ab, h + 0x5be0cd19};
    for (unsigned i = 0; i < 8; ++i) { out[4 * i] = (uint8_t)(v[i] >> 24); out[4 * i + 1] = (uint8_t)(v[i] >> 16); out[4 * i + 2] = (uint8_t)(v[i] >> 8); out[4 * i + 3] = (uint8_t)v[i]; }
}
/* bytes [offset, offset + n) of the stream; offset and n are multiples of 32 except possibly the last piece */
static void stream_fill(uint8_t *dst, uint64_t offset, size_t n)
{
    uint8_t block[32];
    for (size_t done = 0; done < n;) {
        uint64_t at = offset + done, i = at / 32;
        size_t skip = (size_t)(at % 32), take = 32 - skip < n - done ? 32 - skip : n - done;
        stream_block(i, block);
        memcpy(dst + done, block + skip, take);
        done += take;
    }
}

/* --- the CPU clock: a chain of dependent adds timed against the 24 MHz counter ------------------------------------- */
static uint64_t cpu_cycles_per_second(uint64_t *ticks_out)
{
#ifndef __aarch64__                                      /* the host guard check */
    *ticks_out = 0;
    return 0;
#else
    const uint64_t loops = 20000000u;                       /* 64 dependent adds each */
    uint64_t t0 = ss_counter_ticks(), x = 0, n = loops;
    __asm__ volatile(
        "1:\n" ".rept 64\n" "add %[x], %[x], #1\n" ".endr\n" "subs %[n], %[n], #1\n" "b.ne 1b\n"
        : [x] "+r"(x), [n] "+r"(n) : : "cc");
    uint64_t t1 = ss_counter_ticks();
    *ticks_out = t1 - t0;
    return x == loops * 64 && t1 > t0 ? loops * 64 * 24000000u / (t1 - t0) : 0;
#endif
}

/* --- the result -------------------------------------------------------------------------------------------------------- */
struct result {
    uint64_t tag, mode, status, detail;
    uint64_t file_bytes, written_bytes, au_sectors, cpu_hz, cpu_calibration_ticks, counter_hz;
    uint64_t create_ticks, generate_ticks, flush_ticks, close_ticks, total_ticks, read_ticks;
    uint64_t flushes, flush_p50, flush_p99, flush_max;
    uint64_t wear_bytes, wear_data_bytes, wear_metadata_bytes, wear_writes, wear_aligned, wear_unaligned, wear_syncs, wear_flushes;
    uint64_t owner_elapsed_us, owner_asleep_us, owner_wakes;
    uint64_t mismatches, first_mismatch, volume_size, recoveries, recovered_clusters;
    uint64_t histogram[FWR_HIST];
    uint64_t app_core, irq_cores, owner_cores, caller_cores, sha_ce, guard_us;
    uint64_t diag[4], isar0, observed;               /* where an open or enable stopped (ss_storage_diagnostic); the core's ISAR0 */
};
_Static_assert(sizeof(struct result) == 512, "result is the manifest's output_limit");
static struct result r;

static int card_open, activity;
static void finish_report(void)
{
    if (activity) { ss_activity_end(r.status == 0); activity = 0; }          /* closes the thermal window the image reports */
    if (card_open) { (void)ss_storage_close(); card_open = 0; }
    ss_finish(&r, sizeof r);
}
enum { OK = 0, E_BOUNDS = 1, E_REGION = 2, E_DEVICE = 3, E_MBR = 4, E_MOUNT = 5, E_DIR = 6, E_OPEN = 7, E_WRITE = 8, E_CLOSE = 9, E_READ = 10, E_TIMEOUT = 11, E_SIZE = 12 };

static uint32_t flush_ticks[MAX_FLUSHES];
static void sort32(uint32_t *v, unsigned n)
{
    for (unsigned gap = n / 2; gap; gap /= 2)
        for (unsigned i = gap; i < n; ++i) { uint32_t t = v[i]; unsigned j = i; for (; j >= gap && v[j - gap] > t; j -= gap) v[j] = v[j - gap]; v[j] = t; }
}

static uint8_t cluster_buffer[65536] __attribute__((aligned(64)));
static uint8_t fat_window[2 * FWR_FAT_WINDOW_SECTORS * 512] __attribute__((aligned(64)));
static uint8_t recovery_bitmap[512 * 1024];
static struct fwr_volume volume;
static struct fwr_file file;
static struct fwr_device device;
static uint8_t chunk[CHUNK] __attribute__((aligned(64)));
static uint8_t check[CHUNK] __attribute__((aligned(64)));

static int mbr_partition(uint64_t *base, uint64_t *sectors)
{
    static uint8_t mbr[512] __attribute__((aligned(64)));
    if (device.read(device.context, 0, 1, mbr) != 1 || mbr[510] != 0x55 || mbr[511] != 0xaa) return 0;
    unsigned found = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const uint8_t *e = mbr + 446 + 16 * i;
        if (e[4] == 0x0b || e[4] == 0x0c) {
            *base = (uint64_t)e[8] | (uint64_t)e[9] << 8 | (uint64_t)e[10] << 16 | (uint64_t)e[11] << 24;
            *sectors = (uint64_t)e[12] | (uint64_t)e[13] << 8 | (uint64_t)e[14] << 16 | (uint64_t)e[15] << 24;
            ++found;
        }
    }
    return found == 1;
}

static int mount(uint64_t base, uint64_t sectors)
{
    struct fwr_config c = {0};
    c.device = device; c.base = base; c.sectors = sectors;
    c.cluster_buffer = cluster_buffer; c.cluster_buffer_bytes = sizeof cluster_buffer; c.fat_window = fat_window;
    c.recovery_bitmap = recovery_bitmap; c.recovery_bitmap_bytes = sizeof recovery_bitmap;
    c.grow_bytes = 64 * MIB;
    return fwr_mount(&volume, &c);
}

/* Compare the file with the stream from `from` for `bytes`; counts mismatching bytes. */
static void verify(uint64_t bytes)
{
    uint64_t t0 = ss_counter_ticks();
    for (uint64_t off = 0; off < bytes; off += CHUNK) {
        size_t n = bytes - off < CHUNK ? (size_t)(bytes - off) : CHUNK, got = 0;
        if (fwr_read(&volume, PATH, off, check, n, &got) || got != n) { r.status = E_READ; r.detail = off; break; }
        stream_fill(chunk, off, n);
        for (size_t i = 0; i < n; ++i) if (check[i] != chunk[i]) { if (!r.mismatches++) r.first_mismatch = off + i; }
        ss_progress();
    }
    r.read_ticks = ss_counter_ticks() - t0;
}

static void collect(void)
{
    const struct fwr_wear *w = fwr_wear(&volume);
    r.wear_bytes = w->bytes_written; r.wear_data_bytes = w->data_bytes; r.wear_metadata_bytes = w->metadata_bytes; r.wear_writes = w->writes;
    r.wear_aligned = w->au_aligned_writes; r.wear_unaligned = w->unaligned_writes; r.wear_syncs = w->syncs; r.wear_flushes = w->flushes;
    r.recoveries = w->recoveries; r.recovered_clusters = w->recovered_clusters;
    for (unsigned i = 0; i < FWR_HIST; ++i) r.histogram[i] = w->size_histogram[i];
    struct ss_storage_owner_stats s;
    if (ss_storage_owner_stats(&s)) {
        r.owner_elapsed_us = s.elapsed_us; r.owner_asleep_us = s.asleep_us; r.owner_wakes = s.wakes;
        r.irq_cores = s.irq_cores; r.owner_cores = s.owner_cores; r.caller_cores = s.caller_cores;
    }
}

void storage_write_main(const struct ss_grant *grant)
{
    const struct ss_span *bounds = &grant->regions[STORAGE_WRITE_REGION_BOUNDS], *mode = &grant->regions[STORAGE_WRITE_REGION_MODE],
                         *flush = &grant->regions[STORAGE_WRITE_REGION_FLUSH];
    r.tag = TAG; r.counter_hz = 24000000u; r.file_bytes = FILE_BYTES;
    while (!ss_clock_ready()) { ss_progress(); }
    const uint8_t *m = mode->base;
    uint32_t mode_id = m[0] | m[1] << 8 | m[2] << 16 | (uint32_t)m[3] << 24;
    uint64_t cut_bytes = 0;
    for (unsigned i = 0; i < 8; ++i) cut_bytes |= (uint64_t)m[8 + i] << (8 * i);
    r.mode = mode_id;
    r.app_core = grant->main_core; r.guard_us = GUARD_US;
    {
        /* The boot snapshot of ID_AA64ISAR0_EL1 (EL0 cannot read it): the application's core if it was observed, else the first observed
         * Cortex-A76 core (part 0xd0b), which has the same implementation. `observed` holds a bit per observed core and, from bit 32,
         * each core's SHA2 field; `isar0` the register that decided. */
        struct ss_cpu_observation o = {0}, pick = {0};
        int have = 0;
        for (unsigned c = 0; c < 8; ++c) {
            if (!ss_cpu_observations(c, &o)) continue;
            r.observed |= UINT64_C(1) << c | ((o.isar0 >> 12) & 15) << (32 + 4 * c);
            if (c == grant->main_core || (!have && ((o.midr >> 4) & 0xfff) == 0xd0b)) { pick = o; have = (c == grant->main_core) ? 2 : 1; }
        }
        r.isar0 = pick.isar0;
        use_ce = have && ((pick.isar0 >> 12) & 15) != 0;
        r.sha_ce = use_ce;
    }
    r.cpu_hz = cpu_cycles_per_second(&r.cpu_calibration_ticks);
    ss_progress();
    ss_activity_begin(); activity = 1;                          /* the thermal report covers the card work */
    card_open = 1;                                             /* enabling opens the card in the owner task, even if it then refuses */
    if (!ss_storage_enable(bounds->base, bounds->bytes)) { r.status = E_BOUNDS; (void)ss_storage_diagnostic(r.diag); finish_report(); }
    if (!ss_storage_region(flush->base, flush->bytes)) { r.status = E_REGION; (void)ss_storage_diagnostic(r.diag); finish_report(); }
    if (!ss_storage_device(&device)) { r.status = E_DEVICE; (void)ss_storage_diagnostic(r.diag); finish_report(); }
    r.au_sectors = device.au_sectors;
    if (mode_id == 4) { collect(); finish_report(); }                   /* open and enable only (the QEMU boot check) */
    uint64_t base, sectors;
    if (!mbr_partition(&base, &sectors)) { r.status = E_MBR; finish_report(); }
    uint64_t t_start = ss_counter_ticks();
    int rc = mount(base, sectors);
    if (rc) { r.status = E_MOUNT; r.detail = (uint64_t)-rc; finish_report(); }
    if (mode_id == 2 || mode_id == 3) {                              /* read back: the interruption test (2) or the finished file (3) */
        uint64_t size = 0; int dir = 0;
        if (fwr_exists(&volume, PATH, &size, &dir) == FWR_OK) {
            r.volume_size = size;
            if (mode_id == 3 && size != FILE_BYTES) r.status = E_SIZE; else verify(size);
        }
        else r.status = E_OPEN;
        collect(); r.total_ticks = ss_counter_ticks() - t_start;
        (void)fwr_unmount(&volume);
        finish_report();
    }
    int d = fwr_mkdir(&volume, "/sunstone-test");
    if (d && d != FWR_EXISTS) { r.status = E_DIR; r.detail = (uint64_t)-d; finish_report(); }
    uint64_t t0 = ss_counter_ticks();
    rc = fwr_open(&volume, &file, PATH, mode_id == 1 ? (FWR_O_CREATE | FWR_O_APPEND) : FWR_O_CREATE, FILE_BYTES, flush->base, flush->bytes);
    if (rc) { r.status = E_OPEN; r.detail = (uint64_t)-rc; finish_report(); }
    r.create_ticks = ss_counter_ticks() - t0;
    unsigned flushes = 0;
    uint64_t synced = 0;
    int timed_out = 0;
    for (uint64_t off = 0; off < FILE_BYTES; off += CHUNK) {
        uint64_t g0 = ss_counter_ticks();
        stream_fill(chunk, off, CHUNK);
        r.generate_ticks += ss_counter_ticks() - g0;
        size_t put = 0;
        while (put < CHUNK) {
            size_t k = fwr_write(&file, chunk + put, CHUNK - put);
            put += k;
            if (k == CHUNK - put + k && put == CHUNK) break;
            uint64_t f0 = ss_counter_ticks();
            int64_t fl = fwr_service(&file, 0);
            uint64_t dt = ss_counter_ticks() - f0;
            if (fl < 0) { r.status = E_WRITE; r.detail = (uint64_t)-fl; r.written_bytes = off + put; collect(); finish_report(); }
            if (fl > 0) { r.flush_ticks += dt; if (flushes < MAX_FLUSHES) flush_ticks[flushes] = (uint32_t)dt; ++flushes; }
            ss_progress();
        }
        uint64_t f0 = ss_counter_ticks();
        int64_t fl = fwr_service(&file, 0);
        uint64_t dt = ss_counter_ticks() - f0;
        if (fl < 0) { r.status = E_WRITE; r.detail = (uint64_t)-fl; r.written_bytes = off + CHUNK; collect(); finish_report(); }
        if (fl > 0) { r.flush_ticks += dt; if (flushes < MAX_FLUSHES) flush_ticks[flushes] = (uint32_t)dt; ++flushes; }
        r.written_bytes = off + CHUNK;
        if (mode_id == 1 && r.written_bytes - synced >= SYNC_EVERY) {
            if (fwr_sync(&file)) { r.status = E_WRITE; r.detail = 0xffff; finish_report(); }
            synced = r.written_bytes;
        }
        if (mode_id == 1 && r.written_bytes >= cut_bytes) {
            /* Stop reporting progress in the middle of a flush cycle: the watchdog resets the board. Nothing after this
             * line runs in this lifetime. */
            for (;;) { __asm__ volatile("nop"); }
        }
        ss_progress();
        if (mode_id == 0 && (ss_counter_ticks() - t_start) * 1000000u / COUNTER_HZ >= GUARD_US && off + CHUNK < FILE_BYTES) { timed_out = 1; break; }
    }
    uint64_t c0 = ss_counter_ticks();
    rc = fwr_close(&file);
    if (!rc) rc = fwr_unmount(&volume);
    r.close_ticks = ss_counter_ticks() - c0;
    if (rc) { r.status = E_CLOSE; r.detail = (uint64_t)-rc; collect(); finish_report(); }
    r.total_ticks = ss_counter_ticks() - t_start;
    if (timed_out) { r.status = E_TIMEOUT; r.detail = r.written_bytes; }
    unsigned n = flushes < MAX_FLUSHES ? flushes : MAX_FLUSHES;
    r.flushes = flushes;
    if (n) { sort32(flush_ticks, n); r.flush_p50 = flush_ticks[n / 2]; r.flush_p99 = flush_ticks[(n * 99) / 100 < n ? (n * 99) / 100 : n - 1]; r.flush_max = flush_ticks[n - 1]; }
    collect();
    finish_report();                                                /* mode 3 reads the file back in a run of its own */
}
