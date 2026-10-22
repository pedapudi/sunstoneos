#include "nbg_assembly.h"
#include "nbg_relocation.h"
#include "nbg_checksum.h"
#include "a733_npu_controller.h"
#include "nbg_command.h"
#include "a733_range.h"
#include <string.h>

static const uint32_t state[] = {
    0x08010e4c, 0x0491e040, 0x08010e5a, 0x00451440,
    0x08010428, 1, 0x08010429, 0
};

static int separate(const void *a, size_t an, const void *b, size_t bn)
{
    return a733_extents_disjoint_prechecked((uintptr_t)a, an, (uintptr_t)b, bn);
}
static int result_space(const struct nbg_model *m, const void *out, size_t bytes)
{
    return a733_span_excl_nonempty(m, sizeof(*m)) && a733_span_excl_nonempty(out, bytes) &&
           separate(out, bytes, m, sizeof(*m)) &&
           separate(out, bytes, m->data, m->bytes);
}
static uint32_t buffer_count(const struct nbg_assembly_layout *p)
{
    return NBG_ALLOC_INPUT + p->inputs + p->outputs;
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
static int profile(const struct nbg_model *m, struct nbg_assembly_layout *p, int checked)
{
    const struct nbg_model_header *h = &m->header;
    int directory = h->version == NBG_MODEL_DIRECTORY_VERSION;
    struct nbg_checksum checksum;
    if (checked && nbg_checksum_verify(m->data, m->bytes, &checksum) != NBG_CHECKSUM_MATCH) return 0;
    if ((!directory && h->version != NBG_MODEL_VERSION) || h->target != 0x1000003b ||
        h->flags != (directory ? 0x0aU : 0x02d00027U) || h->core_count != 1 || h->nn_bytes != 192 ||
        !h->input_count || h->input_count > NBG_ASSEMBLY_MAX_TENSORS ||
        !h->output_count || h->output_count > NBG_ASSEMBLY_MAX_TENSORS ||
        h->operation_count < 2 || h->operation_count > NBG_ASSEMBLY_MAX_NN + 1 ||
        h->pool_alignment != 64 || h->pool_base || h->axi_sram_base ||
        h->axi_sram_bytes || h->vip_sram_base || !h->vip_sram_bytes ||
        h->vip_sram_bytes > 0x80000) return 0;
    for (unsigned i = 0; i < NBG_SECTION_COUNT; ++i) {
        switch (i) {
        case NBG_INPUT: case NBG_OUTPUT: case NBG_LAYER: case NBG_OPERATION:
        case NBG_NN: case NBG_PATCH: case NBG_LOAD_STATES:
        case NBG_COEFF_TABLE: case NBG_COEFF: case NBG_LOAD_STATES_TABLE: break;
        case NBG_NETWORK_INFO: case NBG_MEMORY_POOL: case NBG_VIP_SRAM:
        case NBG_AXI_SRAM: case NBG_SECTION_CHECKSUM:
            if (!directory && m->sections[i].bytes) return 0;
            break;
        default: if (m->sections[i].bytes) return 0;
        }
    }
    memset(p, 0, sizeof(*p));
    p->inputs = h->input_count; p->outputs = h->output_count;
    p->nn_operations = h->operation_count - 1;
    if (m->sections[NBG_NN].bytes != p->nn_operations * 192 ||
        m->sections[NBG_LOAD_STATES_TABLE].bytes != h->operation_count * 8) return 0;
    /* The body is the prefix plus every operation's state; operation()
     * later admits only the 24- and 32-byte state forms. */
    uint64_t body = sizeof(prefix);
    for (uint32_t i = 1; i <= p->nn_operations; ++i) {
        struct nbg_span state_span;
        if (!nbg_model_table(m, NBG_LOAD_STATES_TABLE, i, &state_span) || state_span.bytes > 32)
            return 0;
        body += state_span.bytes;
    }
    p->command_body_bytes = (uint32_t)body;
    uint32_t command_bytes = p->command_body_bytes + (uint32_t)sizeof(footer);
    uint64_t fetch = A733_NPU_FETCH_BYTES;
    if (fetch < command_bytes) fetch = command_bytes;
    if (!requirement(&p->buffers[NBG_ALLOC_NN], m->sections[NBG_NN].bytes,
                     m->sections[NBG_NN].bytes) ||
        !requirement(&p->buffers[NBG_ALLOC_COEFF], m->sections[NBG_COEFF].bytes,
                     (uint64_t)m->sections[NBG_COEFF].bytes + 128) ||
        (h->pool_bytes && !requirement(&p->buffers[NBG_ALLOC_POOL], h->pool_bytes, h->pool_bytes)) ||
        !requirement(&p->buffers[NBG_ALLOC_COMMAND], command_bytes, fetch)) return 0;
    /* Affine UINT8, INT8 and INT16 tensors, in model order: inputs, then outputs. */
    for (uint32_t i = 0; i < p->inputs + p->outputs; ++i) {
        struct nbg_tensor t;
        int output = i >= p->inputs;
        if (!nbg_model_tensor(m, output ? NBG_OUTPUT : NBG_INPUT, output ? i - p->inputs : i, &t) ||
            (t.format != 2 && t.format != 3 && t.format != 5) || t.buffer_type != 0 ||
            t.quantization != 2 ||
            !requirement(&p->buffers[NBG_ALLOC_INPUT + i], t.logical_bytes,
                         (((uint64_t)t.logical_bytes + 63) & ~UINT64_C(63)) + 64)) return 0;
    }
    uint64_t total = 0;
    for (unsigned i = 0; i < buffer_count(p); ++i) total += p->buffers[i].allocation_bytes;
    if (total > NBG_MODEL_MAX_BYTES) return 0;

    struct nbg_operation op;
    struct nbg_span span;
    struct nbg_patch patch;
    if (!nbg_model_operation(m, 0, &op) || op.kind != 0xffff || op.index != 0xffff ||
        op.layer != (directory ? UINT32_MAX : 0xffffU) || op.state || op.first_patch || op.patch_count != 1 ||
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

/* Slots identify the observed pointer fields, not arbitrary patch destinations.
 * Circular sizes and ends are in 64-byte units in both the Mesa NN layout and
 * the pinned HAL transpose encoder. Checking the encoded ring does not prove
 * that all device accesses (including prefetch) remain within it. */
enum pointer_slot { WEIGHTS, INPUT, OUTPUT, OUTPUT_END, INPUT_END, POINTER_COUNT };
static int ring(const unsigned char *descriptor, struct nbg_relocation *nn,
                const uint32_t *slot, const uint32_t *source, unsigned tensor,
                unsigned end_slot, unsigned size_word)
{
    if (slot[tensor] == UINT32_MAX) return 0;
    uint64_t size = (uint64_t)(word(descriptor + size_word * 4) & 0x03ffffff) << 6;
    if (!size)
        return slot[end_slot] == UINT32_MAX &&
            (word(descriptor + (size_word + 1) * 4) & 0x03ffffff) == 0x03ffffff;
    if (slot[end_slot] == UINT32_MAX) return 0;
    struct nbg_relocation *address = &nn[slot[tensor]], *end = &nn[slot[end_slot]];
    uint64_t limit = end->source_offset;
    if (source[slot[tensor]] != source[slot[end_slot]] ||
        address->source_device != end->source_device || address->source_bytes != end->source_bytes ||
        size > limit || limit > end->source_bytes ||
        address->source_offset < limit - size || address->source_offset >= limit) return 0;
    end->source_offset = (uint32_t)(limit - size);
    end->referenced_bytes = size;
    end->encoding = NBG_ADDRESS_END_SHIFT6;
    return 1;
}

static int operation(const struct nbg_model *m, uint32_t number, uint32_t first,
                     const struct nbg_assembly_buffer *buffers,
                     const struct nbg_assembly_layout *plan, struct operation_image *image,
                     uint32_t *count)
{
    struct nbg_operation op;
    struct nbg_span span;
    int directory = m->header.version == NBG_MODEL_DIRECTORY_VERSION;
    /* Kind 2 is a neural-network operation; kind 8, a neural-transpose step
     * (observed only in the directory format), uses the same 192-byte
     * descriptor and pointer fields without coefficients. */
    if (!nbg_model_operation(m, number, &op) || (op.kind != 2 && !(op.kind == 8 && directory)) ||
        op.index != number - 1 ||
        op.state != number || op.first_patch != first || !op.patch_count ||
        op.patch_count > POINTER_COUNT + 1 || op.auxiliary[0] ||
        !nbg_model_table(m, NBG_LOAD_STATES_TABLE, op.state, &span)) return 0;
    int last = number == plan->nn_operations;
    /* The 24-byte state sets word 5 and runs straight into the next
     * operation. The 32-byte state clears it and appends a second pair; the
     * compiler emits it for the last operation and, in the directory format,
     * for every operation of graphs that contain transpose steps. */
    if (span.bytes != 32 && (last || span.bytes != 24)) return 0;
    if (span.bytes == 32 && !last && !directory) return 0;
    image->command_bytes = (uint32_t)span.bytes;
    for (unsigned i = 0; i < span.bytes / 4; ++i) {
        uint32_t expected = i == 5 && span.bytes == 32 ? 0 : state[i];
        // The observed directory profile changes bit 17 of this state value.
        // Its hardware meaning remains unresolved; require the captured bytes.
        if (i == 1 && m->header.version == NBG_MODEL_DIRECTORY_VERSION) expected = 0x0493e040;
        if (word(span.data + i * 4) != expected) return 0;
    }
    memcpy(image->command, span.data, span.bytes);
    memcpy(image->nn, m->data + m->sections[NBG_NN].offset + op.index * 192, 192);
    struct nbg_relocation nn[POINTER_COUNT], command = {0};
    uint32_t slot[POINTER_COUNT], source[POINTER_COUNT];
    for (unsigned i = 0; i < POINTER_COUNT; ++i) slot[i] = UINT32_MAX;
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
            if (p.kind != 1 || nn_count == POINTER_COUNT) return 0;
            unsigned field;
            switch (p.destination) {
            case 20:
                if (op.kind != 2 || p.source != 5 || p.transformed != 1) return 0;
                field = WEIGHTS; break;
            case 24:
                if ((p.source != 1 && p.source != 3 && p.source != 8) || p.transformed) return 0;
                field = INPUT; break;
            case 28:
                if ((p.source != 2 && p.source != 3 && p.source != 8) || p.transformed) return 0;
                field = OUTPUT; break;
            case 76: case 84:
                if ((p.source != 1 && p.source != 2 && p.source != 3 && p.source != 8) ||
                    p.transformed != 1) return 0;
                field = p.destination == 76 ? OUTPUT_END : INPUT_END; break;
            default: return 0;
            }
            if (slot[field] != UINT32_MAX) return 0;
            slot[field] = nn_count;
            source[nn_count] = p.source;
            unsigned id;
            switch (p.source) {
            case 1: case 2:
                if (p.index < 0 || (uint32_t)p.index >= (p.source == 1 ? plan->inputs : plan->outputs))
                    return 0;
                id = NBG_ALLOC_INPUT + (uint32_t)p.index + (p.source == 1 ? 0 : plan->inputs);
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
            /* Rings are expanded below. Linear/weight access extents are unresolved. */
            r.referenced_bytes = 1;
            r.source_offset = p.source_offset;
            r.destination_offset = p.destination;
            r.encoding = p.transformed ? NBG_ADDRESS_SHIFT6 : NBG_ADDRESS_WORD;
            nn[nn_count++] = r;
        }
    }
    if (!ring(image->nn, nn, slot, source, INPUT, INPUT_END, 20) ||
        !ring(image->nn, nn, slot, source, OUTPUT, OUTPUT_END, 18) ||
        command_count != 1 || !nbg_relocate(image->command, image->command_bytes, &command, 1) ||
        (nn_count && !nbg_relocate(image->nn, sizeof(image->nn), nn, nn_count))) return 0;
    *count = op.patch_count;
    return 1;
}

static int inspect(const struct nbg_model *m, const struct nbg_assembly_buffer *b,
                   struct nbg_assembly_layout *p)
{
    uint32_t first = 1;
    for (uint32_t i = 1; i <= p->nn_operations; ++i) {
        struct operation_image scratch;
        uint32_t count;
        if (!operation(m, i, first, b, p, &scratch, &count)) return 0;
        first += count;
    }
    unsigned patch_bytes = m->header.version == NBG_MODEL_DIRECTORY_VERSION ? 28 : 40;
    if (first != m->sections[NBG_PATCH].bytes / patch_bytes) return 0;
    p->state_patches = p->nn_operations;
    p->descriptor_patches = first - 1 - p->state_patches;
    return 1;
}

int nbg_assembly_layout(const struct nbg_model *m, struct nbg_assembly_layout *out)
{
    struct nbg_assembly_layout p;
    if (!result_space(m, out, sizeof(*out)) || !profile(m, &p, 1) || !inspect(m, NULL, &p)) return 0;
    *out = p;
    return 1;
}

/* Placement rules shared by the one-call and stepwise assemblers. */
static int placed(const struct nbg_model *m, const struct nbg_assembly_buffer *b, size_t count,
                  const struct nbg_assembly_layout *p, const void *out, size_t out_bytes)
{
    const size_t metadata_bytes = count * sizeof(*b);
    for (unsigned i = 0; i < count; ++i) {
        if (!p->buffers[i].allocation_bytes) {
            if (b[i].cpu || b[i].bytes || b[i].device) return 0;
            continue;
        }
        if (b[i].bytes != p->buffers[i].allocation_bytes || !a733_span_excl_nonempty(b[i].cpu, b[i].bytes) ||
            ((uintptr_t)b[i].cpu & 4095) || !b[i].device || (b[i].device & 4095) ||
            b[i].bytes > (UINT64_C(1) << 32) - b[i].device ||
            a733_extents_overlap_prechecked(b[i].device, b[i].bytes, 0x400000, 0x80000) ||
            !separate(b[i].cpu, b[i].bytes, out, out_bytes) ||
            !separate(b[i].cpu, b[i].bytes, b, metadata_bytes) ||
            !separate(b[i].cpu, b[i].bytes, m, sizeof(*m)) ||
            !separate(b[i].cpu, b[i].bytes, m->data, m->bytes)) return 0;
        for (unsigned j = 0; j < i; ++j)
            if (b[j].bytes && (!separate(b[i].cpu, b[i].bytes, b[j].cpu, b[j].bytes) ||
                a733_extents_overlap_prechecked(b[i].device, b[i].bytes, b[j].device, b[j].bytes))) return 0;
    }
    return 1;
}

int nbg_assemble_layout(const struct nbg_model *m, const struct nbg_assembly_buffer *b,
                        size_t count, struct nbg_assembly_layout *out)
{
    struct nbg_assembly_layout p;
    const size_t metadata_bytes = count * sizeof(*b);
    if (!result_space(m, out, sizeof(*out)) || count > NBG_LAYOUT_MAX_BUFFERS ||
        !a733_span_excl_nonempty(b, metadata_bytes) || !separate(out, sizeof(*out), b, metadata_bytes) ||
        !profile(m, &p, 1) || count != buffer_count(&p) || !placed(m, b, count, &p, out, sizeof(*out)) ||
        !inspect(m, b, &p)) return 0;

    /* All caller-visible failure paths precede mutation. Immutable input and
     * metadata make the repeated operation construction infallible here. */
    for (unsigned i = 0; i < count; ++i)
        if (b[i].bytes) memset(b[i].cpu, 0, b[i].bytes);
    memcpy(b[NBG_ALLOC_COEFF].cpu, m->data + m->sections[NBG_COEFF].offset,
           p.buffers[NBG_ALLOC_COEFF].payload_bytes);
    unsigned char *commands = b[NBG_ALLOC_COMMAND].cpu;
    for (size_t i = 0; i < b[NBG_ALLOC_COMMAND].bytes; i += 8) put(commands + i, 0x10000000);
    words(commands, prefix, sizeof(prefix) / 4);
    uint32_t first = 1, offset = (uint32_t)sizeof(prefix);
    for (uint32_t i = 1; i <= p.nn_operations; ++i) {
        struct operation_image scratch;
        uint32_t used = 0;
        (void)operation(m, i, first, b, &p, &scratch, &used);
        memcpy((unsigned char *)b[NBG_ALLOC_NN].cpu + (i - 1) * 192, scratch.nn, 192);
        memcpy(commands + offset, scratch.command, scratch.command_bytes);
        first += used;
        offset += scratch.command_bytes;
    }
    words(commands + offset, footer, sizeof(footer) / 4);
    *out = p;
    return 1;
}

int nbg_assembly_begin(const struct nbg_model *m, const struct nbg_assembly_buffer *b, size_t count,
                       struct nbg_assembly_layout *out, struct nbg_assembly_cursor *cursor)
{
    struct nbg_assembly_layout p;
    const size_t metadata_bytes = count * sizeof(*b);
    if (!result_space(m, out, sizeof(*out)) || !result_space(m, cursor, sizeof(*cursor)) ||
        count > NBG_LAYOUT_MAX_BUFFERS || !a733_span_excl_nonempty(b, metadata_bytes) ||
        !separate(out, sizeof(*out), b, metadata_bytes) || !separate(cursor, sizeof(*cursor), b, metadata_bytes) ||
        !separate(out, sizeof(*out), cursor, sizeof(*cursor)) ||
        !profile(m, &p, 0) || count != buffer_count(&p) || !placed(m, b, count, &p, out, sizeof(*out)) ||
        !placed(m, b, count, &p, cursor, sizeof(*cursor))) return 0;
    *out = p;
    *cursor = (struct nbg_assembly_cursor){1, 1, (uint32_t)sizeof(prefix)};
    return 1;
}

int nbg_assembly_next(const struct nbg_model *m, const struct nbg_assembly_buffer *b,
                      struct nbg_assembly_layout *p, struct nbg_assembly_cursor *cursor,
                      unsigned operations)
{
    if (!cursor->operation || cursor->operation > p->nn_operations + 1 || !operations) return -1;
    unsigned char *commands = b[NBG_ALLOC_COMMAND].cpu;
    if (cursor->operation == 1 && cursor->offset == sizeof(prefix))
        words(commands, prefix, sizeof(prefix) / 4);
    for (; operations && cursor->operation <= p->nn_operations; --operations) {
        struct operation_image scratch;
        uint32_t used = 0;
        if (!operation(m, cursor->operation, cursor->patch, b, p, &scratch, &used) ||
            cursor->offset + scratch.command_bytes > p->command_body_bytes) return -1;
        memcpy((unsigned char *)b[NBG_ALLOC_NN].cpu + (cursor->operation - 1) * 192, scratch.nn, 192);
        memcpy(commands + cursor->offset, scratch.command, scratch.command_bytes);
        cursor->patch += used; cursor->offset += scratch.command_bytes; ++cursor->operation;
    }
    if (cursor->operation <= p->nn_operations) return 0;
    if (cursor->offset == p->command_body_bytes + sizeof(footer)) return 1;
    unsigned patch_bytes = m->header.version == NBG_MODEL_DIRECTORY_VERSION ? 28 : 40;
    if (cursor->offset != p->command_body_bytes ||
        cursor->patch != m->sections[NBG_PATCH].bytes / patch_bytes) return -1;
    words(commands + cursor->offset, footer, sizeof(footer) / 4);
    cursor->offset += (uint32_t)sizeof(footer);
    p->state_patches = p->nn_operations;
    p->descriptor_patches = cursor->patch - 1 - p->state_patches;
    return 1;
}

/* The six-slot interface keeps its single UINT8 input/output contract. */
static int single(const struct nbg_model *m, const struct nbg_assembly_layout *p)
{
    struct nbg_tensor input, output;
    return p->inputs == 1 && p->outputs == 1 &&
        nbg_model_tensor(m, NBG_INPUT, 0, &input) && input.format == 2 &&
        nbg_model_tensor(m, NBG_OUTPUT, 0, &output) && output.format == 2;
}
static void narrow(const struct nbg_assembly_layout *l, struct nbg_assembly_plan *p)
{
    memcpy(p->buffers, l->buffers, sizeof(p->buffers));
    p->nn_operations = l->nn_operations; p->descriptor_patches = l->descriptor_patches;
    p->state_patches = l->state_patches; p->command_body_bytes = l->command_body_bytes;
}

int nbg_assembly_requirements(const struct nbg_model *m, struct nbg_assembly_plan *out)
{
    struct nbg_assembly_layout l;
    if (!result_space(m, out, sizeof(*out)) || !nbg_assembly_layout(m, &l) || !single(m, &l))
        return 0;
    narrow(&l, out);
    return 1;
}

int nbg_assemble(const struct nbg_model *m, const struct nbg_assembly_buffer *b,
                 struct nbg_assembly_plan *out)
{
    struct nbg_assembly_layout l;
    if (!result_space(m, out, sizeof(*out)) ||
        !a733_span_excl_nonempty(b, NBG_ALLOCATION_COUNT * sizeof(*b)) ||
        !separate(out, sizeof(*out), b, NBG_ALLOCATION_COUNT * sizeof(*b)) ||
        !nbg_assembly_layout(m, &l) || !single(m, &l)) return 0;
    for (unsigned i = 0; i < NBG_ALLOCATION_COUNT; ++i)
        if (b[i].bytes && !separate(b[i].cpu, b[i].bytes, out, sizeof(*out))) return 0;
    if (!nbg_assemble_layout(m, b, NBG_ALLOCATION_COUNT, &l)) return 0;
    narrow(&l, out);
    return 1;
}
