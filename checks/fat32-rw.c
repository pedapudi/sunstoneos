/* Host model for the FAT32 write service: a file-backed card with a write log and an independent
 * write guard. Commands (see checks/fat32-rw.py):
 *   session IMAGE BASE SECTORS LOG AU SIZE                create, write SIZE bytes, sync, close and unmount one file, logging every sector write
 *   recover IMAGE BASE SECTORS AU [LOG [CUT]]             mount (recovering), unmount; optional log and cut
 *   serve IMAGE BASE SECTORS AU                           the same recovery, repeatedly, on one open image: reads
 *                                                         "recover [LOG [CUT]]" and "undo" lines from standard input
 *   accounting IMAGE BASE SECTORS AU                      small appends become allocation-unit writes
 *   guard IMAGE BASE SECTORS AU                           the write guard at every region boundary */
#define _FILE_OFFSET_BITS 64
#include "fat32_rw.h"
#include "sunstone_fs.h"
#include <errno.h>
#include <sys/stat.h>
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int image_fd = -1;
static FILE *wlog;
static uint64_t sector_writes;          /* sectors written so far */
static uint64_t cut_at = UINT64_MAX;    /* writes beyond this many sectors are lost and fail */
static int cut_hit;
static uint64_t dev_sectors, region_fat1, region_fat2, region_fsinfo, region_data, region_end, part_base;
static unsigned yields, writes_without_yield;
static uint32_t call_id;
static int expect_yield;
static uint64_t call_log[1 << 17][2]; static unsigned calls;
static int log_calls;
/* serve mode: the sectors a recovery changed, with their earlier contents, so that "undo" restores the image. */
#define UNDO_MAX 8192
static uint64_t undo_sector[UNDO_MAX];
static uint8_t (*undo_data)[512];
static unsigned undo_count;
static int undo_on;

/* The model parses the boot sector itself and refuses any write outside the FAT copies, the
 * FSInfo sector and the data area, whatever the service decides. */
static void model_regions(void)
{
    uint8_t b[512];
    assert(pread(image_fd, b, 512, (off_t)part_base * 512) == 512);
    uint32_t reserved = b[14] | b[15] << 8, fats = b[16], fsz = b[36] | b[37] << 8 | b[38] << 16 | (uint32_t)b[39] << 24;
    uint32_t total = b[32] | b[33] << 8 | b[34] << 16 | (uint32_t)b[35] << 24, info = b[48] | b[49] << 8;
    region_fat1 = part_base + reserved; region_fat2 = region_fat1 + fsz;
    region_fsinfo = info ? part_base + info : UINT64_MAX;
    region_data = part_base + reserved + (uint64_t)fats * fsz; region_end = part_base + total;
}

static int dev_read(void *c, uint64_t s, uint32_t n, uint8_t *buf)
{
    (void)c;
    if (s + n > dev_sectors) { fprintf(stderr, "read outside device\n"); exit(3); }
    return pread(image_fd, buf, (size_t)n * 512, (off_t)s * 512) == (ssize_t)n * 512;
}

static int allowed(uint64_t s)
{
    return (s >= region_fat1 && s < region_fat1 + (region_fat2 - region_fat1)) ||
           (s >= region_fat2 && s < region_fat2 + (region_fat2 - region_fat1)) ||
           s == region_fsinfo || (s >= region_data && s < region_end);
}

static int dev_write(void *c, uint64_t s, uint32_t n, const uint8_t *buf)
{
    (void)c;
    if (expect_yield && !yields) ++writes_without_yield;
    yields = 0; ++call_id;
    if (s + n > dev_sectors) { fprintf(stderr, "write outside device\n"); exit(3); }
    if (log_calls && calls < (1u << 17)) { call_log[calls][0] = s; call_log[calls][1] = n; ++calls; }
    for (uint32_t i = 0; i < n; ++i) {
        if (!allowed(s + i)) { fprintf(stderr, "model refused write to sector %llu\n", (unsigned long long)(s + i)); exit(4); }
        if (sector_writes >= cut_at) { cut_hit = 1; return 0; }
        if (wlog) {
            uint64_t sec = s + i, head = (uint64_t)call_id << 32;
            fwrite(&sec, 8, 1, wlog); fwrite(&head, 8, 1, wlog);
            fwrite(buf + (size_t)i * 512, 512, 1, wlog);
        }
        if (undo_on) {
            unsigned j = 0;
            while (j < undo_count && undo_sector[j] != s + i) ++j;
            if (j == undo_count) {
                assert(undo_count < UNDO_MAX);
                assert(pread(image_fd, undo_data[j], 512, (off_t)(s + i) * 512) == 512);
                undo_sector[undo_count++] = s + i;
            }
        }
        assert(pwrite(image_fd, buf + (size_t)i * 512, 512, (off_t)(s + i) * 512) == 512);
        ++sector_writes;
    }
    return 1;
}
static void dev_yield(void *c) { (void)c; ++yields; }

static uint8_t cluster_buf[65536] __attribute__((aligned(64)));
static uint8_t window[2 * FWR_FAT_WINDOW_SECTORS * 512] __attribute__((aligned(64)));
static uint8_t *bitmap;

static struct fwr_config make_config(uint64_t base, uint64_t sectors, uint32_t au, uint32_t maxw)
{
    struct fwr_config c = {0};
    c.device = (struct fwr_device){.read = dev_read, .write = dev_write, .yield = dev_yield, .sectors = dev_sectors,
                                   .au_sectors = au, .max_write_sectors = maxw};
    c.base = base; c.sectors = sectors;
    c.cluster_buffer = cluster_buf; c.cluster_buffer_bytes = sizeof cluster_buf; c.fat_window = window;
    bitmap = calloc(1, 1 << 20);
    c.recovery_bitmap = bitmap; c.recovery_bitmap_bytes = 1 << 20;
    return c;
}

static uint8_t stream_byte(uint64_t seed, uint64_t i)
{
    uint64_t x = (i >> 3) + seed * 0x9e3779b97f4a7c15ull;
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull; x ^= x >> 27; x *= 0x94d049bb133111ebull; x ^= x >> 31;
    return (uint8_t)(x >> (8 * (i & 7)));
}
static void stream_fill(uint8_t *p, uint64_t seed, uint64_t offset, size_t n)
{ for (size_t i = 0; i < n; ++i) p[i] = stream_byte(seed, offset + i); }

#define CHECK(x) do { int r_ = (x); if (r_ < 0 && !cut_hit) { fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_); exit(2); } \
                      if (r_ < 0) return 1; } while (0)

static int write_all(struct fwr_file *f, uint64_t seed, uint64_t offset, uint64_t n, size_t chunk)
{
    uint8_t buf[70000];
    while (n) {
        size_t m = n < chunk ? (size_t)n : chunk;
        stream_fill(buf, seed, offset, m);
        size_t put = 0;
        while (put < m) {
            size_t k = fwr_write(f, buf + put, m - put);
            put += k;
            if (!k || put < m) { int64_t r = fwr_service(f, 0); if (r < 0) return (int)r; }
        }
        int64_t r = fwr_service(f, 0);
        if (r < 0) return (int)r;
        offset += m; n -= m;
    }
    return 0;
}

static uint8_t fbuf1[1 << 18] __attribute__((aligned(64))), fbuf2[8192];

static void open_image(const char *img, uint64_t base)
{
    image_fd = open(img, O_RDWR); assert(image_fd >= 0);
    dev_sectors = (uint64_t)lseek(image_fd, 0, SEEK_END) / 512; part_base = base;
    model_regions();
}

static int cmd_session(const char *img, uint64_t base, uint64_t sectors, const char *logp, uint32_t au, uint64_t size)
{
    open_image(img, base);
    struct fwr_config cfg = make_config(base, sectors, au, 0);
    static struct fwr_volume v;
    int r = fwr_mount(&v, &cfg); assert(!r);
    wlog = fopen(logp, "wb"); assert(wlog);
    sector_writes = 0;
    struct fwr_file f;
    CHECK(fwr_open(&v, &f, "SESSION.BIN", FWR_O_CREATE, 0, fbuf2, sizeof fbuf2));
    CHECK(write_all(&f, 7, 0, size, 1000));
    CHECK(fwr_sync(&f));
    CHECK(fwr_close(&f));
    CHECK(fwr_unmount(&v));
    fclose(wlog);
    printf("session bytes=%llu sector_writes=%llu\n", (unsigned long long)size, (unsigned long long)sector_writes);
    return 0;
}

static int recover_once(uint64_t base, uint64_t sectors, uint32_t au, const char *logp, uint64_t cut)
{
    sector_writes = 0; cut_hit = 0; yields = 0; writes_without_yield = 0; call_id = 0; expect_yield = 0;
    if (logp) wlog = fopen(logp, "wb");
    cut_at = cut;
    struct fwr_config cfg = make_config(base, sectors, au, 0);
    static struct fwr_volume v;
    memset(&v, 0, sizeof v);
    int r = fwr_mount(&v, &cfg);
    int status = 0;
    if (r) {
        printf("mount failed %d cut=%d\n", r, cut_hit);
        status = cut_hit ? 0 : 1;
    } else {
        r = fwr_unmount(&v);
        if (r && !cut_hit) { printf("unmount failed %d\n", r); status = 1; }
        else printf("recover sector_writes=%llu recoveries=%llu freed=%llu cut=%d\n", (unsigned long long)sector_writes,
                    (unsigned long long)v.wear.recoveries, (unsigned long long)v.wear.recovered_clusters, cut_hit);
    }
    if (wlog) { fclose(wlog); wlog = NULL; }
    free(bitmap); bitmap = NULL;
    return status;
}

static int cmd_recover(const char *img, uint64_t base, uint64_t sectors, uint32_t au, const char *logp, uint64_t cut)
{
    open_image(img, base);
    return recover_once(base, sectors, au, logp, cut);
}

static int cmd_serve(const char *img, uint64_t base, uint64_t sectors, uint32_t au)
{
    char line[4096];
    open_image(img, base);
    undo_data = malloc((size_t)UNDO_MAX * 512); assert(undo_data);
    undo_on = 1;
    while (fgets(line, sizeof line, stdin)) {
        char verb[16] = "", logp[3000] = "";
        unsigned long long cut = UINT64_MAX;
        int n = sscanf(line, "%15s %2999s %llu", verb, logp, &cut);
        if (n >= 1 && !strcmp(verb, "undo")) {
            while (undo_count) {
                --undo_count;
                assert(pwrite(image_fd, undo_data[undo_count], 512, (off_t)undo_sector[undo_count] * 512) == 512);
            }
            puts("undone");
        } else if (n >= 1 && !strcmp(verb, "recover")) {
            if (recover_once(base, sectors, au, n >= 2 ? logp : NULL, n >= 3 ? cut : UINT64_MAX)) puts("recover failed");
        } else {
            puts("unknown command");
        }
        fflush(stdout);
    }
    return 0;
}

static int cmd_accounting(const char *img, uint64_t base, uint64_t sectors, uint32_t au)
{
    image_fd = open(img, O_RDWR); assert(image_fd >= 0);
    dev_sectors = (uint64_t)lseek(image_fd, 0, SEEK_END) / 512; part_base = base;
    model_regions();
    struct fwr_config cfg = make_config(base, sectors, au, 256);
    static struct fwr_volume v;
    assert(!fwr_mount(&v, &cfg));
    expect_yield = 1; log_calls = 1;
    struct fwr_file f;
    assert(!fwr_open(&v, &f, "log.bin", FWR_O_CREATE, 4u << 20, fbuf1, sizeof fbuf1));
    /* 100-byte appends, 20000 of them, no sync. */
    uint8_t rec[100];
    uint64_t seed = 77, off = 0;
    for (unsigned i = 0; i < 20000; ++i) {
        stream_fill(rec, seed, off, sizeof rec); off += sizeof rec;
        size_t put = 0;
        while (put < sizeof rec) { size_t k = fwr_write(&f, rec + put, sizeof rec - put); put += k; if (!k) assert(fwr_service(&f, 0) >= 0); }
        assert(fwr_service(&f, 0) >= 0);
    }
    unsigned data_calls = calls;
    uint64_t data_bytes = v.wear.data_bytes;
    unsigned bad = 0;
    for (unsigned i = 0; i < data_calls; ++i) {
        uint64_t s = call_log[i][0], n = call_log[i][1];
        /* A data write ends on an allocation unit boundary; only the card's write limit splits a unit. */
        if (((s + n) % au) != 0) ++bad;
    }
    assert(!fwr_close(&f));
    assert(!fwr_unmount(&v));
    printf("accounting appends=20000 bytes=%llu data_writes_before_close=%u data_bytes_before_close=%llu "
           "unaligned_ends=%u writes_without_yield=%u au_sectors=%u total_writes=%llu\n",
           (unsigned long long)off, data_calls, (unsigned long long)data_bytes, bad, writes_without_yield, au,
           (unsigned long long)v.wear.writes);
    printf("histogram");
    for (unsigned i = 0; i < FWR_HIST; ++i) printf(" %llu", (unsigned long long)v.wear.size_histogram[i]);
    printf("\n");
    return bad || writes_without_yield ? 1 : 0;
}

/* The newlib system calls, compiled under host_ names (drivers/storage/sunstone_fs_syscalls.c). */
int host__open(const char *, int, int); int host__close(int); int host__write(int, const char *, int);
int host__read(int, char *, int); long host__lseek(int, long, int); int host__fstat(int, struct stat *);
int host__rename(const char *, const char *); int host__unlink(const char *); int host_fsync(int);
int host_ftruncate(int, off_t); int host_posix_fallocate(int, off_t, off_t);
void ss_syscalls_bind(struct ss_fs *);

static uint8_t posix_pool[SS_FS_MAX_FILES][1 << 16] __attribute__((aligned(64)));

static int file_equals(struct fwr_volume *v, const char *path, uint64_t seed, uint64_t size)
{
    uint8_t got[4096], want[4096];
    uint64_t sz; int dir;
    if (fwr_exists(v, path, &sz, &dir) || sz != size) return 0;
    for (uint64_t o = 0; o < size; o += sizeof got) {
        size_t n = size - o < sizeof got ? (size_t)(size - o) : sizeof got, g;
        if (fwr_read(v, path, o, got, n, &g) || g != n) return 0;
        stream_fill(want, seed, o, n);
        if (memcmp(got, want, n)) return 0;
    }
    return 1;
}

static int cmd_posix(const char *img, uint64_t base, uint64_t sectors, uint32_t au)
{
    image_fd = open(img, O_RDWR); assert(image_fd >= 0);
    dev_sectors = (uint64_t)lseek(image_fd, 0, SEEK_END) / 512; part_base = base;
    model_regions();
    struct fwr_config cfg = make_config(base, sectors, au, 256);
    static struct fwr_volume v; static struct ss_fs fs;
    assert(!fwr_mount(&v, &cfg));
    assert(!ss_fs_init(&fs, &v, &posix_pool[0][0], sizeof posix_pool[0]));
    ss_syscalls_bind(&fs);
    unsigned fails = 0;
#define T(cond, what) do { if (!(cond)) { printf("posix FAIL %s (errno %d)\n", what, errno); ++fails; } } while (0)
    uint8_t buf[3000];
    /* fopen("a")/fwrite/fflush/fsync/fclose equivalents. */
    T(ss_mkdir(&fs, "/logs") == 0, "mkdir");
    int fd = host__open("/logs/Run-0001.log", SS_O_WRONLY | SS_O_CREAT | SS_O_APPEND, 0644);
    T(fd >= 3, "open append create");
    uint64_t off = 0;
    for (int i = 0; i < 40; ++i) { stream_fill(buf, 5, off, sizeof buf); T(host__write(fd, (char *)buf, sizeof buf) == (int)sizeof buf, "write"); off += sizeof buf; }
    T(host__lseek(fd, 0, SS_SEEK_CUR) == (long)off, "ftell");
    T(host__lseek(fd, 0, SS_SEEK_SET) == -1 && errno == ESPIPE, "seek back refused");
    uint64_t writes_before = v.wear.writes;
    T(host_fsync(fd) == 0, "fsync");
    T(v.wear.writes > writes_before, "fsync wrote");
    T(file_equals(&v, "/logs/Run-0001.log", 5, off), "durable after fsync");
    /* A would-block write never touches the card. */
    uint64_t w0 = v.wear.writes; size_t pushed = 0; ssize_t k;
    while ((k = ss_write_nowait(&fs, fd, buf, sizeof buf)) > 0) pushed += (size_t)k;
    T(k == 0 && pushed > 0 && v.wear.writes == w0, "nowait does not touch the card");
    T(host__close(fd) == 0, "close");
    uint64_t total = off + pushed; (void)total;
    /* Append resumes at the end. */
    fd = host__open("/logs/Run-0001.log", SS_O_WRONLY | SS_O_APPEND, 0);
    T(fd >= 3, "reopen append");
    struct stat st; T(host__fstat(fd, &st) == 0 && (uint64_t)st.st_size == off + pushed, "fstat size");
    T(host__close(fd) == 0, "close again");
    /* O_TRUNC replaces atomically; O_CREAT on a non-empty file refuses an in-place overwrite. */
    T(host__open("/logs/Run-0001.log", SS_O_WRONLY | SS_O_CREAT, 0) == -1 && errno == ENOTSUP, "overwrite refused");
    T(host__open("/logs/Run-0001.log", SS_O_WRONLY | SS_O_CREAT | SS_O_EXCL, 0) == -1 && errno == EEXIST, "excl");
    fd = host__open("/logs/Run-0001.log", SS_O_WRONLY | SS_O_CREAT | SS_O_TRUNC, 0);
    T(fd >= 3, "open trunc");
    stream_fill(buf, 6, 0, 1500); T(host__write(fd, (char *)buf, 1500) == 1500, "write new body");
    { uint64_t cur; T(!fwr_exists(&v, "/logs/Run-0001.log", &cur, NULL) && cur == off + pushed, "old body visible before fsync"); }
    T(host_fsync(fd) == 0 && host__close(fd) == 0, "fsync close");
    T(file_equals(&v, "/logs/Run-0001.log", 6, 1500), "new body after fsync");
    /* tmp, fsync, rename over the target. */
    fd = host__open("/logs/cfg.tmp", SS_O_WRONLY | SS_O_CREAT | SS_O_EXCL, 0);
    stream_fill(buf, 7, 0, 900); T(host__write(fd, (char *)buf, 900) == 900, "tmp write");
    T(host_fsync(fd) == 0 && host__close(fd) == 0, "tmp fsync");
    T(host__rename("/logs/cfg.tmp", "/logs/Run-0001.log") == 0, "rename over");
    T(file_equals(&v, "/logs/Run-0001.log", 7, 900), "renamed content");
    T(host__open("/logs/cfg.tmp", SS_O_RDONLY, 0) == -1 && errno == ENOENT, "source gone");
    T(host__rename("/logs/Run-0001.log", "/other/x") == -1, "missing directory");
    /* ftruncate and posix_fallocate. */
    fd = host__open("/logs/Run-0001.log", SS_O_WRONLY | SS_O_APPEND, 0);
    T(host_ftruncate(fd, 400) == 0, "ftruncate shrink");
    T(file_equals(&v, "/logs/Run-0001.log", 7, 400), "shrunk content");
    T(host_posix_fallocate(fd, 0, 100000) == 0, "posix_fallocate extends");
    uint64_t sz; T(!fwr_exists(&v, "/logs/Run-0001.log", &sz, NULL) && sz == 100000, "fallocate size");
    uint8_t z[512], got[512]; size_t g; memset(z, 0, sizeof z);
    T(!fwr_read(&v, "/logs/Run-0001.log", 99488, got, 512, &g) && !memcmp(got, z, 512), "fallocate zero fill");
    T(ss_fallocate_keep(&fs, fd, 1u << 20) == 0, "fallocate keep");
    T(!fwr_exists(&v, "/logs/Run-0001.log", &sz, NULL) && sz == 100000, "keep size unchanged");
    T(host__close(fd) == 0, "close fallocate");
    T(ss_unlink(&fs, "/logs/Run-0001.log") == 0, "unlink");
    T(ss_stat(&fs, "/logs/Run-0001.log", &(struct ss_stat){0}) == -ENOENT, "stat after unlink");
    for (int i = 0; i < 9; ++i) { char name[32]; snprintf(name, sizeof name, "/logs/f%d", i); int d = ss_open(&fs, name, SS_O_WRONLY | SS_O_CREAT); if (i < 8) T(d >= 3, "open many"); else T(d == -EMFILE, "descriptor limit"); }
    for (int d = 3; d < 3 + (int)SS_FS_MAX_FILES; ++d) T(ss_close(&fs, d) == 0, "close many");
    assert(!fwr_unmount(&v));
    printf("posix checks done fails=%u\n", fails);
    return fails != 0;
}

static uint8_t big_buffer[16u << 20] __attribute__((aligned(64)));

/* Create /sunstone-test/<name> of `size` bytes on a volume and print the sector ranges written. */
static int cmd_plan(const char *img, uint64_t base, uint64_t sectors, uint32_t au, uint64_t size)
{
    image_fd = open(img, O_RDWR); assert(image_fd >= 0);
    dev_sectors = (uint64_t)lseek(image_fd, 0, SEEK_END) / 512; part_base = base;
    model_regions();
    struct fwr_config cfg = make_config(base, sectors, au, 256);
    static struct fwr_volume v;
    assert(!fwr_mount(&v, &cfg));
    log_calls = 1; expect_yield = 1;
    assert(!fwr_mkdir(&v, "/sunstone-test"));
    struct fwr_file f;
    assert(!fwr_open(&v, &f, "/sunstone-test/sunstone-region-test.bin", FWR_O_CREATE, size, big_buffer, sizeof big_buffer));
    static uint8_t chunk[1 << 20];
    for (uint64_t o = 0; o < size; o += sizeof chunk) {
        size_t n = size - o < sizeof chunk ? (size_t)(size - o) : sizeof chunk;
        stream_fill(chunk, 99, o, n);
        size_t put = 0;
        while (put < n) { size_t k = fwr_write(&f, chunk + put, n - put); put += k; if (put < n) assert(fwr_service(&f, 0) >= 0); }
        assert(fwr_service(&f, 0) >= 0);
    }
    assert(!fwr_close(&f));
    assert(!fwr_unmount(&v));
    /* Merge the logged calls into ranges. */
    unsigned n = calls;
    printf("calls=%u sector_writes=%llu writes_without_yield=%u\n", n, (unsigned long long)sector_writes, writes_without_yield);
    uint64_t lo = call_log[0][0], hi = lo + call_log[0][1];
    for (unsigned i = 1; i <= n; ++i) {
        if (i < n && call_log[i][0] == hi) { hi += call_log[i][1]; continue; }
        printf("range %llu %llu %llu\n", (unsigned long long)lo, (unsigned long long)(hi - 1), (unsigned long long)(hi - lo));
        if (i < n) { lo = call_log[i][0]; hi = lo + call_log[i][1]; }
    }
    const struct fwr_wear *w = &v.wear;
    printf("wear bytes=%llu data=%llu metadata=%llu writes=%llu aligned=%llu unaligned=%llu\n", (unsigned long long)w->bytes_written,
           (unsigned long long)w->data_bytes, (unsigned long long)w->metadata_bytes, (unsigned long long)w->writes,
           (unsigned long long)w->au_aligned_writes, (unsigned long long)w->unaligned_writes);
    return 0;
}

static int cmd_guard(const char *img, uint64_t base, uint64_t sectors, uint32_t au)
{
    image_fd = open(img, O_RDWR); assert(image_fd >= 0);
    dev_sectors = (uint64_t)lseek(image_fd, 0, SEEK_END) / 512; part_base = base;
    model_regions();
    struct fwr_config cfg = make_config(base, sectors, au, 0);
    static struct fwr_volume v;
    assert(!fwr_mount(&v, &cfg));
    uint64_t fat1 = v.fat1, fat2 = v.fat2, fsz = v.fat_sectors, data = v.data_start, end = data + (uint64_t)v.clusters * v.spc;
    unsigned fails = 0;
#define EXPECT(cond, what) do { if (!(cond)) { printf("guard FAIL %s\n", what); ++fails; } } while (0)
    EXPECT(!fwr_write_allowed(&v, base, 1), "boot sector");
    EXPECT(!fwr_write_allowed(&v, base + 1, 1) || v.fsinfo == base + 1, "sector 1");
    EXPECT(fwr_write_allowed(&v, v.fsinfo, 1), "fsinfo");
    EXPECT(!fwr_write_allowed(&v, v.fsinfo, 2), "fsinfo plus one");
    EXPECT(!fwr_write_allowed(&v, fat1 - 1, 1), "before fat1");
    EXPECT(fwr_write_allowed(&v, fat1, 1), "fat1 first");
    EXPECT(fwr_write_allowed(&v, fat1 + fsz - 1, 1), "fat1 last");
    EXPECT(!fwr_write_allowed(&v, fat1 + fsz - 1, 2), "fat1 into fat2");
    EXPECT(fwr_write_allowed(&v, fat2, 1), "fat2 first");
    EXPECT(fwr_write_allowed(&v, fat2 + fsz - 1, 1), "fat2 last");
    EXPECT(fwr_write_allowed(&v, data, 1), "data first");
    EXPECT(fwr_write_allowed(&v, end - 1, 1), "data last");
    EXPECT(!fwr_write_allowed(&v, end, 1), "after data");
    EXPECT(!fwr_write_allowed(&v, end - 1, 2), "data past the end");
    if (data > fat2 + fsz) EXPECT(!fwr_write_allowed(&v, fat2 + fsz, 1), "gap between the FAT copies and the data");
    EXPECT(!fwr_write_allowed(&v, 0, 1), "device sector zero");
    EXPECT(!fwr_write_allowed(&v, dev_sectors, 1), "past device");
    EXPECT(!fwr_write_allowed(&v, data, 0), "empty write");
    EXPECT(!fwr_write_allowed(&v, UINT64_MAX - 1, 4), "wraparound");
    /* A floor narrows the data area. */
    cfg.write_floor = data + 1000;
    static struct fwr_volume v2;
    assert(!fwr_mount(&v2, &cfg));
    EXPECT(!fwr_write_allowed(&v2, data, 1), "below the floor");
    EXPECT(fwr_write_allowed(&v2, data + 1000, 1), "at the floor");
    EXPECT(fwr_write_allowed(&v2, fat1, 1), "floor leaves the FAT");
    printf("guard checks done fails=%u\n", fails);
    return fails != 0;
}

/* Sequential small reads: the work (path resolutions and chain links followed) must be linear in the file size with a cursor,
 * and a cursor must never serve a stale size or chain after the file changes. Prints the figures; the Python side asserts. */
static int cmd_cursor(const char *img, uint64_t base, uint64_t sectors, uint32_t au, uint64_t size)
{
    image_fd = open(img, O_RDWR); assert(image_fd >= 0);
    dev_sectors = (uint64_t)lseek(image_fd, 0, SEEK_END) / 512; part_base = base;
    model_regions();
    struct fwr_config cfg = make_config(base, sectors, au, 256);
    static struct fwr_volume v;
    assert(!fwr_mount(&v, &cfg));
    struct fwr_file f;
    CHECK(fwr_open(&v, &f, "/c.bin", FWR_O_CREATE, size, fbuf1, sizeof fbuf1));
    CHECK(write_all(&f, 5, 0, size, 4096));
    CHECK(fwr_close(&f));
    uint8_t got[1000], want[1000];
    unsigned bad = 0;
    /* 1: the whole-call API, one resolution and one walk from the start per call. */
    uint64_t r0 = v.wear.read_resolves, s0 = v.wear.read_chain_steps;
    struct fwr_cursor none;
    for (uint64_t o = 0; o < size; o += sizeof got) {
        size_t n = size - o < sizeof got ? (size_t)(size - o) : sizeof got, g;
        memset(&none, 0, sizeof none);
        if (fwr_read_cursor(&v, &none, "/c.bin", o, got, n, &g) || g != n) { ++bad; break; }
        stream_fill(want, 5, o, n); if (memcmp(got, want, n)) ++bad;
    }
    uint64_t legacy_steps = v.wear.read_chain_steps - s0, legacy_resolves = v.wear.read_resolves - r0;
    /* 2: one cursor. */
    r0 = v.wear.read_resolves; s0 = v.wear.read_chain_steps;
    struct fwr_cursor cur = {0};
    for (uint64_t o = 0; o < size; o += sizeof got) {
        size_t n = size - o < sizeof got ? (size_t)(size - o) : sizeof got, g;
        if (fwr_read_cursor(&v, &cur, "/c.bin", o, got, n, &g) || g != n) { ++bad; break; }
        stream_fill(want, 5, o, n); if (memcmp(got, want, n)) ++bad;
    }
    uint64_t cursor_steps = v.wear.read_chain_steps - s0, cursor_resolves = v.wear.read_resolves - r0;
    /* 3: a read behind the cursor, and past the end. */
    size_t g;
    if (fwr_read_cursor(&v, &cur, "/c.bin", 3, got, 100, &g) || g != 100) ++bad;
    stream_fill(want, 5, 3, 100); if (memcmp(got, want, 100)) ++bad;
    if (fwr_read_cursor(&v, &cur, "/c.bin", size, got, 10, &g) || g != 0) ++bad;
    /* 4: the file changes under the cursor: append and sync (new size), then replace with other bytes. */
    CHECK(fwr_open(&v, &f, "/c.bin", FWR_O_APPEND, 0, fbuf1, sizeof fbuf1));
    CHECK(write_all(&f, 5, size, 3000, 1000));
    CHECK(fwr_close(&f));
    if (fwr_read_cursor(&v, &cur, "/c.bin", size, got, 1000, &g) || g != 1000) ++bad;
    stream_fill(want, 5, size, 1000); if (memcmp(got, want, 1000)) ++bad;
    CHECK(fwr_open(&v, &f, "/c.bin", FWR_O_REPLACE, 0, fbuf1, sizeof fbuf1));
    CHECK(write_all(&f, 9, 0, 5000, 1000));
    CHECK(fwr_close(&f));
    if (fwr_read_cursor(&v, &cur, "/c.bin", 4000, got, 1000, &g) || g != 1000) ++bad;
    stream_fill(want, 9, 4000, 1000); if (memcmp(got, want, 1000)) ++bad;
    if (fwr_read_cursor(&v, &cur, "/c.bin", 5000, got, 10, &g) || g != 0) ++bad;
    CHECK(fwr_unlink(&v, "/c.bin"));
    if (fwr_read_cursor(&v, &cur, "/c.bin", 0, got, 10, &g) != FWR_NOT_FOUND) ++bad;
    CHECK(fwr_unmount(&v));
    printf("cursor size=%llu pieces=%llu legacy_resolves=%llu legacy_steps=%llu cursor_resolves=%llu cursor_steps=%llu bad=%u\n",
           (unsigned long long)size, (unsigned long long)((size + sizeof got - 1) / sizeof got), (unsigned long long)legacy_resolves,
           (unsigned long long)legacy_steps, (unsigned long long)cursor_resolves, (unsigned long long)cursor_steps, bad);
    return bad != 0;
}

int main(int argc, char **argv)
{
    if (argc >= 8 && !strcmp(argv[1], "session"))
        return cmd_session(argv[2], strtoull(argv[3], 0, 0), strtoull(argv[4], 0, 0), argv[5], (uint32_t)strtoul(argv[6], 0, 0),
                           strtoull(argv[7], 0, 0));
    if (argc >= 6 && !strcmp(argv[1], "recover"))
        return cmd_recover(argv[2], strtoull(argv[3], 0, 0), strtoull(argv[4], 0, 0), (uint32_t)strtoul(argv[5], 0, 0),
                           argc > 6 ? argv[6] : NULL, argc > 7 ? strtoull(argv[7], 0, 0) : UINT64_MAX);
    if (argc >= 6 && !strcmp(argv[1], "serve"))
        return cmd_serve(argv[2], strtoull(argv[3], 0, 0), strtoull(argv[4], 0, 0), (uint32_t)strtoul(argv[5], 0, 0));
    if (argc >= 6 && !strcmp(argv[1], "accounting"))
        return cmd_accounting(argv[2], strtoull(argv[3], 0, 0), strtoull(argv[4], 0, 0), (uint32_t)strtoul(argv[5], 0, 0));
    if (argc >= 7 && !strcmp(argv[1], "cursor"))
        return cmd_cursor(argv[2], strtoull(argv[3], 0, 0), strtoull(argv[4], 0, 0), (uint32_t)strtoul(argv[5], 0, 0), strtoull(argv[6], 0, 0));
    if (argc >= 6 && !strcmp(argv[1], "posix"))
        return cmd_posix(argv[2], strtoull(argv[3], 0, 0), strtoull(argv[4], 0, 0), (uint32_t)strtoul(argv[5], 0, 0));
    if (argc >= 7 && !strcmp(argv[1], "plan"))
        return cmd_plan(argv[2], strtoull(argv[3], 0, 0), strtoull(argv[4], 0, 0), (uint32_t)strtoul(argv[5], 0, 0), strtoull(argv[6], 0, 0));
    if (argc >= 6 && !strcmp(argv[1], "guard"))
        return cmd_guard(argv[2], strtoull(argv[3], 0, 0), strtoull(argv[4], 0, 0), (uint32_t)strtoul(argv[5], 0, 0));
    fputs("usage: see the file header\n", stderr);
    return 64;
}
