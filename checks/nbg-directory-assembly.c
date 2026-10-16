#include "nbg_assembly.h"
#include "nbg_checksum.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char original[1928], image[1928];
static struct nbg_model view;
static struct nbg_assembly_plan plan;
static struct nbg_assembly_buffer buffers[NBG_ALLOCATION_COUNT];
static unsigned char *storage[NBG_ALLOCATION_COUNT], *saved[NBG_ALLOCATION_COUNT];
static unsigned refusals;

static uint32_t word(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put(unsigned char *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (8 * i));
}
/* Independent bitwise CRC reference. All checksum value words are zero for
 * the primary CRC; section CRCs cover their own unmodified byte intervals. */
static uint32_t crc(size_t start, size_t bytes, int primary)
{
    uint32_t value = UINT32_MAX;
    for (size_t i = start; i < start + bytes; ++i) {
        if (!primary || !((i >= 860 && i < 864) || (i >= 868 && i < 872) ||
                          (i >= 876 && i < 880))) value ^= image[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ ((value & 1) ? 0xedb88320u : 0);
    }
    return ~value;
}
static void reset(void)
{
    memcpy(image, original, sizeof(image));
    assert(nbg_model_open(image, sizeof(image), &view) == NBG_DECODE_OK);
}
static void repair(void)
{
    put(image + 860, crc(1244, 192, 0));
    put(image + 868, crc(1600, 256, 0));
    put(image + 876, crc(1856, 72, 0));
    put(image + 12, crc(632, sizeof(image) - 632, 1));
    struct nbg_checksum checksum;
    assert(nbg_checksum_verify(image, sizeof(image), &checksum) == NBG_CHECKSUM_MATCH);
    assert(nbg_model_open(image, sizeof(image), &view) == NBG_DECODE_OK);
}
static void unchanged(void)
{
    for (unsigned i = 0; i < NBG_ALLOCATION_COUNT; ++i)
        if (storage[i]) assert(!memcmp(storage[i], saved[i], plan.buffers[i].allocation_bytes + 8192));
}
static void refuse(void)
{
    struct nbg_assembly_plan out, before;
    memset(&out, 0xa5, sizeof(out)); memcpy(&before, &out, sizeof(out));
    assert(!nbg_assemble(&view, buffers, &out));
    assert(!memcmp(&out, &before, sizeof(out))); unchanged(); ++refusals;
}
static void refuse_profile(void)
{
    struct nbg_assembly_plan out, before;
    memset(&out, 0xa5, sizeof(out)); memcpy(&before, &out, sizeof(out));
    assert(!nbg_assembly_requirements(&view, &out));
    assert(!memcmp(&out, &before, sizeof(out))); refuse();
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *f = fopen(argv[1], "rb"); assert(f);
    assert(fread(original, 1, sizeof(original), f) == sizeof(original));
    assert(fgetc(f) == EOF); fclose(f); reset();
    assert(nbg_assembly_requirements(&view, &plan));
    assert(plan.nn_operations == 1 && plan.descriptor_patches == 3 && plan.state_patches == 1);
    assert(plan.command_body_bytes == 80);
    const uint32_t payload[] = {192, 256, 0, 136, 192, 72};
    const uint32_t minimum[] = {192, 384, 0, 524280, 256, 192};
    const uint32_t allocation[] = {4096, 4096, 0, 524288, 4096, 4096};
    uint32_t device = 0xe0000000;
    for (unsigned i = 0; i < NBG_ALLOCATION_COUNT; ++i) {
        assert(plan.buffers[i].payload_bytes == payload[i] && plan.buffers[i].minimum_bytes == minimum[i]);
        assert(plan.buffers[i].allocation_bytes == allocation[i]);
        if (!allocation[i]) continue;
        size_t n = allocation[i];
        storage[i] = aligned_alloc(4096, n + 8192); saved[i] = malloc(n + 8192);
        assert(storage[i] && saved[i]); memset(storage[i], 0xa5, n + 8192);
        buffers[i] = (struct nbg_assembly_buffer){storage[i] + 4096, n, device};
        device += (uint32_t)n + 4096;
    }
    struct nbg_assembly_plan out;
    assert(nbg_assemble(&view, buffers, &out)); assert(!memcmp(&out, &plan, sizeof(out)));
    for (unsigned i = 0; i < NBG_ALLOCATION_COUNT; ++i) {
        if (!allocation[i]) continue;
        for (unsigned j = 0; j < 4096; ++j)
            assert(storage[i][j] == 0xa5 && storage[i][4096 + allocation[i] + j] == 0xa5);
        unsigned char *data = buffers[i].cpu;
        size_t start = i == NBG_ALLOC_INPUT || i == NBG_ALLOC_OUTPUT ? 0 : payload[i];
        if (i != NBG_ALLOC_COMMAND)
            for (size_t j = start; j < allocation[i]; ++j) assert(!data[j]);
        memcpy(saved[i], storage[i], allocation[i] + 8192);
    }
    unsigned char *nn = buffers[NBG_ALLOC_NN].cpu, *command = buffers[NBG_ALLOC_COMMAND].cpu;
    assert((word(nn + 20) & 0x03ffffff) == buffers[NBG_ALLOC_COEFF].device >> 6);
    assert(word(nn + 24) == buffers[NBG_ALLOC_INPUT].device);
    assert(word(nn + 28) == buffers[NBG_ALLOC_OUTPUT].device);
    assert(word(command + 52) == 0x0493e040 && word(command + 68) == buffers[NBG_ALLOC_NN].device);
    assert(word(command + 108) == 0x40);
    assert(!memcmp(buffers[NBG_ALLOC_COEFF].cpu, image + 1600, 256));
    for (size_t i = 136; i < allocation[NBG_ALLOC_COMMAND]; i += 8)
        assert(word(command + i) == 0x10000000 && word(command + i + 4) == 0);

    /* A zero-sized slot must have neither a host pointer nor a device address. */
    buffers[NBG_ALLOC_POOL].cpu = buffers[NBG_ALLOC_INPUT].cpu; refuse();
    buffers[NBG_ALLOC_POOL] = (struct nbg_assembly_buffer){0};
    buffers[NBG_ALLOC_POOL].bytes = 4096; refuse();
    buffers[NBG_ALLOC_POOL] = (struct nbg_assembly_buffer){0};
    buffers[NBG_ALLOC_POOL].device = 0xe1000000; refuse();
    buffers[NBG_ALLOC_POOL] = buffers[NBG_ALLOC_INPUT]; refuse();
    buffers[NBG_ALLOC_POOL] = (struct nbg_assembly_buffer){0};

    const struct {size_t offset; uint32_t value;} mutations[] = {
        {8,0x1000003c}, {632+92,0x0b}, {1180+8,0xffff},
        {1856+40+4,0x0491e040}, {1180+32+20,3},
        {1436+4*28+4,24}, /* Duplicate tensor pointer destination. */
        {1244+80,1}, /* Ring size without an end relocation. */
        {1436+4*28+16,256} /* Weight address beyond the coefficient payload. */
    };
    for (unsigned i = 0; i < sizeof(mutations)/sizeof(mutations[0]); ++i) {
        reset(); put(image + mutations[i].offset, mutations[i].value); repair(); refuse_profile();
    }
    /* A reference to the absent pool cannot become a one-byte allocation. */
    reset(); put(image + 1436 + 2*28 + 8, 3); put(image + 1436 + 2*28 + 12, UINT32_MAX);
    repair(); refuse_profile();
    /* Integrity is required for both the header-excluded target and payload. */
    reset(); image[1600] ^= 1;
    assert(nbg_model_open(image, sizeof(image), &view) == NBG_DECODE_OK); refuse_profile();
    reset();
    buffers[NBG_ALLOC_OUTPUT].device = 0xfffff000;
    assert(nbg_assemble(&view, buffers, &out));
    printf("PASS directory assembly: five allocations, absent pool, guards, padding, event 0, %u atomic refusals, top address\n", refusals);
    for (unsigned i = 0; i < NBG_ALLOCATION_COUNT; ++i) { free(storage[i]); free(saved[i]); }
}
