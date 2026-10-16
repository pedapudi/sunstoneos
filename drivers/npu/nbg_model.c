#include "nbg_model.h"

static uint32_t u32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int32_t i32(const unsigned char *p)
{
    uint32_t value = u32(p);
    return value > INT32_MAX ? -1 - (int32_t)~value : (int32_t)value;
}

static int cpu_range(const void *p, size_t n)
{
    return p != NULL && n != 0 && n <= UINTPTR_MAX - (uintptr_t)p;
}

static int overlaps(const void *a, size_t an, const void *b, size_t bn)
{
    return (uintptr_t)a < (uintptr_t)b + bn && (uintptr_t)b < (uintptr_t)a + an;
}

static void name(char *out, const unsigned char *in)
{
    for (unsigned i = 0; i < 64; ++i) out[i] = (char)in[i];
    out[64] = 0;
}

static int payload(enum nbg_section_id table)
{
    switch (table) {
    case NBG_COEFF_HEADER_TABLE: return NBG_COEFF_HEADER;
    case NBG_COEFF_TABLE: return NBG_COEFF;
    case NBG_PPU_INSTR_TABLE: return NBG_PPU_INSTR;
    case NBG_DYNA_IO_TABLE: return NBG_DYNA_IO;
    case NBG_LOAD_STATES_TABLE: return NBG_LOAD_STATES;
    case NBG_ICDT: return NBG_ICD;
    default: return -1;
    }
}

static uint32_t stride(const struct nbg_model *m, unsigned id)
{
    switch (id) {
    case NBG_INPUT: case NBG_OUTPUT: return 116;
    case NBG_LAYER: case NBG_LAYER_PARAM: return 76;
    case NBG_OPERATION: return 32;
    case NBG_NN: return m->header.nn_bytes;
    case NBG_TP: return m->header.tp_bytes;
    case NBG_PATCH: return 40;
    case NBG_SW: return 68;
    case NBG_HW_INIT: return 12;
    case NBG_PPU_PARAM: return 48;
    case NBG_SH: return 8;
    default: return payload((enum nbg_section_id)id) >= 0 ? 8 : 1;
    }
}

static enum nbg_decode_result tensor(const unsigned char *p, struct nbg_tensor *out)
{
    static const unsigned bits[] = {32,16,8,8,16,16,8,16,32,32,64,64,64,4,4,0,8};
    struct nbg_tensor t = {0};
    t.dimensions = u32(p);
    t.format = u32(p + 28);
    t.buffer_type = u32(p + 32);
    t.quantization = u32(p + 36);
    if (t.dimensions < 1 || t.dimensions > 6) return NBG_DECODE_INVALID;
    if (t.format >= sizeof(bits) / sizeof(bits[0]) || !bits[t.format] ||
        t.buffer_type > 3 || t.quantization > 2) return NBG_DECODE_UNSUPPORTED;
    uint64_t elements = 1, limit = (uint64_t)UINT32_MAX * 8 / bits[t.format];
    for (unsigned i = 0; i < 6; ++i) {
        t.sizes[i] = u32(p + 4 + i * 4);
        if (i < t.dimensions) {
            if (!t.sizes[i] || elements > limit / t.sizes[i]) return NBG_DECODE_INVALID;
            elements *= t.sizes[i];
        }
    }
    t.logical_bytes = (uint32_t)((elements * bits[t.format] + 7) / 8);
    t.fixed_position = i32(p + 40);
    t.scale_bits = u32(p + 44);
    t.zero_point = i32(p + 48);
    if (t.quantization == 2 && (!t.scale_bits || (t.scale_bits >> 31) ||
                               (t.scale_bits & 0x7f800000) == 0x7f800000))
        return NBG_DECODE_INVALID;
    name(t.name, p + 52);
    *out = t;
    return NBG_DECODE_OK;
}

static int references(const struct nbg_model *m, uint32_t state, uint32_t first, uint32_t count)
{
    uint32_t patches = m->sections[NBG_PATCH].bytes / 40;
    return state < m->sections[NBG_LOAD_STATES_TABLE].bytes / 8 &&
           first <= patches && count <= patches - first;
}

enum nbg_decode_result nbg_model_open(const void *input, size_t bytes, struct nbg_model *result)
{
    if (bytes < NBG_MODEL_HEADER_BYTES || bytes > NBG_MODEL_MAX_BYTES ||
        !cpu_range(input, bytes) || !cpu_range(result, sizeof(*result)) ||
        overlaps(input, bytes, result, sizeof(*result))) return NBG_DECODE_INVALID;
    const unsigned char *data = input;
    if (data[0] != 'V' || data[1] != 'P' || data[2] != 'M' || data[3] != 'N')
        return NBG_DECODE_INVALID;
    if (u32(data + 4) != NBG_MODEL_VERSION) return NBG_DECODE_UNSUPPORTED;
    struct nbg_model m = {0};
    m.data = data; m.bytes = bytes;
    struct nbg_model_header *h = &m.header;
    h->version = u32(data + 4); h->target = u32(data + 8);
    name(h->name, data + 12);
    h->layer_count = u32(data + 76); h->operation_count = u32(data + 80);
    h->input_count = u32(data + 84); h->output_count = u32(data + 88);
    h->flags = u32(data + 92); h->core_count = data[100]; h->checksum = u32(data + 108);
    unsigned nn = (h->flags >> 2) & 3, tp = (h->flags >> 5) & 3;
    if (nn > 1 || tp > 1) return NBG_DECODE_UNSUPPORTED;
    h->nn_bytes = nn ? 192 : 128; h->tp_bytes = tp ? 192 : 128;
    h->pool_bytes = u32(data + 348); h->pool_alignment = u32(data + 352);
    h->pool_base = u32(data + 356); h->axi_sram_base = u32(data + 360);
    h->axi_sram_bytes = u32(data + 364); h->vip_sram_base = u32(data + 368);
    h->vip_sram_bytes = u32(data + 372);
    if (!h->pool_alignment || (h->pool_alignment & (h->pool_alignment - 1)))
        return NBG_DECODE_INVALID;
    for (unsigned i = 0; i < NBG_SECTION_COUNT; ++i) {
        struct nbg_section *s = &m.sections[i];
        s->offset = u32(data + 376 + i * 8); s->bytes = u32(data + 380 + i * 8);
        if (s->offset > bytes || s->bytes > bytes - s->offset ||
            (s->bytes && s->offset < NBG_MODEL_HEADER_BYTES) || s->bytes % stride(&m, i))
            return NBG_DECODE_INVALID;
        for (unsigned j = 0; j < i; ++j) {
            const struct nbg_section *other = &m.sections[j];
            if (s->bytes && other->bytes && s->offset < other->offset + other->bytes &&
                other->offset < s->offset + s->bytes) return NBG_DECODE_INVALID;
        }
    }
    if (m.sections[NBG_INPUT].bytes / 116 != h->input_count ||
        m.sections[NBG_OUTPUT].bytes / 116 != h->output_count ||
        m.sections[NBG_LAYER].bytes / 76 != h->layer_count ||
        m.sections[NBG_OPERATION].bytes / 32 != h->operation_count) return NBG_DECODE_INVALID;
    for (unsigned i = 0; i < NBG_SECTION_COUNT; ++i) {
        int target = payload((enum nbg_section_id)i);
        if (target < 0) continue;
        const struct nbg_section *s = &m.sections[i];
        for (uint32_t at = 0; at < s->bytes; at += 8) {
            const unsigned char *p = data + s->offset + at;
            uint32_t offset = u32(p), length = u32(p + 4), total = m.sections[target].bytes;
            if (offset > total || length > total - offset) return NBG_DECODE_INVALID;
        }
    }
    for (unsigned id = NBG_INPUT; id <= NBG_OUTPUT; ++id) {
        const struct nbg_section *s = &m.sections[id];
        for (uint32_t at = 0; at < s->bytes; at += 116) {
            struct nbg_tensor t;
            enum nbg_decode_result status = tensor(data + s->offset + at, &t);
            if (status != NBG_DECODE_OK) return status;
        }
    }
    const struct nbg_section *s = &m.sections[NBG_OPERATION];
    for (uint32_t at = 0; at < s->bytes; at += 32) {
        const unsigned char *p = data + s->offset + at;
        uint32_t kind = u32(p), index = u32(p + 4), layer = u32(p + 8);
        if (!references(&m, u32(p + 12), u32(p + 16), u32(p + 20)) ||
            (kind >= 1 && kind <= 8 && layer >= h->layer_count) ||
            (kind == 2 && index >= m.sections[NBG_NN].bytes / h->nn_bytes) ||
            (kind == 3 && index >= m.sections[NBG_TP].bytes / h->tp_bytes))
            return NBG_DECODE_INVALID;
    }
    s = &m.sections[NBG_HW_INIT];
    for (uint32_t at = 0; at < s->bytes; at += 12) {
        const unsigned char *p = data + s->offset + at;
        if (!references(&m, u32(p), u32(p + 4), u32(p + 8))) return NBG_DECODE_INVALID;
    }
    s = &m.sections[NBG_PATCH];
    for (uint32_t at = 0; at < s->bytes; at += 40) {
        const unsigned char *p = data + s->offset + at;
        uint32_t source = u32(p + 8), index = u32(p + 12);
        if ((source == 1 && index >= h->input_count) ||
            (source == 2 && index >= h->output_count)) return NBG_DECODE_INVALID;
    }
    *result = m;
    return NBG_DECODE_OK;
}

static int output_valid(const struct nbg_model *m, void *out, size_t bytes)
{
    return cpu_range(m, sizeof(*m)) && cpu_range(out, bytes) &&
        m->bytes >= NBG_MODEL_HEADER_BYTES && m->bytes <= NBG_MODEL_MAX_BYTES &&
        m->header.version == NBG_MODEL_VERSION && cpu_range(m->data, m->bytes) &&
        !overlaps(m, sizeof(*m), out, bytes) && !overlaps(m->data, m->bytes, out, bytes);
}

static const unsigned char *record(const struct nbg_model *m, unsigned id, uint32_t index,
                                    uint32_t width)
{
    if (id >= NBG_SECTION_COUNT || !width) return NULL;
    const struct nbg_section *s = &m->sections[id];
    if (s->offset > m->bytes || s->bytes > m->bytes - s->offset ||
        s->bytes % width || index >= s->bytes / width) return NULL;
    return m->data + s->offset + (size_t)index * width;
}

int nbg_model_section(const struct nbg_model *m, enum nbg_section_id id, struct nbg_span *out)
{
    if (!output_valid(m, out, sizeof(*out)) || (unsigned)id >= NBG_SECTION_COUNT) return 0;
    const struct nbg_section *s = &m->sections[id];
    if (s->offset > m->bytes || s->bytes > m->bytes - s->offset) return 0;
    *out = (struct nbg_span){m->data + s->offset, s->bytes};
    return 1;
}

int nbg_model_tensor(const struct nbg_model *m, enum nbg_section_id id,
                     uint32_t index, struct nbg_tensor *out)
{
    if (!output_valid(m, out, sizeof(*out)) || (id != NBG_INPUT && id != NBG_OUTPUT)) return 0;
    const unsigned char *p = record(m, (unsigned)id, index, 116);
    return p && tensor(p, out) == NBG_DECODE_OK;
}

int nbg_model_operation(const struct nbg_model *m, uint32_t index, struct nbg_operation *out)
{
    if (!output_valid(m, out, sizeof(*out))) return 0;
    const unsigned char *p = record(m, NBG_OPERATION, index, 32);
    if (!p) return 0;
    *out = (struct nbg_operation){u32(p), u32(p+4), u32(p+8), u32(p+12),
                                 u32(p+16), u32(p+20), {u32(p+24), u32(p+28)}};
    return 1;
}

int nbg_model_patch(const struct nbg_model *m, uint32_t index, struct nbg_patch *out)
{
    if (!output_valid(m, out, sizeof(*out))) return 0;
    const unsigned char *p = record(m, NBG_PATCH, index, 40);
    if (!p) return 0;
    *out = (struct nbg_patch){u32(p), u32(p+4), u32(p+8), i32(p+12), u32(p+16),
                             u32(p+20), u32(p+24), u32(p+28), u32(p+32), u32(p+36)};
    return 1;
}

int nbg_model_table(const struct nbg_model *m, enum nbg_section_id id,
                    uint32_t index, struct nbg_span *out)
{
    if (!output_valid(m, out, sizeof(*out))) return 0;
    int target = payload(id);
    if (target < 0) return 0;
    const unsigned char *p = record(m, (unsigned)id, index, 8);
    if (!p) return 0;
    const struct nbg_section *s = &m->sections[target];
    uint32_t offset = u32(p), bytes = u32(p + 4);
    if (s->offset > m->bytes || s->bytes > m->bytes - s->offset ||
        offset > s->bytes || bytes > s->bytes - offset) return 0;
    *out = (struct nbg_span){m->data + s->offset + offset, bytes};
    return 1;
}
