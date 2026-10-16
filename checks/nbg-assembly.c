#include "nbg_assembly.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *original, *image;
static size_t image_bytes;
static struct nbg_model view;
static struct nbg_assembly_plan plan;
static struct nbg_assembly_buffer buffers[NBG_ALLOCATION_COUNT];
static unsigned char *storage[NBG_ALLOCATION_COUNT], *saved[NBG_ALLOCATION_COUNT];
static unsigned refusals;
static void put(unsigned char *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (i * 8));
}
static uint32_t get(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void unchanged(void)
{
    for (unsigned i = 0; i < NBG_ALLOCATION_COUNT; ++i) {
        assert(!memcmp(storage[i], saved[i], plan.buffers[i].allocation_bytes + 8192));
    }
}
static void refuse(struct nbg_assembly_plan *out)
{
    unsigned char before[sizeof(*out)]; memcpy(before, out, sizeof(*out));
    assert(!nbg_assemble(&view, buffers, out));
    assert(!memcmp(before, out, sizeof(*out))); unchanged(); ++refusals;
}
static void reset(void)
{
    memcpy(image, original, image_bytes);
    assert(nbg_model_open(image, image_bytes, &view) == NBG_DECODE_OK);
}
static void bad_word(size_t offset, uint32_t value)
{
    reset(); put(image + offset, value);
    assert(nbg_model_open(image, image_bytes, &view) == NBG_DECODE_OK);
    struct nbg_assembly_plan out, before;
    memset(&out, 0xa5, sizeof(out)); memcpy(&before, &out, sizeof(out));
    assert(!nbg_assembly_requirements(&view, &out));
    assert(!memcmp(&out, &before, sizeof(out)));
    refuse(&out);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *f = fopen(argv[1], "rb"); assert(f);
    assert(!fseek(f, 0, SEEK_END)); long length = ftell(f); assert(length == 964224);
    image_bytes = (size_t)length; rewind(f);
    original = malloc(image_bytes); image = malloc(image_bytes); assert(original && image);
    assert(fread(original, 1, image_bytes, f) == image_bytes); fclose(f); reset();
    assert(nbg_assembly_requirements(&view, &plan));
    assert(plan.nn_operations == 125 && plan.descriptor_patches == 534 && plan.state_patches == 125);
    assert(plan.command_body_bytes == 3056);
    const uint32_t payload[] = {24000, 892608, 1092352, 3112, 150528, 2};
    const uint32_t minimum[] = {24000, 892736, 1092352, 524280, 150592, 128};
    uint32_t device = 0xe0000000;
    for (unsigned i = 0; i < NBG_ALLOCATION_COUNT; ++i) {
        assert(plan.buffers[i].payload_bytes == payload[i] && plan.buffers[i].minimum_bytes == minimum[i]);
        size_t n = plan.buffers[i].allocation_bytes;
        storage[i] = aligned_alloc(4096, n + 8192); saved[i] = malloc(n + 8192);
        assert(storage[i] && saved[i]); memset(storage[i], 0xa5, n + 8192);
        buffers[i] = (struct nbg_assembly_buffer){storage[i] + 4096, n, device};
        device += (uint32_t)n + 4096;
    }
    struct nbg_assembly_plan out;
    assert(nbg_assemble(&view, buffers, &out)); assert(!memcmp(&out, &plan, sizeof(out)));
    for (unsigned i = 0; i < NBG_ALLOCATION_COUNT; ++i) {
        for (unsigned j = 0; j < 4096; ++j)
            assert(storage[i][j] == 0xa5 && storage[i][4096 + buffers[i].bytes + j] == 0xa5);
        unsigned char *data = buffers[i].cpu;
        size_t begin = i == NBG_ALLOC_INPUT || i == NBG_ALLOC_OUTPUT || i == NBG_ALLOC_POOL ? 0 : payload[i];
        if (i != NBG_ALLOC_COMMAND)
            for (size_t j = begin; j < buffers[i].bytes; ++j) assert(!data[j]);
        memcpy(saved[i], storage[i], buffers[i].bytes + 8192);
    }
    assert(!memcmp(buffers[NBG_ALLOC_COEFF].cpu, image + view.sections[NBG_COEFF].offset, payload[NBG_ALLOC_COEFF]));
    unsigned char *cmd = buffers[NBG_ALLOC_COMMAND].cpu;
    assert(get(cmd + plan.command_body_bytes + 28) == 0x40);
    assert(get(cmd + plan.command_body_bytes + 32) == 0x10000000);
    for (size_t j = payload[NBG_ALLOC_COMMAND]; j < buffers[NBG_ALLOC_COMMAND].bytes; j += 8)
        assert(get(cmd + j) == 0x10000000 && get(cmd + j + 4) == 0);

    /* Refusals exercise policies added by assembly, not container parsing again. */
    bad_word(8, 0x1000003c); bad_word(92, 0x02d00026); bad_word(100, 0x20002);
    bad_word(352, 128); bad_word(356, 64); bad_word(360, 0x480000); bad_word(364, 64);
    bad_word(368, 0x400000); bad_word(372, 0x80001); bad_word(372, 0);
    bad_word(348, 0x10000000); /* Total allocation limit. */
    reset(); uint32_t input = view.sections[NBG_INPUT].offset;
    bad_word(input + 28, 3); bad_word(input + 32, 1); bad_word(input + 36, 0);
    bad_word(input + 4, 0x100000); /* Valid logical tensor, excessive allocation. */
    reset(); uint32_t ops = view.sections[NBG_OPERATION].offset;
    bad_word(ops, 0xfffe); bad_word(ops + 24, 1); bad_word(ops + 28, 0);
    bad_word(ops + 32, 4); bad_word(ops + 36, 1); bad_word(ops + 44, 2);
    bad_word(ops + 48, 2); bad_word(ops + 56, 1);
    reset(); uint32_t states = view.sections[NBG_LOAD_STATES].offset;
    bad_word(states + 4, 0x12); bad_word(states + 64, 0x10000000);
    bad_word(states + 125 * 64 + 20, 1); /* Last operation requires terminal flags. */
    reset(); uint32_t patches = view.sections[NBG_PATCH].offset;
    bad_word(patches + 4, 24); bad_word(patches + 20, 1);
    bad_word(patches + 40 + 4, 16); bad_word(patches + 40 + 20, 0);
    bad_word(patches + 80, 2); bad_word(patches + 80 + 4, 192);
    bad_word(patches + 80 + 4, 25); bad_word(patches + 80 + 8, 99);
    bad_word(patches + 80 + 16, 150592); bad_word(patches + 80 + 20, 2);
    for (unsigned i = 24; i <= 36; i += 4) bad_word(patches + 80 + i, 1);
    bad_word(patches + 120 + 4, 24); /* Duplicate descriptor destination. */
    bad_word(patches + 120 + 12, 0); bad_word(patches + 120 + 16, 1092352);
    bad_word(patches + 160 + 16, 1); /* Transformed data address alignment. */
    bad_word(patches + 200 + 12, UINT32_MAX); bad_word(patches + 200 + 12, 60);
    bad_word(patches + 200 + 16, UINT32_MAX);
    bad_word(patches + 659 * 40 + 4, 192); /* Failure after all earlier operations. */
    reset();

    for (unsigned i = 0; i < NBG_ALLOCATION_COUNT; ++i) {
        struct nbg_assembly_buffer b = buffers[i];
        buffers[i].bytes--; refuse(&out); buffers[i] = b;
        buffers[i].bytes += 4096; refuse(&out); buffers[i] = b;
        buffers[i].cpu = (unsigned char *)b.cpu + 1; refuse(&out); buffers[i] = b;
        buffers[i].cpu = NULL; refuse(&out); buffers[i] = b;
        buffers[i].device = 0; refuse(&out); buffers[i] = b;
        buffers[i].device++; refuse(&out); buffers[i] = b;
        buffers[i].device = 0x400000; refuse(&out); buffers[i] = b;
        if (b.bytes > 4096) {
            buffers[i].device = 0xfffff000; refuse(&out); buffers[i] = b;
        }
    }
    struct nbg_assembly_buffer b = buffers[1];
    buffers[1].cpu = buffers[0].cpu; refuse(&out); buffers[1] = b;
    buffers[1].device = buffers[0].device; refuse(&out); buffers[1] = b;
    buffers[1].cpu = (void *)(UINTPTR_MAX & ~(uintptr_t)4095); refuse(&out); buffers[1] = b;
    refuse((struct nbg_assembly_plan *)buffers[0].cpu);
    refuse((struct nbg_assembly_plan *)image);
    refuse((struct nbg_assembly_plan *)&view);
    refuse((struct nbg_assembly_plan *)buffers);
    /* Metadata and view aliases with a valid aligned output base. */
    struct nbg_assembly_buffer *alias = buffers[0].cpu;
    memcpy(alias, buffers, sizeof(buffers));
    memcpy(saved[0], storage[0], buffers[0].bytes + 8192);
    assert(!nbg_assemble(&view, alias, &out)); unchanged(); ++refusals;
    struct nbg_model *alias_view = buffers[0].cpu; memcpy(alias_view, &view, sizeof(view));
    memcpy(saved[0], storage[0], buffers[0].bytes + 8192);
    assert(!nbg_assemble(alias_view, buffers, &out)); unchanged(); ++refusals;

    /* A final allocation may end at the top of the 32-bit address space. */
    buffers[NBG_ALLOC_OUTPUT].device = 0xfffff000;
    assert(nbg_assemble(&view, buffers, &out));
    /* Reassembly is deterministic and resets caller payload contents. */
    for (unsigned i = 0; i < NBG_ALLOCATION_COUNT; ++i)
        memcpy(saved[i], storage[i], buffers[i].bytes + 8192);
    memset(buffers[NBG_ALLOC_INPUT].cpu, 0x37, buffers[NBG_ALLOC_INPUT].bytes);
    assert(nbg_assemble(&view, buffers, &out)); unchanged();
    printf("PASS native assembly: full fixture, padding, event 0, atomic refusal (%u cases), top address, reassembly\n", refusals);
    for (unsigned i = 0; i < NBG_ALLOCATION_COUNT; ++i) { free(storage[i]); free(saved[i]); }
    free(original); free(image);
}
