#include "nbg_model.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *image, *original;
static size_t image_bytes;
static struct nbg_model view;
static unsigned refused, mutated;

static void put(unsigned char *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (i * 8));
}

static void section(unsigned char *data, unsigned id, uint32_t offset, uint32_t bytes)
{
    put(data + 376 + id * 8, offset); put(data + 380 + id * 8, bytes);
}

static void refuse(const void *data, size_t bytes)
{
    struct nbg_model saved;
    memset(&view, 0xa5, sizeof(view)); memcpy(&saved, &view, sizeof(view));
    assert(nbg_model_open(data, bytes, &view) != NBG_DECODE_OK);
    assert(!memcmp(&saved, &view, sizeof(view)));
    ++refused;
}

static void bad_word(size_t offset, uint32_t value)
{
    memcpy(image, original, image_bytes); put(image + offset, value);
    refuse(image, image_bytes);
}

static void exercise_views(const struct nbg_model *m)
{
    struct nbg_span span;
    for (unsigned i = 0; i < NBG_SECTION_COUNT; ++i) {
        assert(nbg_model_section(m, (enum nbg_section_id)i, &span));
        assert(span.data >= m->data && span.data <= m->data + m->bytes);
        assert(span.bytes <= (size_t)(m->data + m->bytes - span.data));
    }
    for (unsigned i = 0; i < m->header.operation_count; ++i) {
        struct nbg_operation op;
        assert(nbg_model_operation(m, i, &op));
        assert(nbg_model_table(m, NBG_LOAD_STATES_TABLE, op.state, &span));
        for (uint32_t j = 0; j < op.patch_count; ++j) {
            struct nbg_patch p;
            assert(nbg_model_patch(m, op.first_patch + j, &p));
        }
    }
    for (unsigned id = NBG_INPUT; id <= NBG_OUTPUT; ++id) {
        uint32_t count = id == NBG_INPUT ? m->header.input_count : m->header.output_count;
        for (uint32_t i = 0; i < count; ++i) {
            struct nbg_tensor t;
            assert(nbg_model_tensor(m, (enum nbg_section_id)id, i, &t));
            assert(t.logical_bytes && t.name[64] == 0);
        }
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *f = fopen(argv[1], "rb"); assert(f);
    assert(!fseek(f, 0, SEEK_END)); long length = ftell(f); assert(length == 964224);
    image_bytes = (size_t)length; rewind(f);
    original = malloc(image_bytes); assert(original);
    assert(fread(original, 1, image_bytes, f) == image_bytes); fclose(f);
    unsigned char *storage = malloc(image_bytes + 2); assert(storage);
    image = storage + 1; /* Exercise byte loads from an unaligned model. */
    storage[0] = storage[image_bytes + 1] = 0x5a;
    memcpy(image, original, image_bytes);
    assert(nbg_model_open(image, image_bytes, &view) == NBG_DECODE_OK);
    assert(view.header.target == 0x1000003b && view.header.version == 0x1001e);
    assert(!strcmp(view.header.name, "ShuffleNetV2_uint8_NCHW"));
    assert(view.header.layer_count == 87 && view.header.operation_count == 126);
    assert(view.header.nn_bytes == 192 && view.header.pool_bytes == 1092352);
    assert(view.header.vip_sram_bytes == 518144 && view.header.checksum == 0xdd476ad1);
    struct nbg_tensor tensor;
    assert(nbg_model_tensor(&view, NBG_INPUT, 0, &tensor) && tensor.logical_bytes == 150528);
    assert(nbg_model_tensor(&view, NBG_OUTPUT, 0, &tensor) && tensor.logical_bytes == 2);
    exercise_views(&view);

    struct nbg_model saved;
    memcpy(&saved, &view, sizeof(view));
    struct nbg_span span, span_saved;
    memset(&span, 0xa5, sizeof(span)); memcpy(&span_saved, &span, sizeof(span));
    assert(!nbg_model_section(&view, (enum nbg_section_id)-1, &span));
    assert(!nbg_model_section(&view, NBG_SECTION_COUNT, &span));
    assert(!nbg_model_table(&view, NBG_COEFF, 0, &span));
    assert(!nbg_model_table(&view, NBG_COEFF_TABLE, UINT32_MAX, &span));
    assert(!memcmp(&span, &span_saved, sizeof(span)));
    struct nbg_operation operation, operation_saved;
    memset(&operation, 0xa5, sizeof(operation)); memcpy(&operation_saved, &operation, sizeof(operation));
    assert(!nbg_model_operation(&view, 126, &operation));
    assert(!nbg_model_operation(&view, UINT32_MAX, &operation));
    assert(!memcmp(&operation, &operation_saved, sizeof(operation)));
    struct nbg_patch patch, patch_saved;
    memset(&patch, 0xa5, sizeof(patch)); memcpy(&patch_saved, &patch, sizeof(patch));
    assert(!nbg_model_patch(&view, 660, &patch));
    assert(!memcmp(&patch, &patch_saved, sizeof(patch)));
    struct nbg_tensor tensor_saved;
    memcpy(&tensor_saved, &tensor, sizeof(tensor));
    assert(!nbg_model_tensor(&view, NBG_INPUT, 1, &tensor));
    assert(!nbg_model_tensor(&view, NBG_LAYER, 0, &tensor));
    assert(!memcmp(&tensor, &tensor_saved, sizeof(tensor)));
    assert(!nbg_model_section(&view, NBG_INPUT, (void *)&view));
    assert(!nbg_model_tensor(&view, NBG_INPUT, 0, (void *)(image + 15)));
    assert(!memcmp(&view, &saved, sizeof(view)));
    assert(!memcmp(image, original, image_bytes));
    assert(!nbg_model_section(NULL, NBG_INPUT, &span));
    assert(!nbg_model_section(&view, NBG_INPUT, NULL));
    assert(nbg_model_open(image, image_bytes, (void *)(image + 15)) == NBG_DECODE_INVALID);
    assert(!memcmp(image, original, image_bytes));
    refuse(NULL, image_bytes); refuse((void *)(UINTPTR_MAX - 32), image_bytes);
    refuse(image, NBG_MODEL_MAX_BYTES + (size_t)1);
    for (size_t n = 0; n < NBG_MODEL_HEADER_BYTES; ++n) refuse(image, n);
    refuse(image, image_bytes - 1);
    bad_word(0, 0); bad_word(4, 0x10014); bad_word(76, UINT32_MAX);
    bad_word(80, 0); bad_word(84, 2); bad_word(88, 0);
    bad_word(92, 8); bad_word(92, 64); bad_word(352, 0); bad_word(352, 3);
    bad_word(376, UINT32_MAX); bad_word(380, UINT32_MAX); bad_word(376, 0);
    bad_word(384, 592); /* Input/output alias. */
    bad_word(380 + NBG_PATCH * 8, 26399);
    bad_word(592, 0); bad_word(592, 7); bad_word(596, 0);
    bad_word(596, UINT32_MAX); bad_word(620, 15); bad_word(624, 4); bad_word(628, 3);
    for (unsigned i = 0; i < 5; ++i) {
        const uint32_t scales[] = {0,0x80000000,0x7f800000,0x7fc00001,0xbf800000};
        bad_word(636, scales[i]);
    }
    bad_word(7436 + 32 + 4, 125); bad_word(7436 + 32 + 8, 87);
    bad_word(7436 + 12, 126); bad_word(7436 + 16, UINT32_MAX);
    bad_word(7436 + 20, UINT32_MAX);
    bad_word(61868, UINT32_MAX); bad_word(61868 + 4, UINT32_MAX);
    bad_word(62348, 8065); bad_word(62348 + 4, 8065);
    /* First input relocation follows initialization and first state pointer. */
    bad_word(35468 + 2 * 40 + 12, UINT32_MAX);

    memcpy(image, original, image_bytes);
    put(image + 592, 1); put(image + 596, 3); put(image + 620, 13); put(image + 628, 0);
    put(image + 632, 0x80000000); put(image + 640, 0xffffffff);
    memset(image + 644, 'x', 64);
    assert(nbg_model_open(image, image_bytes, &view) == NBG_DECODE_OK);
    assert(nbg_model_tensor(&view, NBG_INPUT, 0, &tensor));
    assert(tensor.logical_bytes == 2 && tensor.fixed_position == INT32_MIN && tensor.zero_point == -1);
    assert(strlen(tensor.name) == 64);
    put(image + 620, 2); put(image + 596, UINT32_MAX); put(image + 628, 2); put(image + 636, 1);
    assert(nbg_model_open(image, image_bytes, &view) == NBG_DECODE_OK);
    assert(nbg_model_tensor(&view, NBG_INPUT, 0, &tensor));
    assert(tensor.logical_bytes == UINT32_MAX && tensor.scale_bits == 1);

    /* Unknown op/source values are inventory, not implicit execution support. */
    memcpy(image, original, image_bytes); put(image + 7436 + 32, 99);
    put(image + 35468 + 8, 0x12345678); put(image + 35468 + 12, 0x80000000);
    assert(nbg_model_open(image, image_bytes, &view) == NBG_DECODE_OK);
    assert(nbg_model_operation(&view, 1, &operation) && operation.kind == 99);
    assert(nbg_model_patch(&view, 0, &patch) && patch.source == 0x12345678 && patch.index == INT32_MIN);

    /* Exercise hardware-init references using the fixture's unused table gap. */
    memcpy(image, original, image_bytes); section(image, NBG_HW_INIT, 63356, 12);
    put(image + 63356, 0); put(image + 63360, 0); put(image + 63364, 1);
    assert(nbg_model_open(image, image_bytes, &view) == NBG_DECODE_OK);
    put(image + 63356, UINT32_MAX); refuse(image, image_bytes);

    unsigned char tiny[664] = {0}; memcpy(tiny, "VPMN", 4);
    put(tiny + 4, NBG_MODEL_VERSION); put(tiny + 352, 64);
    const unsigned tables[] = {NBG_COEFF_HEADER_TABLE, NBG_COEFF_TABLE, NBG_PPU_INSTR_TABLE,
                               NBG_DYNA_IO_TABLE, NBG_LOAD_STATES_TABLE, NBG_ICDT};
    const unsigned targets[] = {NBG_COEFF_HEADER, NBG_COEFF, NBG_PPU_INSTR,
                                NBG_DYNA_IO, NBG_LOAD_STATES, NBG_ICD};
    for (unsigned i = 0; i < 6; ++i) {
        unsigned start = 592 + i * 12;
        section(tiny, tables[i], start, 8); section(tiny, targets[i], start + 8, 4);
        put(tiny + start, 3); put(tiny + start + 4, 1); tiny[start + 11] = (unsigned char)(i + 1);
    }
    assert(nbg_model_open(tiny, sizeof(tiny), &view) == NBG_DECODE_OK);
    for (unsigned i = 0; i < 6; ++i) {
        assert(nbg_model_table(&view, (enum nbg_section_id)tables[i], 0, &span));
        assert(span.bytes == 1 && *span.data == i + 1);
    }
    put(tiny + 592, 4); put(tiny + 596, 0);
    assert(nbg_model_open(tiny, sizeof(tiny), &view) == NBG_DECODE_OK);
    assert(nbg_model_table(&view, NBG_COEFF_HEADER_TABLE, 0, &span) && span.bytes == 0);
    put(tiny + 596, 1); refuse(tiny, sizeof(tiny));

    /* Bounded mutation campaign for parser memory safety; valid mutations need
     * not be rejected because this component does not admit command semantics. */
    uint32_t seed = 733;
    for (unsigned trial = 0; trial < 512; ++trial) {
        memcpy(image, original, image_bytes);
        seed = seed * 1664525u + 1013904223u;
        size_t offset = trial % 3 == 0 ? seed % 592 :
                        trial % 3 == 1 ? 7436 + seed % 4032 : 35468 + seed % 26400;
        image[offset] ^= (unsigned char)(1u << ((seed >> 24) & 7));
        memset(&view, 0xa5, sizeof(view)); memcpy(&saved, &view, sizeof(view));
        size_t size = trial % 7 ? image_bytes : seed % image_bytes;
        enum nbg_decode_result status = nbg_model_open(image, size, &view);
        if (status == NBG_DECODE_OK) exercise_views(&view);
        else assert(!memcmp(&saved, &view, sizeof(view)));
        ++mutated;
    }
    assert(storage[0] == 0x5a && storage[image_bytes + 1] == 0x5a);
    printf("PASS native NBG decoder: %u refusals, %u bounded mutations, borrowed views and table boundaries\n",
           refused, mutated);
    free(storage); free(original);
}
