#include "pvr_mips.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const size_t sizes[] = {102400, 20480, PVR_MIPS_TABLE_BYTES};
static unsigned char *allocations[3];
static struct pvr_mips_boot boot;
static unsigned refusals;

static void fill(void)
{
    for (unsigned i = 0; i < 3; ++i) memset(allocations[i], 0xa5, sizes[i] + 130);
    memset(&boot, 0xa5, sizeof(boot));
}
static void pattern(const void *data, size_t bytes)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < bytes; ++i) assert(p[i] == 0xa5);
}
static void refuse(const void *image, size_t bytes, const struct pvr_mips_buffer *b,
                   const struct pvr_mips_object *m, size_t count, struct pvr_mips_boot *out)
{
    fill();
    assert(!pvr_mips_prepare(image, bytes, b, m, count, out));
    for (unsigned i = 0; i < 3; ++i) pattern(allocations[i], sizes[i] + 130);
    pattern(&boot, sizeof(boot));
    ++refusals;
}
static void save(const char *dir, const char *name, const void *data, size_t bytes)
{
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
    assert(n > 0 && (size_t)n < sizeof(path));
    FILE *f = fopen(path, "wb");
    assert(f && fwrite(data, 1, bytes, f) == bytes && !fclose(f));
}
static void put32(unsigned char *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(n >> (i * 8));
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    FILE *f = fopen(argv[1], "rb");
    assert(f && !fseek(f, 0, SEEK_END));
    long length = ftell(f);
    assert(length == 131072 && !fseek(f, 0, SEEK_SET));
    size_t bytes = (size_t)length;
    unsigned char *raw = malloc(bytes + 1), *copy = malloc(bytes);
    assert(raw && copy);
    unsigned char *image = raw + 1;
    assert(fread(image, 1, bytes, f) == bytes && !fclose(f));
    memcpy(copy, image, bytes);
    uint64_t code[25], data[5], table[4], extra[] = {
        UINT64_C(0x440000000), UINT64_C(0x440007000), UINT64_C(0xffffff000)};
    for (unsigned i = 0; i < 25; ++i) code[i] = UINT64_C(0x140000000) + i * 0x3000;
    for (unsigned i = 0; i < 5; ++i) data[i] = UINT64_C(0x240000000) + i * 0x5000;
    for (unsigned i = 0; i < 4; ++i) table[i] = UINT64_C(0x340000000) + i * 0x7000;
    struct pvr_mips_buffer b[3];
    for (unsigned i = 0; i < 3; ++i) {
        allocations[i] = malloc(sizes[i] + 130);
        assert(allocations[i]);
        b[i] = (struct pvr_mips_buffer){{allocations[i] + 65, sizes[i]},
            i == 0 ? code : i == 1 ? data : table, (uint32_t)(sizes[i] / 4096)};
    }
    struct pvr_mips_object m[2] = {
        {extra, 2, 0xc0100000, PVR_MIPS_WRITE_THROUGH},
        {extra + 2, 1, 0xc0fff000, PVR_MIPS_UNCACHED}};
    fill();
    assert(pvr_mips_prepare(image, bytes, b, m, 2, &boot));
    const char *names[] = {"code.bin", "data.bin", "table.bin"};
    for (unsigned i = 0; i < 3; ++i) {
        pattern(allocations[i], 65);
        pattern(allocations[i] + 65 + sizes[i], 65);
        save(argv[2], names[i], b[i].storage.cpu, sizes[i]);
    }
    _Static_assert(sizeof(boot) == 184, "boot result serialization");
    save(argv[2], "wrapper.bin", &boot, sizeof(boot));
    assert(!memcmp(copy, image, bytes));

    /* Removing optional objects clears their old table entries. */
    assert(pvr_mips_prepare(image, bytes, b, NULL, 0, &boot));
    const unsigned char *pt = b[2].storage.cpu;
    for (unsigned i = 0; i < 8; ++i) assert(!pt[0x100 * 4 + i]);
    for (unsigned i = 0; i < 4; ++i) assert(!pt[4095 * 4 + i]);

    /* Validate real image metadata and architectural layout constraints. */
    const uint32_t footer = 126976, layout = footer + 48, info = 122880;
    const struct { uint32_t offset, value; } changes[] = {
        {info + 32, 0},                 /* Missing address-zero workaround. */
        {info + 24, 28}, {info + 24, 30}, /* Missing / trailing parameter. */
        {info + 56, 0x80},              /* Unknown feature bit 71. */
        {info + 52, 0},                 /* Missing physical-address feature. */
        {layout + 24 + 8, 0x9fc03000},   /* Wrong exception alias. */
        {layout + 4 * 24 + 8, 0xbfc04000}, /* Wrong boot-data alias. */
        {layout + 5 * 24 + 8, 0xcf700000}, /* Wrong stack alias. */
        {24, 0xbfc00000},               /* Wrong boot instruction mode. */
        {footer + 16, 0}, {0, 0}
    };
    for (unsigned i = 0; i < sizeof(changes) / sizeof(changes[0]); ++i) {
        put32(copy + changes[i].offset, changes[i].value);
        refuse(copy, bytes, b, m, 2, &boot);
        memcpy(copy, image, bytes);
    }
    /* Find the width parameter through the known fixture value sequence. */
    const uint32_t width_offset = info + 64 + 16 * 8;
    assert(copy[width_offset] == 36);
    put32(copy + width_offset, 32);
    refuse(copy, bytes, b, m, 2, &boot);
    memcpy(copy, image, bytes);

    uint64_t saved = code[0];
    const uint64_t invalid[] = {0, 1, UINT64_C(1) << 36, 0x1fc00000,
        0x1fc01000, 0x1fc02000, 0x1fc03000, code[1], data[0], table[3], extra[2]};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        code[0] = invalid[i];
        refuse(image, bytes, b, m, 2, &boot);
    }
    code[0] = saved;
    uint64_t saved_table = table[3];
    table[3] = table[0];
    refuse(image, bytes, b, m, 2, &boot);
    table[3] = saved_table;

    struct pvr_mips_object changed[2];
    const uint32_t bad_addresses[] = {0, 0xbffff000, 0xc1000000, 0xfffff000,
        0xc0000001, 0xc0000000, 0xc0032000, 0xc0fff000};
    for (unsigned i = 0; i < sizeof(bad_addresses) / sizeof(bad_addresses[0]); ++i) {
        memcpy(changed, m, sizeof(m)); changed[0].address = bad_addresses[i];
        refuse(image, bytes, b, changed, 2, &boot);
    }
    memcpy(changed, m, sizeof(m)); changed[1].address = m[0].address + 4096;
    refuse(image, bytes, b, changed, 2, &boot);
    memcpy(changed, m, sizeof(m)); changed[0].cache_policy = 3;
    refuse(image, bytes, b, changed, 2, &boot);
    memcpy(changed, m, sizeof(m)); changed[0].page_count = 0;
    refuse(image, bytes, b, changed, 2, &boot);
    changed[0].page_count = UINT32_MAX;
    refuse(image, bytes, b, changed, 2, &boot);
    memcpy(changed, m, sizeof(m)); changed[0].pages = NULL;
    refuse(image, bytes, b, changed, 2, &boot);

    struct pvr_mips_buffer bad[3];
    for (unsigned i = 0; i < 3; ++i) {
        memcpy(bad, b, sizeof(b)); --bad[i].storage.bytes;
        refuse(image, bytes, bad, m, 2, &boot);
        memcpy(bad, b, sizeof(b)); --bad[i].page_count;
        refuse(image, bytes, bad, m, 2, &boot);
        memcpy(bad, b, sizeof(b)); bad[i].storage.cpu = NULL;
        refuse(image, bytes, bad, m, 2, &boot);
        memcpy(bad, b, sizeof(b)); bad[i].pages = NULL;
        refuse(image, bytes, bad, m, 2, &boot);
    }
    memcpy(bad, b, sizeof(b)); bad[1].storage.cpu = b[0].storage.cpu;
    refuse(image, bytes, bad, m, 2, &boot);
    memcpy(bad, b, sizeof(b)); bad[2].storage.cpu = copy;
    refuse(copy, bytes, bad, m, 2, &boot);
    assert(!memcmp(copy, image, bytes));
    memcpy(bad, b, sizeof(b)); bad[2].storage.cpu = bad;
    refuse(image, bytes, bad, m, 2, &boot);
    memcpy(bad, b, sizeof(b)); bad[2].storage.cpu = m;
    refuse(image, bytes, bad, m, 2, &boot);
    memcpy(bad, b, sizeof(b)); bad[0].pages = (uint64_t *)(allocations[0] + 64);
    refuse(image, bytes, bad, m, 2, &boot);
    refuse(image, bytes, b, m, 2, (struct pvr_mips_boot *)(allocations[1] + 64));
    refuse(image, bytes, b, m, 2, NULL);
    refuse(image, bytes, NULL, m, 2, &boot);
    refuse(image, bytes, b, NULL, 1, &boot);
    refuse(image, bytes, b, m, PVR_MIPS_MAX_OBJECTS + 1, &boot);
    refuse(NULL, bytes, b, m, 2, &boot);
    refuse(image, SIZE_MAX, b, m, 2, &boot);
    assert(!memcmp(copy, image, bytes));
    for (unsigned i = 0; i < 3; ++i) free(allocations[i]);
    free(copy); free(raw);
    printf("PASS GPU MIPS preparation, guards, table replacement and %u atomic refusals\n", refusals);
}
