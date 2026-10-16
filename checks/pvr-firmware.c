#include "pvr_firmware.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put32(unsigned char *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(n >> (8 * i));
}
static void save(const char *path, const void *data, size_t bytes)
{
    FILE *f = fopen(path, "wb");
    assert(f && fwrite(data, 1, bytes, f) == bytes && !fclose(f));
}
static void pattern(const unsigned char *p, size_t n, unsigned char value)
{
    for (size_t i = 0; i < n; ++i) assert(p[i] == value);
}
int main(int argc, char **argv)
{
    assert(argc == 4);
    FILE *f = fopen(argv[1], "rb");
    assert(f && !fseek(f, 0, SEEK_END));
    long length = ftell(f);
    assert(length == 131072 && !fseek(f, 0, SEEK_SET));
    size_t bytes = (size_t)length;
    unsigned char *storage = malloc(bytes + 1), *original = malloc(bytes), *copy = malloc(bytes);
    assert(storage && original && copy);
    unsigned char *image = storage + 1; /* Exercise unaligned input. */
    assert(fread(image, 1, bytes, f) == bytes && !fclose(f));
    memcpy(original, image, bytes);
    struct pvr_fw_plan plan, before;
    assert(pvr_fw_requirements(image, bytes, PVR_FW_A733_BVNC, &plan));
    assert(plan.major == 1 && plan.minor == 1 && plan.build == 6976702);
    assert(plan.entry == 0xbfc00001 && plan.load_count == 5);
    assert(plan.allocation_bytes[0] == 102400 && plan.allocation_bytes[1] == 20480);
    assert(plan.device_info_offset == 122880 && plan.device_info_bytes == 4096);
    assert(plan.device_info_counts[0] == 1 && plan.device_info_counts[1] == 1 &&
           plan.device_info_counts[2] == 2 && plan.device_info_counts[3] == 29);
    unsigned char *alloc[2];
    struct pvr_fw_buffer buffers[2];
    for (unsigned i = 0; i < 2; ++i) {
        alloc[i] = malloc(plan.allocation_bytes[i] + 128);
        assert(alloc[i]);
        memset(alloc[i], 0x5a, plan.allocation_bytes[i] + 128);
        buffers[i] = (struct pvr_fw_buffer){alloc[i] + 64, plan.allocation_bytes[i]};
    }
    assert(pvr_fw_stage(image, bytes, PVR_FW_A733_BVNC, buffers));
    for (unsigned i = 0; i < 2; ++i) {
        pattern(alloc[i], 64, 0x5a);
        pattern(alloc[i] + 64 + buffers[i].bytes, 64, 0x5a);
        save(argv[2 + i], buffers[i].cpu, buffers[i].bytes);
    }
    assert(!memcmp(image, original, bytes));

    /* Structural, compatibility, source bounds, and destination bounds. */
    const uint32_t footer = 126976, layout = footer + 48, ph = 52, info = 122880;
    const struct { uint32_t offset, value; } cases[] = {
        {footer, 2}, {footer + 4, 4095}, {footer + 8, 0xffffffff},
        {footer + 12, 0xffffffff}, {footer + 16, 0}, {footer + 24, 8192},
        {footer + 28, 0}, {footer + 32, 0x10002}, {footer + 40, 0xffffffff},
        {footer + 40, 31}, {footer + 44, 1}, {info, 0xffffffff},
        {info + 24, 0xffffffff}, {info + 48, 0},
        {layout, 10}, {layout + 24, 4}, {layout + 4, 2},
        {layout + 16, 0}, {layout + 16, 0x100000},
        {layout + 8, 0xfffff000}, {layout + 20, 0xfffff000},
        {layout + 24 + 8, 0xc0000000}, {layout + 24 + 20, 0},
        {0, 0}, {16, 0x00080003}, {20, 2}, {24, 0xc0000001},
        {28, 0xfffffff0}, {40, 0x00100034}, {44, 0xffff},
        {ph + 4, 126976}, {ph + 8, 0x100000},
        {ph + 16, 0xffffffff}, {ph + 20, 0}, {ph + 20, 8192},
        {ph + 24, 6}, {ph + 28, 3},
        {ph + 32 + 8, 0xbfc00000 + 4096 - 4},
        {ph + 128 + 8, 0xc0032000},
    };
    memset(&before, 0xa5, sizeof(before));
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        memcpy(copy, original, bytes);
        put32(copy + cases[c].offset, cases[c].value);
        struct pvr_fw_plan result = before;
        if (pvr_fw_requirements(copy, bytes, PVR_FW_A733_BVNC, &result)) {
            fprintf(stderr, "unexpected acceptance: case %u at %#x\n", c, cases[c].offset);
            abort();
        }
        assert(!memcmp(&result, &before, sizeof(result)));
        for (unsigned i = 0; i < 2; ++i) memset(alloc[i], 0x5a, buffers[i].bytes + 128);
        assert(!pvr_fw_stage(copy, bytes, PVR_FW_A733_BVNC, buffers));
        for (unsigned i = 0; i < 2; ++i) pattern(alloc[i], buffers[i].bytes + 128, 0x5a);
    }
    assert(!pvr_fw_requirements(image, bytes, PVR_FW_A733_BVNC + 1, &before));
    assert(!pvr_fw_requirements(image, bytes - 1, PVR_FW_A733_BVNC, &before));
    assert(!pvr_fw_requirements(image, 4096, PVR_FW_A733_BVNC, &before));
    assert(!pvr_fw_requirements(image, bytes, PVR_FW_A733_BVNC, (void *)image));
    assert(!memcmp(image, original, bytes));
    struct pvr_fw_buffer bad[2] = {buffers[0], buffers[1]};
    bad[1].bytes--;
    assert(!pvr_fw_stage(image, bytes, PVR_FW_A733_BVNC, bad));
    bad[1] = buffers[1]; bad[1].cpu = buffers[0].cpu;
    assert(!pvr_fw_stage(image, bytes, PVR_FW_A733_BVNC, bad));
    bad[0].cpu = image; bad[1] = buffers[1];
    assert(!pvr_fw_stage(image, bytes, PVR_FW_A733_BVNC, bad));
    bad[0] = buffers[0]; bad[1].cpu = bad;
    assert(!pvr_fw_stage(image, bytes, PVR_FW_A733_BVNC, bad));
    assert(!pvr_fw_stage(image, bytes, PVR_FW_A733_BVNC, NULL));
    for (unsigned i = 0; i < 2; ++i) {
        pattern(alloc[i], buffers[i].bytes + 128, 0x5a);
        free(alloc[i]);
    }
    free(storage); free(original); free(copy);
    printf("PASS firmware staging, unaligned input, guards, %zu malformed-image refusals, and buffer refusal cases\n",
           sizeof(cases) / sizeof(cases[0]));
    return 0;
}
