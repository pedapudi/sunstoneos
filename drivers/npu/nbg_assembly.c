#include "nbg_assembly.h"
#include "nbg_relocation.h"
#include "nbg_checksum.h"
#include "a733_npu_controller.h"
#include <string.h>

static const uint32_t prefix[] = {
    0x08010e02, 0x701, 0x48000000, 0x701, 0x0801022c, 0x1f,
    0x08010e03, 0xc23, 0x08010e02, 0x701, 0x48000000, 0x701
};
static const uint32_t footer[] = {
    0x08010e02, 0x30000701, 0x48000000, 0x30000701,
    0x08010e03, 0xc23, 0x08010e01, 0x40, 0x10000000, 0, 0, 0, 0, 0
};
static const uint32_t initial[] = {
    0x0801028a, 0x11, 0x08010e13, 2, 0x08010e4e, 0,
    0x08010e4f, 0, 0x08010e50, 0
};
static const uint32_t state[] = {
    0x08010e4c, 0x0491e040, 0x08010e5a, 0x00451440,
    0x08010428, 1, 0x08010429, 0
};

static uint32_t word(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put(unsigned char *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (i * 8));
}
static void words(unsigned char *p, const uint32_t *v, size_t n)
{
    for (size_t i = 0; i < n; ++i) put(p + i * 4, v[i]);
}
static int range(const void *p, size_t n)
{
    return p && n && n <= UINTPTR_MAX - (uintptr_t)p;
}
static int overlaps(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{
    return a < b + bn && b < a + an;
}
static int separate(const void *a, size_t an, const void *b, size_t bn)
{
    return !overlaps((uintptr_t)a, an, (uintptr_t)b, bn);
}
static int result_space(const struct nbg_model *m, struct nbg_assembly_plan *out)
{
    return range(m, sizeof(*m)) && range(out, sizeof(*out)) &&
           separate(out, sizeof(*out), m, sizeof(*m)) &&
           separate(out, sizeof(*out), m->data, m->bytes);
}
static int requirement(struct nbg_allocation_requirement *r, uint32_t payload,
                        uint64_t minimum)
{
    uint64_t allocation = (minimum + 4095) & ~UINT64_C(4095);
    if (!payload || !minimum || allocation > NBG_MODEL_MAX_BYTES) return 0;
    *r = (struct nbg_allocation_requirement){payload, (uint32_t)minimum,
                                            (uint32_t)allocation};
    return 1;
}
static int patch_fields(const struct nbg_patch *p)
{
    return p->transformed <= 1 && !p->name_word && !p->high_destination &&
           !p->high_mask && !p->destination_base;
}

/* Profile checks are intentionally narrower than structural container decoding.
 * The INIT bytes are recognized before being omitted, as in the SDK capture.
 * Their equivalence to a cold native bootstrap is still unqualified. */
static int profile(const struct nbg_model *m, struct nbg_assembly_plan *p)
{
    const struct nbg_model_header *h = &m->header;
    struct nbg_checksum checksum;
    if (nbg_checksum_verify(m->data, m->bytes, &checksum) != NBG_CHECKSUM_MATCH) return 0;
    if (h->version != NBG_MODEL_VERSION || h->target != 0x1000003b ||
        h->flags != 0x02d00027 || h->core_count != 1 || h->nn_bytes != 192 ||
        h->input_count != 1 || h->output_count != 1 ||
        h->operation_count < 2 || h->operation_count > NBG_ASSEMBLY_MAX_NN + 1 ||
        h->pool_alignment != 64 || h->pool_base || h->axi_sram_base ||
        h->axi_sram_bytes || h->vip_sram_base || !h->vip_sram_bytes ||
        h->vip_sram_bytes > 0x80000) return 0;
    for (unsigned i = 0; i < NBG_SECTION_COUNT; ++i) {
        switch (i) {
        case NBG_INPUT: case NBG_OUTPUT: case NBG_LAYER: case NBG_OPERATION:
        case NBG_NN: case NBG_PATCH: case NBG_LOAD_STATES:
        case NBG_COEFF_TABLE: case NBG_COEFF: case NBG_LOAD_STATES_TABLE: break;
        default: if (m->sections[i].bytes) return 0;
        }
    }
    memset(p, 0, sizeof(*p));
    p->nn_operations = h->operation_count - 1;
    if (m->sections[NBG_NN].bytes != p->nn_operations * 192 ||
        m->sections[NBG_LOAD_STATES_TABLE].bytes != h->operation_count * 8) return 0;
    p->command_body_bytes = (uint32_t)sizeof(prefix) + p->nn_operations * 24 + 8;
    uint32_t command_bytes = p->command_body_bytes + (uint32_t)sizeof(footer);
    uint64_t fetch = A733_NPU_FETCH_BYTES;
    if (fetch < command_bytes) fetch = command_bytes;
    if (!requirement(&p->buffers[NBG_ALLOC_NN], m->sections[NBG_NN].bytes,
                     m->sections[NBG_NN].bytes) ||
        !requirement(&p->buffers[NBG_ALLOC_COEFF], m->sections[NBG_COEFF].bytes,
                     (uint64_t)m->sections[NBG_COEFF].bytes + 128) ||
        !requirement(&p->buffers[NBG_ALLOC_POOL], h->pool_bytes, h->pool_bytes) ||
        !requirement(&p->buffers[NBG_ALLOC_COMMAND], command_bytes, fetch)) return 0;
    for (unsigned i = 0; i < 2; ++i) {
        struct nbg_tensor t;
        if (!nbg_model_tensor(m, i ? NBG_OUTPUT : NBG_INPUT, 0, &t) ||
            t.format != 2 || t.buffer_type != 0 || t.quantization != 2 ||
            !requirement(&p->buffers[NBG_ALLOC_INPUT + i], t.logical_bytes,
                         (((uint64_t)t.logical_bytes + 63) & ~UINT64_C(63)) + 64)) return 0;
    }
    uint64_t total = 0;
    for (unsigned i = 0; i < NBG_ALLOCATION_COUNT; ++i) total += p->buffers[i].allocation_bytes;
    if (total > NBG_MODEL_MAX_BYTES) return 0;

    struct nbg_operation op;
    struct nbg_span span;
    struct nbg_patch patch;
    if (!nbg_model_operation(m, 0, &op) || op.kind != 0xffff || op.index != 0xffff ||
        op.layer != 0xffff || op.state || op.first_patch || op.patch_count != 1 ||
        op.auxiliary[0] || op.auxiliary[1] != UINT32_MAX ||
        !nbg_model_table(m, NBG_LOAD_STATES_TABLE, 0, &span) ||
        span.bytes != sizeof(initial)) return 0;
    for (unsigned i = 0; i < sizeof(initial) / 4; ++i)
        if (word(span.data + i * 4) != initial[i]) return 0;
    if (!nbg_model_patch(m, 0, &patch) || !patch_fields(&patch) || patch.kind != 1 ||
        patch.destination != 20 || patch.source != 8 || patch.index != -1 ||
        patch.source_offset || patch.transformed) return 0;
    return 1;
}

/* Each scratch descriptor is private. A later operation's refusal therefore
 * cannot leave an earlier caller-owned descriptor partially assembled. */
struct operation_image { unsigned char nn[192], command[32]; uint32_t command_bytes; };
static int operation(const struct nbg_model *m, uint32_t number, uint32_t first,
                     const struct nbg_assembly_buffer *buffers,
                     const struct nbg_assembly_plan *plan, struct operation_image *image,
                     uint32_t *count)
{
    struct nbg_operation op;
    struct nbg_span span;
    if (!nbg_model_operation(m, number, &op) || op.kind != 2 || op.index != number - 1 ||
        op.state != number || op.first_patch != first || !op.patch_count ||
        op.patch_count > 49 || op.auxiliary[0] ||
        !nbg_model_table(m, NBG_LOAD_STATES_TABLE, op.state, &span)) return 0;
    int last = number == plan->nn_operations;
    image->command_bytes = last ? 32 : 24;
    if (span.bytes != image->command_bytes) return 0;
    for (unsigned i = 0; i < span.bytes / 4; ++i)
        if (word(span.data + i * 4) != (i == 5 && last ? 0 : state[i])) return 0;
    memcpy(image->command, span.data, span.bytes);
    memcpy(image->nn, m->data + m->sections[NBG_NN].offset + op.index * 192, 192);
    struct nbg_relocation nn[48], command = {0};
    uint32_t nn_count = 0, command_count = 0;
    for (uint32_t i = 0; i < op.patch_count; ++i) {
        struct nbg_patch p;
        if (!nbg_model_patch(m, first + i, &p) || !patch_fields(&p)) return 0;
        struct nbg_relocation r = {0};
        if (p.kind == 0) {
            if (command_count || p.source || p.index || p.source_offset ||
                p.destination != 20 || p.transformed != 1) return 0;
            command = (struct nbg_relocation){plan->buffers[NBG_ALLOC_NN].payload_bytes,
                192, buffers ? buffers[NBG_ALLOC_NN].device : 0x1000,
                op.index * 192, 20, NBG_ADDRESS_FLAGS6};
            ++command_count;
        } else {
            if (p.kind != 1 || nn_count == 48) return 0;
            unsigned id;
            switch (p.source) {
            case 1: case 2:
                if (p.index) return 0;
                id = p.source == 1 ? NBG_ALLOC_INPUT : NBG_ALLOC_OUTPUT;
                r.source_device = buffers ? buffers[id].device : 0x1000;
                r.source_bytes = plan->buffers[id].minimum_bytes;
                break;
            case 3:
                if (p.index != -1) return 0;
                r.source_device = buffers ? buffers[NBG_ALLOC_POOL].device : 0x1000;
                r.source_bytes = m->header.pool_bytes;
                break;
            case 5: {
                if (p.index < 0 || !nbg_model_table(m, NBG_COEFF_TABLE, (uint32_t)p.index, &span)) return 0;
                uint64_t offset = (uint64_t)(span.data - (m->data + m->sections[NBG_COEFF].offset));
                uint64_t base = buffers ? buffers[NBG_ALLOC_COEFF].device : 0x1000;
                if (base + offset > UINT32_MAX) return 0;
                r.source_device = (uint32_t)(base + offset);
                r.source_bytes = span.bytes;
                break;
            }
            case 8:
                if (p.index != -1) return 0;
                r.source_device = 0x400000;
                r.source_bytes = m->header.vip_sram_bytes;
                break;
            default: return 0;
            }
            /* Address resolution only. Descriptor access lengths are unresolved. */
            r.referenced_bytes = 1;
            r.source_offset = p.source_offset;
            r.destination_offset = p.destination;
            r.encoding = p.transformed ? NBG_ADDRESS_SHIFT6 : NBG_ADDRESS_WORD;
            nn[nn_count++] = r;
        }
    }
    if (command_count != 1 || !nbg_relocate(image->command, image->command_bytes, &command, 1) ||
        (nn_count && !nbg_relocate(image->nn, sizeof(image->nn), nn, nn_count))) return 0;
    *count = op.patch_count;
    return 1;
}

static int inspect(const struct nbg_model *m, const struct nbg_assembly_buffer *b,
                   struct nbg_assembly_plan *p)
{
    uint32_t first = 1;
    for (uint32_t i = 1; i <= p->nn_operations; ++i) {
        struct operation_image scratch;
        uint32_t count;
        if (!operation(m, i, first, b, p, &scratch, &count)) return 0;
        first += count;
    }
    if (first != m->sections[NBG_PATCH].bytes / 40) return 0;
    p->state_patches = p->nn_operations;
    p->descriptor_patches = first - 1 - p->state_patches;
    return 1;
}

int nbg_assembly_requirements(const struct nbg_model *m, struct nbg_assembly_plan *out)
{
    struct nbg_assembly_plan p;
    if (!result_space(m, out) || !profile(m, &p) || !inspect(m, NULL, &p)) return 0;
    *out = p;
    return 1;
}

int nbg_assemble(const struct nbg_model *m, const struct nbg_assembly_buffer *b,
                 struct nbg_assembly_plan *out)
{
    struct nbg_assembly_plan p;
    const size_t metadata_bytes = NBG_ALLOCATION_COUNT * sizeof(*b);
    if (!result_space(m, out) || !range(b, metadata_bytes) ||
        !separate(out, sizeof(*out), b, metadata_bytes) || !profile(m, &p)) return 0;
    for (unsigned i = 0; i < NBG_ALLOCATION_COUNT; ++i) {
        if (b[i].bytes != p.buffers[i].allocation_bytes || !range(b[i].cpu, b[i].bytes) ||
            ((uintptr_t)b[i].cpu & 4095) || !b[i].device || (b[i].device & 4095) ||
            b[i].bytes > (UINT64_C(1) << 32) - b[i].device ||
            overlaps(b[i].device, b[i].bytes, 0x400000, 0x80000) ||
            !separate(b[i].cpu, b[i].bytes, out, sizeof(*out)) ||
            !separate(b[i].cpu, b[i].bytes, b, metadata_bytes) ||
            !separate(b[i].cpu, b[i].bytes, m, sizeof(*m)) ||
            !separate(b[i].cpu, b[i].bytes, m->data, m->bytes)) return 0;
        for (unsigned j = 0; j < i; ++j)
            if (!separate(b[i].cpu, b[i].bytes, b[j].cpu, b[j].bytes) ||
                overlaps(b[i].device, b[i].bytes, b[j].device, b[j].bytes)) return 0;
    }
    if (!inspect(m, b, &p)) return 0;

    /* All caller-visible failure paths precede mutation. Immutable input and
     * metadata make the repeated operation construction infallible here. */
    for (unsigned i = 0; i < NBG_ALLOCATION_COUNT; ++i) memset(b[i].cpu, 0, b[i].bytes);
    memcpy(b[NBG_ALLOC_COEFF].cpu, m->data + m->sections[NBG_COEFF].offset,
           p.buffers[NBG_ALLOC_COEFF].payload_bytes);
    unsigned char *commands = b[NBG_ALLOC_COMMAND].cpu;
    for (size_t i = 0; i < b[NBG_ALLOC_COMMAND].bytes; i += 8) put(commands + i, 0x10000000);
    words(commands, prefix, sizeof(prefix) / 4);
    uint32_t first = 1, offset = (uint32_t)sizeof(prefix);
    for (uint32_t i = 1; i <= p.nn_operations; ++i) {
        struct operation_image scratch;
        uint32_t count = 0;
        (void)operation(m, i, first, b, &p, &scratch, &count);
        memcpy((unsigned char *)b[NBG_ALLOC_NN].cpu + (i - 1) * 192, scratch.nn, 192);
        memcpy(commands + offset, scratch.command, scratch.command_bytes);
        first += count;
        offset += scratch.command_bytes;
    }
    words(commands + offset, footer, sizeof(footer) / 4);
    *out = p;
    return 1;
}
