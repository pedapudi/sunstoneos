#include "pvr_fw_state.h"
#include "pvr_ccb.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *memory[PVR_FW_RESOURCE_COUNT];
static struct pvr_mips_buffer buffers[PVR_FW_RESOURCE_COUNT];
static struct pvr_mips_object mappings[PVR_FW_RESOURCE_COUNT];
static struct pvr_fw_state_config config = {PVR_FW_A733_BVNC, 123456789, 1, 16};
static unsigned refusals;
static uint64_t read_le(const unsigned char *p, unsigned bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i) value |= (uint64_t)p[i] << (i * 8);
    return value;
}
static void pattern(const void *data, size_t bytes)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < bytes; ++i) assert(p[i] == 0xa5);
}
static void fill(void)
{
    for (unsigned i = 0; i < PVR_FW_RESOURCE_COUNT; ++i)
        memset(memory[i], 0xa5, buffers[i].storage.bytes + 130);
    memset(mappings, 0xa5, sizeof(mappings));
}
static void refuse(const struct pvr_mips_buffer *b, const struct pvr_fw_state_config *c,
                   struct pvr_mips_object *out)
{
    fill();
    assert(!pvr_fw_state_prepare(b, c, out));
    for (unsigned i = 0; i < PVR_FW_RESOURCE_COUNT; ++i)
        pattern(memory[i], buffers[i].storage.bytes + 130);
    pattern(mappings, sizeof(mappings));
    ++refusals;
}
static int read_memory(void *ctx, enum pvr_ccb_region region, uint32_t offset, void *out, size_t n)
{
    (void)ctx;
    assert((unsigned)region < 5 && offset <= pvr_ccb_region_bytes(region) &&
           n <= pvr_ccb_region_bytes(region) - offset);
    memcpy(out, (unsigned char *)buffers[region].storage.cpu + offset, n);
    return 1;
}
static int write_memory(void *ctx, enum pvr_ccb_region region, uint32_t offset, const void *in, size_t n)
{
    (void)ctx;
    assert((unsigned)region < 5 && offset <= pvr_ccb_region_bytes(region) &&
           n <= pvr_ccb_region_bytes(region) - offset);
    memcpy((unsigned char *)buffers[region].storage.cpu + offset, in, n);
    return 1;
}
static int barrier(void *ctx, enum pvr_ccb_barrier kind) { (void)ctx; (void)kind; return 1; }
static int kick(void *ctx, uint32_t reg, uint32_t value)
{ (void)ctx; (void)reg; (void)value; assert(0); return 0; }
static FILE *open_output(const char *dir, const char *name)
{
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
    assert(n > 0 && (size_t)n < sizeof(path));
    FILE *f = fopen(path, "wb"); assert(f); return f;
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    assert(!pvr_fw_state_layout((enum pvr_fw_resource)-1));
    assert(!pvr_fw_state_layout(PVR_FW_RESOURCE_COUNT));
    uint64_t pages[PVR_FW_RESOURCE_COUNT][12];
    unsigned physical_page = 0;
    for (unsigned i = 0; i < PVR_FW_RESOURCE_COUNT; ++i) {
        const struct pvr_fw_resource_layout *l = pvr_fw_state_layout((enum pvr_fw_resource)i);
        memory[i] = malloc(l->allocation_bytes + 130); assert(memory[i]);
        buffers[i] = (struct pvr_mips_buffer){{memory[i] + 65, l->allocation_bytes},
            pages[i], l->allocation_bytes / 4096};
        for (unsigned j = 0; j < buffers[i].page_count; ++j)
            pages[i][j] = UINT64_C(0x440000000) + physical_page++ * 0x3000;
    }
    assert(physical_page == 45);
    fill();
    assert(pvr_fw_state_prepare(buffers, &config, mappings));
    for (unsigned i = 0; i < 5; ++i) pattern(buffers[i].storage.cpu, buffers[i].storage.bytes);
    for (unsigned i = 0; i < PVR_FW_RESOURCE_COUNT; ++i) {
        pattern(memory[i], 65); pattern(memory[i] + 65 + buffers[i].storage.bytes, 65);
        assert(mappings[i].pages == pages[i] && mappings[i].page_count == buffers[i].page_count);
    }

    /* Integrate queue formatting with the same resource allocations. */
    const struct pvr_ccb_ops ops = {read_memory, write_memory, barrier, kick};
    struct pvr_ccb queue = {0};
    assert(pvr_ccb_init(&queue, &ops, NULL, 1) == PVR_CCB_OK);
    assert(queue.phase == PVR_CCB_PREPARED);

    FILE *f = fopen(argv[1], "rb"); assert(f);
    unsigned char *image = malloc(131072); assert(image);
    assert(fread(image, 1, 131072, f) == 131072 && !fclose(f));
    struct pvr_mips_buffer firmware[3];
    uint64_t fw_pages[3][25];
    const uint32_t sizes[] = {102400, 20480, 16384};
    for (unsigned i = 0; i < 3; ++i) {
        firmware[i] = (struct pvr_mips_buffer){{malloc(sizes[i]), sizes[i]}, fw_pages[i], sizes[i] / 4096};
        assert(firmware[i].storage.cpu);
        for (unsigned j = 0; j < firmware[i].page_count; ++j)
            fw_pages[i][j] = (UINT64_C(0x140000000) + ((uint64_t)i << 32)) + j * 0x5000;
    }
    struct pvr_mips_boot boot;
    assert(pvr_mips_prepare(image, 131072, firmware, mappings, PVR_FW_RESOURCE_COUNT, &boot));
    const unsigned char *table = firmware[2].storage.cpu;
    for (unsigned i = 0; i < PVR_FW_RESOURCE_COUNT; ++i) {
        for (unsigned j = 0; j < buffers[i].page_count; ++j) {
            unsigned index = (mappings[i].address - 0xc0000000) / 4096 + j;
            uint32_t pte = (uint32_t)read_le(table + index * 4, 4);
            assert(((uint64_t)(pte & 0x3fffffc0) << 6) == pages[i][j] && (pte & 63) == 23);
        }
    }
    unsigned mapped = 0;
    for (unsigned i = 0; i < 4096; ++i) if (read_le(table + i * 4, 4)) ++mapped;
    assert(mapped == 75);
    assert(read_le(buffers[PVR_FW_SYSTEM_INIT].storage.cpu, 8) == pages[PVR_FW_FAULT_PAGE][0]);

    /* Save all allocated bytes and the supplied addresses for an independent
     * comparison with the original initialization functions. */
    f = open_output(argv[2], "fixture.json");
    fprintf(f, "{\"clock_hz\":123456789,\"clusters\":1,\"slc_kilobytes\":16,\"resources\":[");
    for (unsigned i = 0; i < PVR_FW_RESOURCE_COUNT; ++i) {
        const struct pvr_fw_resource_layout *l = pvr_fw_state_layout((enum pvr_fw_resource)i);
        fprintf(f, "%s{\"address\":%u,\"bytes\":%u,\"allocation_bytes\":%u,\"cpu\":%llu,\"physical\":%llu}",
            i ? "," : "", l->address, l->bytes, l->allocation_bytes,
            (unsigned long long)(uintptr_t)buffers[i].storage.cpu, (unsigned long long)pages[i][0]);
        char name[32]; snprintf(name, sizeof(name), "resource-%u.bin", i);
        FILE *out = open_output(argv[2], name);
        assert(fwrite(buffers[i].storage.cpu, 1, buffers[i].storage.bytes, out) == buffers[i].storage.bytes && !fclose(out));
    }
    fprintf(f, "]}\n"); assert(!fclose(f));

    struct pvr_fw_state_config bad_config;
    bad_config = config; bad_config.bvnc = 0; refuse(buffers, &bad_config, mappings);
    bad_config = config; bad_config.gpu_clock_hz = 0; refuse(buffers, &bad_config, mappings);
    bad_config = config; bad_config.clusters = 2; refuse(buffers, &bad_config, mappings);
    bad_config = config; bad_config.slc_kilobytes = 0; refuse(buffers, &bad_config, mappings);
    uint64_t saved = pages[18][0];
    const uint64_t invalid[] = {0, 1, UINT64_C(1) << 36, 0x1fc00000, 0x1fc03000, pages[0][0]};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        pages[18][0] = invalid[i]; refuse(buffers, &config, mappings);
    }
    pages[18][0] = saved;
    saved = pages[13][11]; pages[13][11] = pages[13][0];
    refuse(buffers, &config, mappings); pages[13][11] = saved;
    struct pvr_mips_buffer bad[PVR_FW_RESOURCE_COUNT];
    const unsigned selected[] = {0, 10, 13, 18};
    for (unsigned j = 0; j < sizeof(selected) / sizeof(selected[0]); ++j) {
        unsigned i = selected[j];
        memcpy(bad, buffers, sizeof(bad)); --bad[i].storage.bytes; refuse(bad, &config, mappings);
        memcpy(bad, buffers, sizeof(bad)); --bad[i].page_count; refuse(bad, &config, mappings);
        memcpy(bad, buffers, sizeof(bad)); bad[i].storage.cpu = NULL; refuse(bad, &config, mappings);
        memcpy(bad, buffers, sizeof(bad)); bad[i].pages = NULL; refuse(bad, &config, mappings);
    }
    memcpy(bad, buffers, sizeof(bad)); bad[18].storage.cpu = buffers[0].storage.cpu;
    refuse(bad, &config, mappings);
    memcpy(bad, buffers, sizeof(bad)); bad[18].storage.cpu = mappings;
    refuse(bad, &config, mappings);
    memcpy(bad, buffers, sizeof(bad)); bad[18].storage.cpu = bad;
    refuse(bad, &config, mappings);
    memcpy(bad, buffers, sizeof(bad)); bad[18].storage.cpu = &config;
    refuse(bad, &config, mappings);
    memcpy(bad, buffers, sizeof(bad)); bad[18].pages = (uint64_t *)(memory[18] + 64);
    refuse(bad, &config, mappings);
    refuse(buffers, &config, (struct pvr_mips_object *)(memory[18] + 64));
    refuse(NULL, &config, mappings); refuse(buffers, NULL, mappings); refuse(buffers, &config, NULL);
    for (unsigned i = 0; i < 3; ++i) free(firmware[i].storage.cpu);
    for (unsigned i = 0; i < PVR_FW_RESOURCE_COUNT; ++i) free(memory[i]);
    free(image);
    printf("PASS firmware initialization, queue formatting, 75 integrated MIPS mappings and %u atomic refusals\n", refusals);
}
