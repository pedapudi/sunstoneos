/* Host model for the storage-write test's time guard: apps/storage-write-test/main.c is compiled unchanged except for a
 * scaled-down file and guard (FILE_BYTES, GUARD_US), against a file-backed card image whose writes are slow (a fixed
 * sleep per write call) and a stub of the OS kit. The model refuses writes outside the FAT copies and data area, and
 * ss_finish stores the 512-byte result in the file named by RESULT and exits. Usage:
 *   storage-write-guard IMAGE BASE SECTORS MODE RESULT DELAY_US */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "../apps/storage-write-test/main.c"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static int image_fd;
static uint64_t dev_sectors, part_base, part_sectors, delay_us;
static const char *result_path;
static uint64_t region_fat1, region_fat2, region_data, region_end, region_fsinfo;

uint64_t ss_counter_ticks(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return ((uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec) / 1000u * 24u;      /* 24 MHz */
}
int ss_clock_ready(void) { return 1; }
void ss_progress(void) {}
void ss_activity_begin(void) {}
void ss_activity_end(int success) { (void)success; }
void sha256_block_ce(const uint8_t b[64], uint8_t d[32]) { (void)b; (void)d; abort(); }          /* the CE path is verified in QEMU */
int ss_cpu_observations(unsigned core, struct ss_cpu_observation *o) { (void)core; memset(o, 0, sizeof *o); return 1; }
int ss_storage_enable(const void *b, size_t n) { (void)b; (void)n; return 1; }
int ss_storage_region(void *b, size_t n) { (void)b; (void)n; return 1; }
int ss_storage_close(void) { return 1; }
int ss_storage_diagnostic(uint64_t out[4]) { memset(out, 0, 32); return 1; }
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
    return pread(image_fd, buf, (size_t)n * 512, (off_t)s * 512) == (ssize_t)n * 512;
}
static int allowed(uint64_t s)
{
    uint64_t fsz = region_fat2 - region_fat1;
    return (s >= region_fat1 && s < region_fat2 + fsz) || s == region_fsinfo || (s >= region_data && s < region_end);
}
static int dev_write(void *c, uint64_t s, uint32_t n, const uint8_t *buf)
{
    (void)c;
    if (s + n > dev_sectors) { fprintf(stderr, "write outside device\n"); exit(3); }
    for (uint32_t i = 0; i < n; ++i)
        if (!allowed(s + i)) { fprintf(stderr, "model refused write to sector %llu\n", (unsigned long long)(s + i)); exit(4); }
    struct timespec d = {(time_t)(delay_us / 1000000u), (long)(delay_us % 1000000u) * 1000};
    nanosleep(&d, NULL);                                     /* the slow card */
    return pwrite(image_fd, buf, (size_t)n * 512, (off_t)s * 512) == (ssize_t)n * 512;
}
int ss_storage_device(struct fwr_device *d)
{
    *d = (struct fwr_device){.context = NULL, .read = dev_read, .write = dev_write, .yield = NULL, .sectors = dev_sectors,
                             .au_sectors = 256, .max_write_sectors = 256};
    return 1;
}

static uint8_t bounds_blob[64], mode_blob[4096] __attribute__((aligned(64)));
static uint8_t flush_buffer[1 << 20] __attribute__((aligned(64)));

int main(int argc, char **argv)
{
    if (argc != 7) return 2;
    image_fd = open(argv[1], O_RDWR);
    if (image_fd < 0) return 2;
    part_base = strtoull(argv[2], 0, 0); part_sectors = strtoull(argv[3], 0, 0);
    dev_sectors = (uint64_t)lseek(image_fd, 0, SEEK_END) / 512;
    mode_blob[0] = (uint8_t)atoi(argv[4]);
    result_path = argv[5]; delay_us = strtoull(argv[6], 0, 0);
    uint8_t b[512];
    if (pread(image_fd, b, 512, (off_t)part_base * 512) != 512) return 2;
    uint32_t reserved = b[14] | b[15] << 8, fats = b[16], fsz = b[36] | b[37] << 8 | b[38] << 16 | (uint32_t)b[39] << 24;
    uint32_t info = b[48] | b[49] << 8;
    region_fat1 = part_base + reserved; region_fat2 = region_fat1 + fsz;
    region_fsinfo = info ? part_base + info : UINT64_MAX;
    region_data = part_base + reserved + (uint64_t)fats * fsz; region_end = part_base + part_sectors;
    struct ss_grant g = {0};
    g.regions[STORAGE_WRITE_REGION_BOUNDS] = (struct ss_span){bounds_blob, sizeof bounds_blob};
    g.regions[STORAGE_WRITE_REGION_MODE] = (struct ss_span){mode_blob, sizeof mode_blob};
    g.regions[STORAGE_WRITE_REGION_FLUSH] = (struct ss_span){flush_buffer, sizeof flush_buffer};
    storage_write_main(&g);
    return 6;
}
