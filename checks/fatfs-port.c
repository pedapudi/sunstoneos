/* Host model for the read-only FatFs port. Each case runs in a fresh process
 * because FatFs drive 0 binds once. Images and expected files come from
 * checks/fatfs-port.py, which builds them with mkfs and its own writers. */
#include "fatfs_port.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *image;
static size_t image_bytes;
static uint64_t image_sectors;

static void *read_file(const char *path, size_t *bytes)
{
    FILE *f = fopen(path, "rb"); assert(f);
    assert(!fseek(f, 0, SEEK_END)); long n = ftell(f); assert(n >= 0); rewind(f);
    uint8_t *p = malloc((size_t)n + 1); assert(p);
    assert(fread(p, 1, (size_t)n, f) == (size_t)n); fclose(f);
    *bytes = (size_t)n; return p;
}

/* RAM device: records every request and can refuse or fail one of them. */
struct ram {
    uint64_t sectors, calls, max_end;
    uint32_t max_blocks;
    uint64_t refuse_at, fail_at; /* 1-based request numbers; zero disables. */
    uint8_t *failed_buffer;
};
static int ram_read(void *ctx, uint64_t first, uint32_t blocks, uint8_t *buffer, uint64_t deadline)
{
    struct ram *r = ctx; ++r->calls;
    assert(deadline == 1000);
    assert(blocks >= 1 && blocks <= FATFS_PORT_MAX_BLOCKS);
    assert(first < r->sectors && blocks <= r->sectors - first);
    if (blocks > r->max_blocks) r->max_blocks = blocks;
    if (first + blocks > r->max_end) r->max_end = first + blocks;
    if (r->calls == r->refuse_at) return 0;
    if (r->calls == r->fail_at) {
        /* Partial bytes from a failed transfer must never reach FatFs. */
        memset(buffer, 0xe7, (size_t)blocks * 512u); r->failed_buffer = buffer; return -1;
    }
    memcpy(buffer, image + first * 512u, (size_t)blocks * 512u);
    return 1;
}

static struct fatfs_port port;
static struct ram ram;
static uint8_t out[1u << 20];

static void bind_ram(uint64_t sectors)
{
    ram = (struct ram){.sectors = sectors};
    struct fatfs_port_device d = {&ram, ram_read, sectors};
    assert(fatfs_port_bind(&port, &d));
    assert(!fatfs_port_bind(&port, &d));
}
static void load_ok(const char *path, const char *expected_path)
{
    size_t expected_bytes, bytes = 99;
    uint8_t *expected = read_file(expected_path, &expected_bytes);
    memset(out, 0x5a, sizeof(out));
    FRESULT r = fatfs_port_load(&port, path, out, sizeof(out), &bytes, 1000);
    if (r != FR_OK) fprintf(stderr, "%s: %d\n", path, (int)r);
    assert(r == FR_OK && bytes == expected_bytes && !memcmp(out, expected, bytes));
    assert(bytes == sizeof(out) || out[bytes] == 0x5a);
    free(expected);
}
static int has_fail_run(const uint8_t *p, size_t n)
{
    for (size_t i = 0, run = 0; i < n; ++i) if ((run = p[i] == 0xe7 ? run + 1 : 0) == 512) return 1;
    return 0;
}

/* Nominal: every listed file loads with exact size and contents. */
static void ram_nominal(int argc, char **argv)
{
    bind_ram(image_sectors);
    assert(fatfs_port_mount(&port, 1000) == FR_OK);
    for (int i = 0; i + 1 < argc; i += 2) load_ok(argv[i], argv[i + 1]);
    assert(ram.max_end <= image_sectors);
    printf("ram-nominal files=%d requests=%llu max-blocks=%u\n", argc / 2,
           (unsigned long long)ram.calls, ram.max_blocks);
}
/* Refusals that leave the port usable. */
static void ram_refusals(int argc, char **argv)
{
    assert(argc >= 2);
    size_t expected_bytes, bytes = 0;
    free(read_file(argv[1], &expected_bytes));
    assert(expected_bytes > 1);
    struct fatfs_port_device bad = {&ram, 0, image_sectors};
    assert(!fatfs_port_bind(&port, &bad));
    bad.read = ram_read; bad.sectors = 0;
    assert(!fatfs_port_bind(&port, &bad));
    assert(fatfs_port_load(&port, argv[0], out, sizeof(out), &bytes, 1000) == FR_INVALID_PARAMETER);
    bind_ram(image_sectors);
    /* Buffers overlapping the port are refused before any device request. */
    assert(fatfs_port_load(&port, argv[0], &port, sizeof(port), &bytes, 1000) == FR_INVALID_PARAMETER);
    assert(fatfs_port_load(&port, argv[0], NULL, 1, &bytes, 1000) == FR_INVALID_PARAMETER);
    assert(fatfs_port_load(&port, argv[0], out, sizeof(out), NULL, 1000) == FR_INVALID_PARAMETER);
    assert(!ram.calls);
    assert(fatfs_port_mount(&port, 1000) == FR_OK);
    /* Too small: size reported, no data copied. */
    memset(out, 0x5a, sizeof(out));
    uint64_t before = ram.calls;
    assert(fatfs_port_load(&port, argv[0], out, expected_bytes - 1, &bytes, 1000) == FR_INVALID_PARAMETER);
    assert(bytes == expected_bytes && out[0] == 0x5a && out[expected_bytes - 2] == 0x5a);
    assert(ram.calls - before <= 8); /* Directory lookups only. */
    assert(fatfs_port_load(&port, "missing.bin", out, sizeof(out), &bytes, 1000) == FR_NO_FILE);
    assert(fatfs_port_load(&port, "models", out, sizeof(out), &bytes, 1000) == FR_NO_FILE);
    /* A refused request fails this load only. */
    ram.refuse_at = ram.calls + 2;
    FRESULT r = fatfs_port_load(&port, argv[0], out, sizeof(out), &bytes, 1000);
    assert(r == FR_DISK_ERR && port.state == FATFS_PORT_READY && port.refusals == 1);
    ram.refuse_at = 0;
    load_ok(argv[0], argv[1]);
    printf("ram-refusals requests=%llu\n", (unsigned long long)ram.calls);
}
/* A failed request that may have started ends the port and keeps its buffer. */
static void ram_retained(int argc, char **argv)
{
    assert(argc >= 2);
    bind_ram(image_sectors);
    assert(fatfs_port_mount(&port, 1000) == FR_OK);
    size_t bytes;
    ram.fail_at = ram.calls + 3;
    memset(out, 0x5a, sizeof(out));
    assert(fatfs_port_load(&port, argv[0], out, sizeof(out), &bytes, 1000) == FR_DISK_ERR);
    assert(port.state == FATFS_PORT_RETAINED && ram.failed_buffer == port.bounce);
    assert(!has_fail_run(out, sizeof(out)));
    assert(!has_fail_run((const uint8_t *)&port.volume, sizeof(port.volume)));
    uint64_t calls = ram.calls;
    uint8_t bounce[sizeof(port.bounce)];
    memcpy(bounce, port.bounce, sizeof(bounce));
    assert(fatfs_port_load(&port, argv[0], out, sizeof(out), &bytes, 1000) == FR_NOT_READY);
    assert(fatfs_port_mount(&port, 1000) == FR_NOT_READY);
    assert(f_mount(&port.volume, "", 1) == FR_NOT_READY);
    assert(ram.calls == calls && !memcmp(bounce, port.bounce, sizeof(bounce)));
    printf("ram-retained requests=%llu\n", (unsigned long long)calls);
}
/* A device shorter than the volume: requests past its end never reach it. */
static void ram_short(int argc, char **argv)
{
    assert(argc == 2);
    uint64_t sectors = strtoull(argv[1], 0, 0);
    assert(sectors && sectors < image_sectors);
    bind_ram(sectors);
    FRESULT r = fatfs_port_mount(&port, 1000);
    if (r == FR_OK) {
        size_t bytes;
        r = fatfs_port_load(&port, argv[0], out, sizeof(out), &bytes, 1000);
    }
    if (r != FR_DISK_ERR) fprintf(stderr, "result %d\n", (int)r);
    assert(r == FR_DISK_ERR && port.state == FATFS_PORT_READY && ram.max_end <= sectors);
    printf("ram-short sectors=%llu requests=%llu\n", (unsigned long long)sectors,
           (unsigned long long)ram.calls);
}
/* Corrupt or unsupported metadata: the expected FatFs result, bounded reads. */
static void ram_expect(int argc, char **argv)
{
    assert(argc == 2);
    int expected = atoi(argv[1]);
    bind_ram(image_sectors);
    FRESULT r = fatfs_port_mount(&port, 1000);
    if (r == FR_OK) {
        size_t bytes;
        r = fatfs_port_load(&port, argv[0], out, sizeof(out), &bytes, 1000);
    }
    if ((int)r != expected) fprintf(stderr, "result %d expected %d\n", (int)r, expected);
    assert((int)r == expected && port.state == FATFS_PORT_READY && ram.calls < 2048);
    printf("ram-expect result=%d requests=%llu\n", expected, (unsigned long long)ram.calls);
}

/* SD card model for the polled core, serving the image through CMD17/CMD18.
 * Register behavior follows checks/sunxi-mmc.c for an SDHC card. */
struct model {
    uint32_t reg[0x304 / 4], csd[4], cid[4];
    uint64_t now, calls, mmio_after_fault, read_commands, idle;
    unsigned state, app, ocr_calls, selected, data_command, fifo_total, fifo_pos, advertised;
    uint64_t data_offset, crc_at_read, remove_at_read;
};
static struct model m;
static struct sunxi_mmc core;
static void field(uint32_t *r, unsigned first, unsigned count, uint32_t v)
{
    for (unsigned b = 0; b < count; ++b) {
        unsigned p = first + b, word = 3 - p / 32, bit = p % 32;
        r[word] = (r[word] & ~(1u << bit)) | (((v >> b) & 1u) << bit);
    }
}
static uint32_t status(void)
{
    uint32_t v = m.fifo_pos == m.fifo_total ? 4u : 0u;
    unsigned level = (m.fifo_total - m.fifo_pos) / 4;
    if (level > 256) level = 256;
    m.advertised = level;
    return v | level << 17;
}
static int read32(void *ctx, uint32_t off, uint32_t *v)
{
    (void)ctx; ++m.calls; assert(off <= 0x300 && !(off & 3));
    if (core.state == SUNXI_MMC_FAULT) ++m.mmio_after_fault;
    if (off == 0x38 && m.data_command && m.fifo_pos == m.fifo_total) {
        m.reg[off / 4] |= 8;
        if (m.data_command & (1u << 12)) m.reg[off / 4] |= 1u << 14;
    }
    if (off == 0x38 && m.crc_at_read && m.read_commands == m.crc_at_read) m.reg[off / 4] |= 1u << 7;
    if (off == 0x3c) { *v = status(); return 1; }
    if (off == 0x200) {
        assert(m.fifo_pos < m.fifo_total && m.advertised); --m.advertised;
        *v = 0;
        for (unsigned b = 0; b < 4; ++b, ++m.fifo_pos) {
            uint64_t at = m.data_offset + m.fifo_pos;
            assert(at < image_bytes);
            *v |= (uint32_t)image[at] << (8 * b);
        }
        return 1;
    }
    *v = m.reg[off / 4]; return 1;
}
static void command(uint32_t cmd)
{
    assert(!(cmd & (1u << 10))); /* No write direction ever. */
    unsigned n = cmd & 63, arg = m.reg[0x1c / 4];
    if (cmd & (1u << 21)) { m.reg[0x18 / 4] = 0; return; }
    m.reg[0x18 / 4] = cmd & ~(1u << 31); m.reg[0x38 / 4] = 4;
    m.reg[0x20 / 4] = (m.state << 9) | 0x100;
    m.data_command = 0; m.fifo_total = m.fifo_pos = 0;
    switch (n) {
    case 0: m.state = 0; break;
    case 8: assert(!m.state && arg == 0x1aa); m.reg[0x20 / 4] = 0x1aa; break;
    case 55: m.app = 1; m.reg[0x20 / 4] |= 1u << 5; break;
    case 41:
        assert(m.app); m.app = 0;
        m.reg[0x20 / 4] = 0x40300000u | (++m.ocr_calls == 2 ? 1u << 31 : 0);
        if (m.ocr_calls == 2) m.state = 1;
        break;
    case 2: m.state = 2; for (unsigned i = 0; i < 4; ++i) m.reg[(0x2c - i * 4) / 4] = m.cid[i]; break;
    case 3: m.state = 3; m.reg[0x20 / 4] = 0x12340700; break;
    case 9: for (unsigned i = 0; i < 4; ++i) m.reg[(0x2c - i * 4) / 4] = m.csd[i]; break;
    case 7: m.state = 4; m.selected = 1; break;
    case 13: assert(m.selected); m.reg[0x20 / 4] = 0x900; break;
    case 17: case 18:
        assert(m.selected && m.reg[0x10 / 4] == 512 && (cmd & (1u << 9)));
        ++m.read_commands; m.data_command = cmd; m.fifo_total = m.reg[0x14 / 4];
        m.data_offset = (uint64_t)arg * 512u;
        assert(m.fifo_total >= 512 && m.fifo_total <= 4096 && m.data_offset + m.fifo_total <= image_bytes);
        m.reg[0x20 / 4] = 0x900; m.reg[0x24 / 4] = 0x900;
        break;
    default: assert(!"unsupported card command");
    }
}
static int write32(void *ctx, uint32_t off, uint32_t value)
{
    (void)ctx; ++m.calls; assert(off <= 0x300 && !(off & 3));
    if (core.state == SUNXI_MMC_FAULT) ++m.mmio_after_fault;
    if (off == 0x38) m.reg[off / 4] &= ~value;
    else if (off == 0) { m.reg[0] = value & ~7u; if (value & 7) m.fifo_total = m.fifo_pos = 0; }
    else m.reg[off / 4] = value;
    if (off == 0x18) command(value);
    return 1;
}
static int barrier(void *ctx) { (void)ctx; ++m.calls; return 1; }
static int now_us(void *ctx, uint64_t *v) { (void)ctx; ++m.calls; *v = (m.now += 50); return 1; }
static int admit(void *ctx, uintptr_t token, enum sunxi_mmc_kind kind, uint32_t hz)
{ (void)ctx; return token == 0xa733 && kind == SUNXI_MMC_SD && hz == 800000; }
static int retained(void *ctx, uintptr_t token)
{
    (void)ctx; assert(token == 0xa733);
    return !(m.remove_at_read && m.read_commands == m.remove_at_read);
}
static int read_fifo(void *ctx, uint32_t *w, unsigned n)
{ for (unsigned i = 0; i < n; ++i) if (read32(ctx, 0x200, &w[i]) != 1) return 0; return 1; }
static int write_fifo(void *ctx, const uint32_t *w, unsigned n)
{ (void)ctx; (void)w; (void)n; assert(!"media write"); return 0; }
static void idle(void *ctx) { assert(ctx == &m); ++m.idle; }

static struct fatfs_port_mmc binding;
static void identify(void)
{
    assert(image_sectors % 1024 == 0);
    m.reg[0x300 / 4] = 0x03050530; m.reg[0x5c / 4] = 0x81710331; m.reg[0x10c / 4] = 0x80001234;
    m.reg[0x54 / 4] = 0x12345676; m.reg[0x58 / 4] = 0xffff; m.reg[0x104 / 4] = 7;
    m.reg[0x140 / 4] = 0xa5a40000;
    field(m.csd, 126, 2, 1); field(m.csd, 80, 4, 9); field(m.csd, 84, 12, 5);
    field(m.csd, 48, 22, (uint32_t)(image_sectors / 1024 - 1));
    for (unsigned i = 0; i < 4; ++i) m.cid[i] = 0x12345678u + i;
    struct sunxi_mmc_config c = {{read32, write32, barrier, now_us, admit, retained, NULL,
        read_fifo, write_fifo, NULL, NULL}, &m, 0xa733, SUNXI_MMC_SD, 800000, 0x00300000, SUNXI_MMC_SD_LEGACY, NULL};
    struct fatfs_port_device d;
    binding = (struct fatfs_port_mmc){&core, idle, &m, {0}};
    assert(sunxi_mmc_init(&core, &c) && sunxi_mmc_open(&core, 1000000));
    assert(!fatfs_port_mmc_device(&binding, &d));
    for (unsigned i = 0; i < 20000 && core.state == SUNXI_MMC_IDENTIFY; ++i) sunxi_mmc_poll(&core);
    assert(core.state == SUNXI_MMC_READY && core.card.sectors == image_sectors);
    assert(fatfs_port_mmc_device(&binding, &d) && d.sectors == image_sectors);
    assert(fatfs_port_bind(&port, &d));
}
/* End to end: FatFs, the port, the binding and the actual polled core. */
static void mmc_nominal(int argc, char **argv)
{
    identify();
    assert(fatfs_port_mount(&port, m.now + 10000000) == FR_OK);
    for (int i = 0; i + 1 < argc; i += 2) {
        size_t expected_bytes, bytes;
        uint8_t *expected = read_file(argv[i + 1], &expected_bytes);
        assert(fatfs_port_load(&port, argv[i], out, sizeof(out), &bytes, m.now + 10000000) == FR_OK);
        assert(bytes == expected_bytes && !memcmp(out, expected, bytes));
        free(expected);
    }
    assert(binding.result.error == SUNXI_MMC_OK && m.idle && core.state == SUNXI_MMC_READY);
    printf("mmc-nominal files=%d card-reads=%llu idle=%llu\n", argc / 2,
           (unsigned long long)m.read_commands, (unsigned long long)m.idle);
}
/* Controller CRC error, card removal or deadline expiry during a file read:
 * the core faults, keeps the bounce buffer borrowed and the port stops. */
static void mmc_fault(int argc, char **argv)
{
    assert(argc == 2);
    identify();
    assert(fatfs_port_mount(&port, m.now + 10000000) == FR_OK);
    uint64_t deadline = m.now + 10000000;
    enum sunxi_mmc_error expected;
    if (!strcmp(argv[1], "crc")) { m.crc_at_read = m.read_commands + 2; expected = SUNXI_MMC_CONTROLLER; }
    else if (!strcmp(argv[1], "removal")) { m.remove_at_read = m.read_commands + 2; expected = SUNXI_MMC_ADMISSION; }
    else { assert(!strcmp(argv[1], "deadline")); deadline = m.now + 150; expected = SUNXI_MMC_TIMEOUT; }
    size_t bytes;
    FRESULT r = fatfs_port_load(&port, argv[0], out, sizeof(out), &bytes, deadline);
    assert(r == FR_DISK_ERR && port.state == FATFS_PORT_RETAINED);
    assert(core.state == SUNXI_MMC_FAULT && core.result.error == expected);
    assert(core.borrowed == port.bounce && binding.result.error == expected);
    uint64_t calls = m.calls;
    assert(fatfs_port_load(&port, argv[0], out, sizeof(out), &bytes, UINT64_MAX) == FR_NOT_READY);
    assert(m.calls == calls && !m.mmio_after_fault && core.borrowed == port.bounce);
    printf("mmc-fault %s card-reads=%llu\n", argv[1], (unsigned long long)m.read_commands);
}

int main(int argc, char **argv)
{
    assert(argc >= 3);
    image = read_file(argv[2], &image_bytes);
    assert(image_bytes % 512 == 0); image_sectors = image_bytes / 512;
    const char *mode = argv[1];
    argc -= 3; argv += 3;
    if (!strcmp(mode, "ram-nominal")) ram_nominal(argc, argv);
    else if (!strcmp(mode, "ram-refusals")) ram_refusals(argc, argv);
    else if (!strcmp(mode, "ram-retained")) ram_retained(argc, argv);
    else if (!strcmp(mode, "ram-short")) ram_short(argc, argv);
    else if (!strcmp(mode, "ram-expect")) ram_expect(argc, argv);
    else if (!strcmp(mode, "mmc-nominal")) mmc_nominal(argc, argv);
    else if (!strcmp(mode, "mmc-fault")) mmc_fault(argc, argv);
    else assert(!"unknown mode");
    free(image);
    return 0;
}
