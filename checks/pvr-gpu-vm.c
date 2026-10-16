#include "pvr_gpu_vm.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct pvr_gpu_mapping fixture[] = {
    {0,             UINT64_C(0x140000000), 0x2000,   12, 1},
    {0x3000,        UINT64_C(0x140003000), 0x1000,   12, 2},
    {0x1ff000,      UINT64_C(0x150000000), 0x2000,   12, 4},
    {0x400000,      UINT64_C(0x200000000), 0x8000,   14, 8},
    {0x800000,      UINT64_C(0x220000000), 0x20000,  16, 15},
    {0x1000000,     UINT64_C(0x230000000), 0x80000,  18, 0},
    {0x20000000,    UINT64_C(0x240000000), 0x200000, 20, 2},
    {0x3fe00000,    UINT64_C(0x260000000), 0x400000, 21, 4},
    {UINT64_C(0xda00000000), UINT64_C(0x300000000), 0x1000, 12, 1},
    {UINT64_C(0xe000000000), UINT64_C(0x310000000), 0x1000, 12, 1},
    {UINT64_C(0xfffffff000), UINT64_C(0xffffff000), 0x1000, 12, 8}
};
#define COUNT (sizeof(fixture) / sizeof(fixture[0]))
static unsigned char *storage, context_guard[160];
static struct pvr_gpu_vm_tables tables;
static unsigned refusals;
static uint64_t read_le(const unsigned char *p, unsigned n)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < n; ++i) value |= (uint64_t)p[i] << (i * 8);
    return value;
}
static void pattern(const void *p, size_t n)
{
    const unsigned char *bytes = p;
    for (size_t i = 0; i < n; ++i) assert(bytes[i] == 0xa5);
}
static void fill(void)
{
    memset(storage, 0xa5, tables.bytes + 130); memset(context_guard, 0xa5, sizeof(context_guard));
}
static void refuse(const struct pvr_gpu_mapping *maps, size_t count,
                   const struct pvr_gpu_vm_tables *t, void *context, size_t bytes)
{
    fill(); assert(!pvr_gpu_vm_build(maps, count, t, context, bytes));
    pattern(storage, tables.bytes + 130); pattern(context_guard, sizeof(context_guard)); ++refusals;
}
static const unsigned char *table_at(uint64_t physical)
{
    for (unsigned i = 0; i < tables.page_count; ++i)
        if (tables.physical_pages[i] == physical) return (unsigned char *)tables.cpu + i * 4096;
    assert(0); return NULL;
}
static int walk(uint64_t address, uint64_t *physical, uint64_t *permission)
{
    uint32_t pc = (uint32_t)read_le((unsigned char *)tables.cpu + (address >> 30) * 4, 4);
    if (!(pc & 1)) return 0;
    const unsigned char *pd = table_at((uint64_t)(pc & 0xfffffff0) << 8);
    uint64_t de = read_le(pd + ((address >> 21) & 511) * 8, 8);
    if (!(de & 1)) return 0;
    unsigned code = (de >> 1) & 7;
    static const unsigned shifts[] = {12, 14, 16, 18, 20, 21};
    assert(code < 6);
    unsigned shift = shifts[code];
    const unsigned char *pt = table_at(de & UINT64_C(0xfffffff000));
    uint64_t leaf = read_le(pt + ((address & 0x1fffff) >> shift) * 8, 8);
    if (!(leaf & 1)) return 0;
    uint64_t mask = (UINT64_C(1) << shift) - 1;
    *physical = (leaf & UINT64_C(0xfffffff000) & ~mask) | (address & mask);
    *permission = leaf & UINT64_C(0x400000000000000e);
    return 1;
}
static FILE *open_file(const char *dir, const char *name)
{
    char path[4096]; int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
    assert(n > 0 && (size_t)n < sizeof(path)); FILE *f = fopen(path, "wb"); assert(f); return f;
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    struct pvr_gpu_vm_requirements r;
    assert(pvr_gpu_vm_require(fixture, COUNT, &r));
    assert(r.table_pages == 17 && r.directory_pages == 5 && r.leaf_tables == 11 && r.mapped_pages == 18);
    uint64_t pages[17];
    for (unsigned i = 0; i < 17; ++i) pages[i] = UINT64_C(0x800001000) + i * 0x3000;
    storage = malloc((size_t)r.table_bytes + 130); assert(storage);
    tables = (struct pvr_gpu_vm_tables){storage + 65, (size_t)r.table_bytes, pages, 17};
    unsigned char *context = context_guard + 65;
    fill(); assert(pvr_gpu_vm_build(fixture, COUNT, &tables, context, 32));
    pattern(storage, 65); pattern(storage + 65 + tables.bytes, 65);
    pattern(context_guard, 65); pattern(context + 32, sizeof(context_guard) - 97);
    assert(read_le(context, 8) == pages[0] && read_le(context + 8, 4) == UINT32_MAX);
    for (unsigned i = 12; i < 32; ++i) assert(context[i] == 0);
    for (unsigned i = 0; i < COUNT; ++i) {
        uint64_t step = UINT64_C(1) << fixture[i].page_shift;
        for (uint64_t offset = 0; offset < fixture[i].bytes; offset += step) {
            const uint64_t offsets[] = {offset, offset + step / 2, offset + step - 1};
            for (unsigned j = 0; j < 3; ++j) {
                uint64_t physical, flags;
                assert(walk(fixture[i].address + offsets[j], &physical, &flags));
                assert(physical == fixture[i].physical + offsets[j]);
                assert(((flags & 2) != 0) == ((fixture[i].flags & 1) != 0));
                assert(((flags & 4) != 0) == ((fixture[i].flags & 2) != 0));
                assert(((flags & 8) != 0) == ((fixture[i].flags & 4) != 0));
                assert((flags >> 62) == ((fixture[i].flags >> 3) & 1));
            }
        }
    }
    uint64_t physical, permissions;
    const uint64_t holes[] = {0x2000, 0x4000, 0x201000, 0x408000, 0x600000, UINT64_C(0xc0000000)};
    for (unsigned i = 0; i < sizeof(holes) / sizeof(holes[0]); ++i) assert(!walk(holes[i], &physical, &permissions));
    FILE *f = open_file(argv[1], "tables.bin");
    assert(fwrite(tables.cpu, 1, tables.bytes, f) == tables.bytes && !fclose(f));
    f = open_file(argv[1], "context.bin"); assert(fwrite(context, 1, 32, f) == 32 && !fclose(f));
    f = open_file(argv[1], "fixture.json");
    fprintf(f, "{\"maps\":[");
    for (unsigned i = 0; i < COUNT; ++i) fprintf(f,
        "%s{\"address\":%llu,\"physical\":%llu,\"bytes\":%llu,\"page_shift\":%u,\"flags\":%u}",
        i ? "," : "", (unsigned long long)fixture[i].address, (unsigned long long)fixture[i].physical,
        (unsigned long long)fixture[i].bytes, fixture[i].page_shift, fixture[i].flags);
    fprintf(f, "],\"table_pages\":[");
    for (unsigned i = 0; i < 17; ++i) fprintf(f, "%s%llu", i ? "," : "", (unsigned long long)pages[i]);
    fprintf(f, "]}\n"); assert(!fclose(f));

    struct pvr_gpu_mapping maps[COUNT];
    const struct pvr_gpu_mapping invalid[] = {
        {0,0x1000,0,12,0}, {0,0,4096,12,0}, {0,0x1001,4096,12,0},
        {1,0x1000,4096,12,0}, {0,0x1000,4095,12,0}, {0,0x1000,4096,13,0},
        {0,0x1000,4096,32,0}, {0,0x1000,4096,12,16},
        {UINT64_C(1)<<40,0x1000,4096,12,0},
        {UINT64_C(0xfffffff000),0x1000,8192,12,0},
        {0,UINT64_C(1)<<36,4096,12,0}, {0,UINT64_C(0xffffff000),8192,12,0},
        {0,0x1000,UINT64_MAX,12,0}
    };
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        struct pvr_gpu_vm_requirements before; memset(&before, 0xa5, sizeof(before)); r = before;
        assert(!pvr_gpu_vm_require(&invalid[i], 1, &r) && !memcmp(&r, &before, sizeof(r)));
        refuse(&invalid[i], 1, &tables, context, 32);
    }
    memcpy(maps, fixture, sizeof(maps)); maps[1].address = 0;
    refuse(maps, COUNT, &tables, context, 32);
    memcpy(maps, fixture, sizeof(maps)); maps[1] = (struct pvr_gpu_mapping){0x4000,0x4000,0x4000,14,0};
    refuse(maps, COUNT, &tables, context, 32); /* Different page sizes in one PD entry. */
    memcpy(maps, fixture, sizeof(maps)); maps[5].physical = UINT64_C(0x800000000);
    assert(pvr_gpu_vm_require(maps, COUNT, &r));
    refuse(maps, COUNT, &tables, context, 32); /* Table backing lies inside a larger mapped span. */
    uint64_t saved = pages[16];
    const uint64_t bad_pages[] = {pages[15], pages[0]-4096, 0, 1, UINT64_C(1)<<36};
    for (unsigned i = 0; i < sizeof(bad_pages)/sizeof(bad_pages[0]); ++i) {
        pages[16] = bad_pages[i]; refuse(fixture, COUNT, &tables, context, 32);
    }
    pages[16] = saved;
    struct pvr_gpu_vm_tables bad = tables;
    --bad.page_count; refuse(fixture, COUNT, &bad, context, 32);
    bad = tables; --bad.bytes; refuse(fixture, COUNT, &bad, context, 32);
    bad = tables; bad.cpu = NULL; refuse(fixture, COUNT, &bad, context, 32);
    bad = tables; bad.physical_pages = NULL; refuse(fixture, COUNT, &bad, context, 32);
    bad = tables; bad.cpu = pages; refuse(fixture, COUNT, &bad, context, 32);
    bad = tables; bad.cpu = &bad; refuse(fixture, COUNT, &bad, context, 32);
    bad = tables; bad.cpu = maps; refuse(maps, COUNT, &bad, context, 32);
    refuse(fixture, COUNT, &tables, tables.cpu, 32);
    refuse(fixture, COUNT, &tables, context, 31);
    refuse(fixture, COUNT, &tables, NULL, 32);
    refuse(NULL, 1, &tables, context, 32);
    refuse(fixture, SIZE_MAX, &tables, context, 32);
    refuse(fixture, COUNT, NULL, context, 32);

    /* Empty contexts are valid, and only their exact output extent is cleared. */
    assert(pvr_gpu_vm_require(NULL, 0, &r) && r.table_pages == 1 && r.mapped_pages == 0);
    bad = tables; bad.bytes = 4096; bad.page_count = 1;
    fill(); assert(pvr_gpu_vm_build(NULL, 0, &bad, context, 32));
    for (unsigned i = 0; i < 4096; ++i) assert(!((unsigned char *)bad.cpu)[i]);
    pattern((unsigned char *)bad.cpu + 4096, tables.bytes - 4096);
    free(storage);
    printf("PASS GPU VM six page sizes, boundary translations, flags, empty contexts and %u atomic refusals\n", refusals);
}
