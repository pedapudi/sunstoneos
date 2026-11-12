/* Host model for the storage-write test's time guard: apps/storage-write-test/main.c is compiled unchanged except for a
 * scaled-down file and guard (FILE_BYTES, GUARD_US), against a file-backed card image whose writes are slow (a fixed
 * sleep per write call) and a stub of the OS kit. The model refuses writes outside the FAT copies and data area, and
 * ss_finish stores the 512-byte result in the file named by RESULT and exits. In mode 10 the model admits writes only inside the header's
 * extent. Usage:
 *   storage-write-guard IMAGE BASE SECTORS MODE RESULT DELAY_US */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "../apps/storage-write-test/main.c"
#include <fcntl.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static int image_fd;
static uint64_t dev_sectors, part_base, part_sectors, delay_us;
static const char *result_path;
static unsigned format_certified, format_serial;
static uint64_t region_fat1, region_fat2, region_data, region_end, region_fsinfo, region_boot = UINT64_MAX;   /* the boot sector is writable only in the format mode */
static uint64_t extent_first, extent_end;     /* mode 10: the header's extent, the only sectors it may write */

/* The model's clock is virtual: it advances only by the waits the application asks for (ss_usb_wait) and by the modeled card's
 * time per write call. Host load therefore never moves a deadline relative to the writes: a session that must be cut after N
 * write calls is cut after N write calls on any host. */
static uint64_t virtual_us;
uint64_t ss_counter_ticks(void) { return virtual_us * 24u; }                               /* 24 MHz */
int ss_clock_ready(void) { return 1; }
static uint64_t finish_by_us;             /* the session's own deadline in the counter's microseconds; 0 none */
static int cut_enabled;
static uint64_t now_us(void) { return virtual_us; }
static void maybe_cut(void) { if (cut_enabled && finish_by_us && now_us() >= finish_by_us) _exit(77); }       /* the OS ends the session at its deadline */
void ss_progress(void) { maybe_cut(); }
void ss_usb_wait(uint32_t us)
{
    virtual_us += us; maybe_cut();
    if (!cut_enabled && finish_by_us && virtual_us > finish_by_us + 10000000u) _exit(78);        /* a deadline with no cut: the session would wait forever */
}
void ss_activity_begin(void) {}
void ss_activity_end(int success) { (void)success; }
void sha256_block_ce(const uint8_t b[64], uint8_t d[32]) { (void)b; (void)d; abort(); }          /* the CE path is verified in QEMU */
int ss_cpu_observations(unsigned core, struct ss_cpu_observation *o) { (void)core; memset(o, 0, sizeof *o); return 1; }
int ss_storage_enable(const void *b, size_t n) { (void)b; (void)n; return 1; }
int ss_storage_region(void *b, size_t n) { (void)b; (void)n; return 1; }
int ss_storage_close(void) { if ((r.mode == 9 || r.mode == 10) && !r.status) assert(format_certified); return 1; }
int ss_storage_open_read(void) { return 1; }
int ss_storage_diagnostic(uint64_t out[4]) { memset(out, 0, 32); return 1; }
int ss_storage_mode(uint64_t out[5]) { memset(out, 0, 40); return 1; }
int ss_storage_owner_stats(struct ss_storage_owner_stats *s) { memset(s, 0, sizeof *s); s->irq_cores = s->owner_cores = s->caller_cores = 1; return 1; }
void ss_finish(const void *res, size_t bytes)
{
    FILE *f = fopen(result_path, "wb");
    if (!f || fwrite(res, 1, bytes, f) != bytes) exit(5);
    fclose(f);
    exit(0);
}

static int dev_read(void *c, uint64_t s, uint32_t n, uint8_t *buf)
{
    (void)c;
    if (s == 0 && n == 1) {                                  /* a synthesized MBR: one FAT32 LBA partition */
        memset(buf, 0, 512);
        uint8_t *e = buf + 446;
        e[4] = 0x0c;
        for (unsigned i = 0; i < 4; ++i) { e[8 + i] = (uint8_t)(part_base >> (8 * i)); e[12 + i] = (uint8_t)(part_sectors >> (8 * i)); }
        buf[510] = 0x55; buf[511] = 0xaa;
        return 1;
    }
    if (s + n > dev_sectors) { fprintf(stderr, "read outside device\n"); exit(3); }
    if (getenv("READ_LOG")) {                                    /* every read call: first sector and count, for the command-count check */
        FILE *log = fopen(getenv("READ_LOG"), "a");
        if (log) { fprintf(log, "%llu %u\n", (unsigned long long)s, n); fclose(log); }
    }
    return pread(image_fd, buf, (size_t)n * 512, (off_t)s * 512) == (ssize_t)n * 512;
}
static int allowed(uint64_t s)
{
    uint64_t fsz = region_fat2 - region_fat1;
    if (r.mode == 10) return s >= extent_first && s < extent_end;
    return (s >= region_fat1 && s < region_fat2 + fsz) || s == region_fsinfo || s == region_boot || (s >= region_data && s < region_end);
}
static int dev_write(void *c, uint64_t s, uint32_t n, const uint8_t *buf)
{
    (void)c;
    format_certified = 0;
    if (s + n > dev_sectors) { fprintf(stderr, "write outside device\n"); exit(3); }
    for (uint32_t i = 0; i < n; ++i)
        if (!allowed(s + i)) { fprintf(stderr, "model refused write to sector %llu\n", (unsigned long long)(s + i)); exit(4); }
    if (getenv("WRITE_LOG")) {                                   /* every write call: first sector and count, for the geometry check */
        FILE *log = fopen(getenv("WRITE_LOG"), "a");
        if (log) { fprintf(log, "%llu %u\n", (unsigned long long)s, n); fclose(log); }
    }
    if (cut_enabled && finish_by_us && now_us() >= finish_by_us) {      /* cut in the middle of a write: the first half of its sectors reach the card */
        uint32_t half = (n + 1) / 2;
        (void)pwrite(image_fd, buf, (size_t)half * 512, (off_t)s * 512);
        _exit(77);
    }
    virtual_us += delay_us;                                  /* the slow card */
    return pwrite(image_fd, buf, (size_t)n * 512, (off_t)s * 512) == (ssize_t)n * 512;
}
static int format_transaction(void *c, unsigned event, unsigned operation, uint32_t *token)
{
    (void)c;
    if (event == FWR_TX_BEGIN) { *token = ++format_serial; format_certified = 0; }
    else format_certified = event == FWR_TX_COMMIT && operation == FWR_OP_CHECKPOINT;
    return 1;
}
static int format_write(void *c, uint64_t sector, uint32_t count, const uint8_t *data, unsigned phase, uint32_t token)
{ (void)phase; (void)token; return dev_write(c, sector, count, data); }
int ss_storage_device(struct fwr_device *d)
{
    *d = (struct fwr_device){.context = NULL, .read = dev_read, .write = dev_write, .yield = NULL, .sectors = dev_sectors,
                             .au_sectors = getenv("AU_SECTORS") ? (uint32_t)strtoul(getenv("AU_SECTORS"), 0, 0) : 256, .max_write_sectors = 256};
    if (r.mode == 9 || r.mode == 10) { d->transaction = format_transaction; d->write_classified = format_write; }
    return 1;
}

static uint8_t bounds_blob[65536], mode_blob[254464] __attribute__((aligned(64)));
static size_t bounds_bytes = 64, mode_bytes = 4096;
/* BOUNDS and MODE_INPUT name files that replace the inputs (mode 10 reads its extent and candidate from them). */
static size_t load(const char *name, uint8_t *to, size_t limit, size_t otherwise)
{
    const char *path = getenv(name);
    if (!path) return otherwise;
    FILE *f = fopen(path, "rb");
    if (!f) exit(2);
    size_t n = fread(to, 1, limit, f);
    if (!feof(f) && fgetc(f) != EOF) exit(2);
    fclose(f);
    return n;
}
static uint8_t flush_buffer[16 << 20] __attribute__((aligned(64)));

int main(int argc, char **argv)
{
    if (argc != 7) return 2;
    image_fd = open(argv[1], O_RDWR);
    if (image_fd < 0) return 2;
    part_base = strtoull(argv[2], 0, 0); part_sectors = strtoull(argv[3], 0, 0);
    dev_sectors = (uint64_t)lseek(image_fd, 0, SEEK_END) / 512;
    mode_blob[0] = (uint8_t)atoi(argv[4]);
    bounds_bytes = load("BOUNDS", bounds_blob, sizeof bounds_blob, bounds_bytes);
    mode_bytes = load("MODE_INPUT", mode_blob, sizeof mode_blob, mode_bytes);
    if (mode_blob[0] == 10) {
        for (unsigned i = 0; i < 8; ++i) extent_first |= (uint64_t)mode_blob[8 + i] << (8 * i);
        extent_end = extent_first + (mode_bytes > 512 ? (mode_bytes - 512) / 512 : 0);
    }
    result_path = argv[5]; delay_us = strtoull(argv[6], 0, 0);
    uint8_t b[512];
    if (pread(image_fd, b, 512, (off_t)part_base * 512) != 512) return 2;
    uint32_t reserved = b[14] | b[15] << 8, fats = b[16], fsz = b[36] | b[37] << 8 | b[38] << 16 | (uint32_t)b[39] << 24;
    uint32_t info = b[48] | b[49] << 8;
    region_fat1 = part_base + reserved; region_fat2 = region_fat1 + fsz;
    region_fsinfo = info ? part_base + info : UINT64_MAX;
    if (mode_blob[0] == 9) region_boot = part_base;
    region_data = part_base + reserved + (uint64_t)fats * fsz; region_end = part_base + part_sectors;
    struct ss_grant g = {0};
    if (getenv("FINISH_AFTER_US")) { finish_by_us = now_us() + strtoull(getenv("FINISH_AFTER_US"), 0, 0); cut_enabled = getenv("CUT") != NULL; g.finish_by_us = finish_by_us; }
    /* The grant points at a span table in manifest order (ABI version 2). */
    static struct ss_span spans[] = {
        [STORAGE_WRITE_REGION_BOUNDS] = {bounds_blob, 0},
        [STORAGE_WRITE_REGION_MODE] = {mode_blob, 0},
        [STORAGE_WRITE_REGION_FLUSH] = {flush_buffer, sizeof flush_buffer},
    };
    spans[STORAGE_WRITE_REGION_BOUNDS].bytes = bounds_bytes; spans[STORAGE_WRITE_REGION_MODE].bytes = mode_bytes;
    g.regions = spans;
    g.region_count = sizeof spans / sizeof spans[0];
    storage_write_main(&g);
    return 6;
}
