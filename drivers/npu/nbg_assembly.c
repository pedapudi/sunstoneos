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
    return nbg_layout_buffers(p);
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
/* A stream-processor state is 0x08010e5b and its operand, 1..16 pairs of 0x08010e5c and 0x08010e5d
 * words, then the 32-byte tail of the other kinds (see operation()). */
#define MAX_STATE_BYTES (8u + 16u * 16u + 32u)
/* A shader state stream has no length limit of its own (loader rule 7): it must fit in one fetch segment with its
 * link (see segment_place()). The loader keeps no copy of it and no table of its packets: it reads the stream where
 * it lies in the model and writes the patched copy straight into the command allocation, so a stream costs no stack. */
/* The command fetch is a 16-bit count of 8-byte units (A733_NPU_FETCH_BYTES, 524,280 bytes). A command stream longer
 * than one fetch is cut between operations into segments. A segment ends with a LINK command, two words: opcode 8 in
 * bits 31:27 and the size of the next fetch in 8-byte units in the low bits, then the device address of the next
 * segment (the vendor's cmd_link, vip_drv_hardware.c; the vendor's linker libNBGlinker.so inserts the same command to
 * split a big command buffer, "nbglk_insert_link"). The next segment starts on a 64-byte boundary of the same
 * allocation; the bytes between the link and it are END words. */
/* The reason of the last refusal that has one, for a message (advisory: it is not synchronized, and a caller that runs
 * admissions concurrently may read another's). */
static uint32_t refusal_reason, refusal_operation;
static void refused(uint32_t reason, uint32_t operation) { refusal_reason = reason; refusal_operation = operation; }
uint32_t nbg_assembly_refusal(uint32_t *operation)
{
    if (operation) *operation = refusal_operation;
    return refusal_reason;
}
#define LINK_BYTES 8u
#define SEGMENT_ALIGN 64u
/* The longest segment, link included. A test build lowers it to split small graphs (checks/nbg-command-link.py). */
#ifndef NBG_SEGMENT_BYTES
#define NBG_SEGMENT_BYTES A733_NPU_FETCH_BYTES
#endif
/* The offset at which an item of this many bytes goes, given the stream so far ends at offset and its current
 * segment began at *segment. When the item and a link after it would not fit in the segment, a link goes at offset and
 * the item starts the next segment (the caller writes the link). UINT64_MAX: the item fits in no segment. */
static uint64_t segment_place(uint64_t offset, uint64_t *segment, uint64_t bytes)
{
    if (bytes + LINK_BYTES > NBG_SEGMENT_BYTES - SEGMENT_ALIGN) return UINT64_MAX;
    if (offset - *segment + bytes + LINK_BYTES > NBG_SEGMENT_BYTES) {
        offset = (offset + LINK_BYTES + SEGMENT_ALIGN - 1) & ~(uint64_t)(SEGMENT_ALIGN - 1);
        *segment = offset;
    }
    return offset;
}
/* Write the link at offset from to the segment at offset to, and END words between. The fetch of the next segment is
 * the rest of the allocation, at most one fetch. */
static void write_link(unsigned char *commands, size_t allocation, uint32_t device, uint32_t from, uint32_t to)
{
    size_t window = allocation - to;
    if (window > A733_NPU_FETCH_BYTES) window = A733_NPU_FETCH_BYTES;
    put(commands + from + 4, device + to);
    put(commands + from, UINT32_C(0x40000000) | (uint32_t)(window >> 3));
    for (uint32_t i = from + LINK_BYTES; i < to; i += 8) { put(commands + i, 0x10000000); put(commands + i + 4, 0); }
}
#define UNIFORM_FIRST 0xd800u
#define UNIFORM_END 0xd900u
/* A 16-bit signed tensor in dynamic fixed point: values are the integers scaled by 2^-fixed_position. The position is
 * host-side metadata of the tensor record: the engine's descriptors carry their own scales and shifts, and nothing in the loader
 * or the runtime computes with it (only the application's dequantization does), so no value is refused. A tensor of large
 * values has a negative position (-2 is a step of 4). */
static int dynamic_fixed_point(const struct nbg_tensor *t)
{
    return t->quantization == 1 && t->format == 5;
}
/* A 16-bit floating-point tensor without quantization (the compiler keeps some graph inputs in this form). */
static int half_float(const struct nbg_tensor *t)
{
    return t->quantization == 0 && t->format == 1 && !t->fixed_position && !t->zero_point && !t->scale_bits;
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
        h->operation_count < 2 ||
        h->pool_alignment != 64 || h->pool_base || h->axi_sram_base ||
        h->axi_sram_bytes || h->vip_sram_base || !h->vip_sram_bytes ||
        h->vip_sram_bytes > 0x80000) return 0;
    for (unsigned i = 0; i < NBG_SECTION_COUNT; ++i) {
        switch (i) {
        case NBG_INPUT: case NBG_OUTPUT: case NBG_LAYER: case NBG_OPERATION:
        case NBG_NN: case NBG_PATCH: case NBG_LOAD_STATES:
        case NBG_COEFF_TABLE: case NBG_COEFF: case NBG_LOAD_STATES_TABLE: break;
        case NBG_DYNA_IO: case NBG_DYNA_IO_TABLE:
        case NBG_SH: case NBG_PPU_INSTR: case NBG_PPU_INSTR_TABLE:
            if (!directory && m->sections[i].bytes) return 0;
            break;
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
    /* Shader operations (kind 1) have no neural-network record: they take their program from the shader table. */
    uint32_t shaders = 0;
    for (uint32_t i = 1; i <= p->nn_operations; ++i) {
        struct nbg_operation q;
        if (!nbg_model_operation(m, i, &q)) return 0;
        if (q.kind == 1) ++shaders;
    }
    if (m->sections[NBG_NN].bytes != (p->nn_operations - shaders) * 192 ||
        m->sections[NBG_LOAD_STATES_TABLE].bytes != h->operation_count * 8) return 0;
    if (shaders ? (!directory || !m->sections[NBG_SH].bytes || !m->sections[NBG_PPU_INSTR].bytes ||
                   (m->sections[NBG_PPU_INSTR].bytes & 255) || !m->sections[NBG_PPU_INSTR_TABLE].bytes ||
                   m->sections[NBG_PPU_PARAM].bytes) :
                  (m->sections[NBG_SH].bytes || m->sections[NBG_PPU_INSTR].bytes ||
                   m->sections[NBG_PPU_INSTR_TABLE].bytes)) return 0;
    /* The body is the prefix plus every operation's state; operation()
     * later admits only the 24- and 32-byte state forms. */
    uint64_t body = sizeof(prefix), segment = 0;
    for (uint32_t i = 1; i <= p->nn_operations; ++i) {
        struct nbg_span state_span;
        if (!nbg_model_table(m, NBG_LOAD_STATES_TABLE, i, &state_span)) return 0;
        body = segment_place(body, &segment, state_span.bytes);
        if (body == UINT64_MAX) return 0;
        body += state_span.bytes;
    }
    /* The footer starts where the last operation ends, or in a segment of its own after a link. */
    body = segment_place(body, &segment, sizeof(footer));
    if (body == UINT64_MAX || body > NBG_MODEL_MAX_BYTES) return 0;
    p->command_body_bytes = (uint32_t)body;
    uint32_t command_bytes = p->command_body_bytes + (uint32_t)sizeof(footer);
    /* The allocation holds the stream and an END pair after it, rounded to a page. The first fetch is at most
     * A733_NPU_FETCH_BYTES; a longer stream continues through links. */
    uint64_t fetch = (uint64_t)command_bytes + 64;
    if (fetch < A733_NPU_MIN_COMMAND_BYTES) fetch = A733_NPU_MIN_COMMAND_BYTES;
    /* A graph of shader operations only has no neural-network record; its slot then holds one unused page. */
    const uint32_t nn_bytes = m->sections[NBG_NN].bytes ? m->sections[NBG_NN].bytes : 192;
    if (!requirement(&p->buffers[NBG_ALLOC_NN], nn_bytes, nn_bytes) ||
        /* A graph without coefficients (an element-wise or activation-free graph) has an empty slot. */
        (m->sections[NBG_COEFF].bytes && !requirement(&p->buffers[NBG_ALLOC_COEFF], m->sections[NBG_COEFF].bytes,
                     (uint64_t)m->sections[NBG_COEFF].bytes + 128)) ||
        (h->pool_bytes && !requirement(&p->buffers[NBG_ALLOC_POOL], h->pool_bytes, h->pool_bytes)) ||
        !requirement(&p->buffers[NBG_ALLOC_COMMAND], command_bytes, fetch)) return 0;
    /* Affine UINT8, INT8 and INT16 tensors, in model order: inputs, then outputs. */
    for (uint32_t i = 0; i < p->inputs + p->outputs; ++i) {
        struct nbg_tensor t;
        int output = i >= p->inputs;
        if (!nbg_model_tensor(m, output ? NBG_OUTPUT : NBG_INPUT, output ? i - p->inputs : i, &t) ||
            (t.format != 1 && t.format != 2 && t.format != 3 && t.format != 5) || t.buffer_type != 0 ||
            (t.quantization != 2 && !dynamic_fixed_point(&t) && !half_float(&t)) ||
            !requirement(&p->buffers[NBG_ALLOC_INPUT + i], t.logical_bytes,
                         (((uint64_t)t.logical_bytes + 63) & ~UINT64_C(63)) + 64)) return 0;
    }
    if (m->sections[NBG_DYNA_IO].bytes || m->sections[NBG_DYNA_IO_TABLE].bytes || shaders) {
        if ((m->sections[NBG_DYNA_IO].bytes || m->sections[NBG_DYNA_IO_TABLE].bytes) &&
            (!directory || !m->sections[NBG_DYNA_IO].bytes || !m->sections[NBG_DYNA_IO_TABLE].bytes)) return 0;
        /* The dynamic-input section, then the program section at a multiple of 256, in one read-only allocation. */
        uint64_t payload = m->sections[NBG_DYNA_IO].bytes;
        if (shaders) {
            p->program_offset = (uint32_t)((payload + 255) & ~UINT64_C(255));
            p->program_bytes = m->sections[NBG_PPU_INSTR].bytes;
            payload = (uint64_t)p->program_offset + p->program_bytes;
        }
        if (payload > NBG_MODEL_MAX_BYTES) return 0;
        p->dynamic = 1;
        if (!requirement(&p->buffers[nbg_layout_dynamic_index(p)], (uint32_t)payload, payload + 128)) return 0;
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
/* nn_slot: the index of the operation's descriptor in the descriptor section, UINT32_MAX for a shader operation. */
struct operation_image { unsigned char nn[192], command[MAX_STATE_BYTES]; uint32_t command_bytes, nn_slot; };

/* Slots identify the observed pointer fields, not arbitrary patch destinations.
 * Circular sizes and ends are in 64-byte units in both the Mesa NN layout and
 * the pinned HAL transpose encoder. Checking the encoded ring does not prove
 * that all device accesses (including prefetch) remain within it. */
enum pointer_slot { WEIGHTS, INPUT, OUTPUT, OUTPUT_END, INPUT_END, INPUT2, INPUT2_END, POINTER_COUNT };
static int ring(const unsigned char *descriptor, struct nbg_relocation *nn,
                const uint32_t *slot, const uint32_t *source, unsigned tensor,
                unsigned end_slot, unsigned size_word)
{
    uint64_t size = (uint64_t)(word(descriptor + size_word * 4) & 0x03ffffff) << 6;
    if (slot[tensor] == UINT32_MAX) {
        /* An operation without this tensor (a stream-processor step that only computes on its program
         * and constants) keeps an unpatched pointer word inside the first page, which the page tables
         * never map: the table builder refuses a region at device address 0 and every region it accepts starts
         * at or above its page size (checks/npu-mmu.c, "page zero"; checks/os-npu-service.c walks the runtime's
         * own tables), and placed() requires buffers at device addresses of at least NBG_ASSEMBLY_LOW_RESERVED. The ring stays disabled. */
        return slot[end_slot] == UINT32_MAX && !size &&
            word(descriptor + (size_word == 20 ? 24 : 28)) < NBG_ASSEMBLY_LOW_RESERVED &&
            (word(descriptor + (size_word + 1) * 4) & 0x03ffffff) == 0x03ffffff;
    }
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

/* The byte range an input or output pointer occupies in its region after ring() and the second-input ring have
 * run: the ring when it has an end field, else the addressed byte. */
static void extent(const struct nbg_relocation *nn, const uint32_t *slot, unsigned pointer, unsigned end,
                   uint64_t *begin, uint64_t *stop)
{
    if (slot[end] != UINT32_MAX) {
        *begin = nn[slot[end]].source_offset; *stop = *begin + nn[slot[end]].referenced_bytes;
    } else {
        *begin = nn[slot[pointer]].source_offset; *stop = *begin + 1;
    }
}
/* No second input overlaps the output range in the same region (0 of 54 in the examined graphs), and neither
 * does the first input of a stream-processor, activation or transpose operation (0 of 534). A neural-network
 * operation may overlap its first input and output, as the compiler emits for the published graph (7 of
 * 535 operations): the engine consumes a ring as it writes behind it. */
static int aliased(const struct nbg_relocation *nn, const uint32_t *slot, const uint32_t *source, uint32_t kind)
{
    if (slot[OUTPUT] == UINT32_MAX) return 0;
    uint64_t ob, oe;
    extent(nn, slot, OUTPUT, OUTPUT_END, &ob, &oe);
    for (unsigned which = 0; which < 2; ++which) {
        const unsigned pointer = which ? INPUT2 : INPUT, end = which ? INPUT2_END : INPUT_END;
        if (slot[pointer] == UINT32_MAX || (!which && kind == 2) || source[slot[pointer]] != source[slot[OUTPUT]] ||
            nn[slot[pointer]].source_device != nn[slot[OUTPUT]].source_device) continue;
        uint64_t ib, ie;
        extent(nn, slot, pointer, end, &ib, &ie);
        if (ib < oe && ob < ie) return 1;
    }
    return 0;
}
/* Shader operations (kind 1): docs/hardware/npu-command-formats.md, "Loader specification (shader operations)". The
 * state stream is a sequence of load-state, stall and pad packets (S1). It may write only the registers the compiler
 * writes (S2a, checked per operand register) and the uniform bank (S4); the loader-owned flush and synchronization writes
 * are pinned, and the MMU, cache-flush and command-fetch registers are not in the set. The program is the row of the
 * program table that the shader table names (S9); patches name regions of the job (S6) and descriptors stay inside them
 * (S7, S8). The program words are not interpreted. */
static const uint16_t shader_registers[] = {
    0x5580, 0x0e02, 0x02a4, 0x0420, 0x0416, 0x0409, 0x021f, 0x0424, 0x040a, 0x021a, 0x0425, 0x0427, 0x0402, 0x0403, 0x0404,
    0x0228, 0x028c, 0x0e07, 0x040c, 0x020c, 0x0201, 0x02aa, 0x0e22, 0x0412, 0x0e27, 0x0240, 0x0249, 0x022e, 0x0247, 0x024b,
    0x024d, 0x024f, 0x0256, 0x0257, 0x0258, 0x0250, 0x0251, 0x0252, 0x0253, 0x0254, 0x0255, 0x0248, 0x0e03};
static int shader_register(uint32_t r)
{
    for (unsigned i = 0; i < sizeof(shader_registers) / sizeof(*shader_registers); ++i)
        if (shader_registers[i] == r) return 1;
    return 0;
}
/* The packet at word i of a state stream: the register and operand count of a load-state packet, or 0 for anything else. */
struct shader_packet { uint32_t first, reg, count; };
/* Find the load-state packet that has an operand at word w; returns 0 when w is not an operand word. */
static int shader_packet_of(const unsigned char *state, uint32_t n, uint32_t w, struct shader_packet *out)
{
    for (uint32_t i = 0; i < n;) {
        const uint32_t h = word(state + 4 * i);
        if ((h >> 27) == 1) {
            const uint32_t cnt = (h >> 16) & 0x7ff;
            if (w > i && w <= i + cnt) { *out = (struct shader_packet){i + 1, h & 0xffff, cnt}; return 1; }
            i += 1 + cnt + ((1 + cnt) & 1);
        } else i += 2;
    }
    return 0;
}
/* Find the load-state packet whose first register is reg and that has exactly cnt operands. */
static int shader_packet_at(const unsigned char *state, uint32_t n, uint32_t reg, uint32_t cnt, struct shader_packet *out)
{
    for (uint32_t i = 0; i < n;) {
        const uint32_t h = word(state + 4 * i);
        if ((h >> 27) == 1) {
            const uint32_t c = (h >> 16) & 0x7ff;
            if ((h & 0xffff) == reg && c == cnt) { *out = (struct shader_packet){i + 1, reg, c}; return 1; }
            i += 1 + c + ((1 + c) & 1);
        } else i += 2;
    }
    return 0;
}
/* command_out is the place the patched stream goes (the command allocation), or NULL when only the layout is wanted.
 * The model is read in place; the only memory this function adds to the stack is a few words. */
static int shader_operation(const struct nbg_model *m, uint32_t number, uint32_t first,
                            const struct nbg_assembly_buffer *buffers, const struct nbg_assembly_layout *plan,
                            const struct nbg_operation *op, struct operation_image *image, uint32_t *count,
                            unsigned char *command_out, size_t command_room)
{
    struct nbg_span state, program;
    if (!plan->program_bytes || op->state != number || op->first_patch != first || !op->patch_count ||
        !nbg_model_table(m, NBG_LOAD_STATES_TABLE, op->state, &state) ||
        state.bytes < 8 || state.bytes > A733_NPU_FETCH_BYTES || (state.bytes & 7) || (command_out && state.bytes > command_room)) return 0;
    /* A patch names one operand word of the stream, so there are at most as many patches as operand words. */
    const uint32_t n = (uint32_t)state.bytes / 4;
    if (op->patch_count > n) return 0;
    /* S9: the shader table row, its program row and the program inside the program section. */
    const struct nbg_section *sh = &m->sections[NBG_SH];
    if ((uint64_t)op->index * 8 + 8 > sh->bytes) return 0;
    const unsigned char *row = m->data + sh->offset + (size_t)op->index * 8;
    if (word(row + 4) || !nbg_model_table(m, NBG_PPU_INSTR_TABLE, word(row), &program)) return 0;
    const uint64_t start = (uint64_t)(program.data - (m->data + m->sections[NBG_PPU_INSTR].offset));
    if ((start & 255) || !program.bytes || (program.bytes & 15) || start + program.bytes > plan->program_bytes) return 0;
    /* S1, S2a, S4: the packets. The uniform bank is the register range 0xd800 to 0xd8ff (S4), 256 words, one bit each. */
    uint8_t uniform_seen[(UNIFORM_END - UNIFORM_FIRST) / 8] = {0};
    uint32_t instructions = 0, last_pc = 0;
    unsigned have_count = 0, have_last = 0, have_program = 0;
    for (uint32_t i = 0; i < n;) {
        const uint32_t h = word(state.data + 4 * i);
        if ((h >> 27) == 1) {
            /* S1: a load-state packet has 1 to 64 operands. The count field is ten bits wide (1023); 64 is the
             * specification's bound, which every compiled stream observed keeps. */
            const uint32_t cnt = (h >> 16) & 0x7ff, reg = h & 0xffff;
            if ((h & 0x04000000) || cnt < 1 || cnt > 64 || i + 1 + cnt > n) return 0;
            const int uniform = reg >= UNIFORM_FIRST && reg + cnt <= UNIFORM_END;
            if (uniform) {
                /* S4: a uniform packet has 1 to 4 operands (the specification's bound), and no word is written twice. */
                if (cnt > 4) return 0;
                for (uint32_t k = 0; k < cnt; ++k) {
                    const uint32_t bit = reg - UNIFORM_FIRST + k;
                    if (uniform_seen[bit / 8] & (1u << (bit % 8))) return 0;
                    uniform_seen[bit / 8] |= (uint8_t)(1u << (bit % 8));
                }
            }
            for (uint32_t k = 0; k < cnt; ++k) {
                const uint32_t r = reg + k, v = word(state.data + 4 * (i + 1 + k));
                if (!uniform && !shader_register(r)) return 0; /* S2a: the MMU, flush, fetch and control registers are not in the set */
                if ((r == 0x0e02 && v != 0x701) || (r == 0x0e03 && v != 0x24)) return 0; /* loader-owned writes, pinned */
                if (r == 0x0424) { if (have_count) return 0; have_count = 1; instructions = v; }
                if (r == 0x0425) { if (have_last) return 0; have_last = 1; last_pc = v; }
                if (r == 0x040a) { if (have_program) return 0; have_program = 1; }
            }
            i += 1 + cnt;
            if ((1 + cnt) & 1) { if (i >= n) return 0; ++i; } /* the pad word, rewritten below */
        } else if (h == 0x48000000) {
            if (i + 1 >= n || word(state.data + 4 * (i + 1)) != 0x701) return 0;
            i += 2;
        } else return 0;
    }
    /* S5: the instruction count and the program length. */
    if (!have_count || !have_last || !have_program || !instructions || last_pc != instructions - 1 ||
        (uint64_t)instructions * 16 != program.bytes) return 0;
    /* S6, S7, S8: the patches and their regions. Each relocation is computed on a one-word cell and then written into
     * the patched stream, so the model is never copied. */
    unsigned program_patch = 0;
    int program_at = -1, data_at = -1;
    const uint32_t extra = buffers ? buffers[nbg_layout_dynamic_index(plan)].device : 0x1000;
    unsigned char *const out = command_out;
    if (out) memcpy(out, state.data, state.bytes);
    for (uint32_t i = 0; i < op->patch_count; ++i) {
        struct nbg_patch p;
        if (!nbg_model_patch(m, op->first_patch + i, &p) || p.kind != 0 || p.name_word || p.high_destination ||
            p.high_mask || p.destination_base || (p.destination & 3) || p.destination / 4 >= n) return 0;
        for (uint32_t j = 0; j < i; ++j) { /* no word is patched twice */
            struct nbg_patch q;
            if (!nbg_model_patch(m, op->first_patch + j, &q) || q.destination == p.destination) return 0;
        }
        struct shader_packet packet;
        if (!shader_packet_of(state.data, n, p.destination / 4, &packet)) return 0;
        const uint32_t reg = packet.reg + (p.destination / 4 - packet.first);
        struct nbg_relocation r = {0};
        r.destination_offset = 0; r.encoding = NBG_ADDRESS_WORD; r.source_offset = p.source_offset;
        if (p.source == 0) {
            /* The program patch: the only source-0 patch, on the program address, which the loader writes. */
            if (reg != 0x040a || p.index != -1 || p.source_offset || p.transformed || program_patch) return 0;
            program_patch = 1; program_at = (int)p.destination;
            r.source_device = extra + plan->program_offset + (uint32_t)start; r.source_bytes = program.bytes;
            r.referenced_bytes = program.bytes;
        } else {
            if (reg < UNIFORM_FIRST || reg >= UNIFORM_END) return 0;
            if (p.transformed && !(p.transformed == 14 && p.source == 6)) return 0;
            uint64_t region = 0;
            switch (p.source) {
            case 1: case 2: {
                struct nbg_tensor t;
                const uint32_t count_of = p.source == 1 ? plan->inputs : plan->outputs;
                if (p.index < 0 || (uint32_t)p.index >= count_of ||
                    !nbg_model_tensor(m, p.source == 1 ? NBG_INPUT : NBG_OUTPUT, (uint32_t)p.index, &t)) return 0;
                const unsigned id = NBG_ALLOC_INPUT + (uint32_t)p.index + (p.source == 1 ? 0 : plan->inputs);
                region = t.logical_bytes;
                r.source_device = buffers ? buffers[id].device : 0x1000; r.source_bytes = plan->buffers[id].minimum_bytes;
                break;
            }
            case 3:
                if (p.index != -1) return 0;
                region = m->header.pool_bytes;
                r.source_device = buffers ? buffers[NBG_ALLOC_POOL].device : 0x1000; r.source_bytes = region;
                break;
            case 5: {
                struct nbg_span row_span;
                if (p.index < 0 || !nbg_model_table(m, NBG_COEFF_TABLE, (uint32_t)p.index, &row_span)) return 0;
                const uint64_t offset = (uint64_t)(row_span.data - (m->data + m->sections[NBG_COEFF].offset));
                const uint64_t base = buffers ? buffers[NBG_ALLOC_COEFF].device : 0x1000;
                if (base + offset > UINT32_MAX) return 0;
                region = row_span.bytes;
                r.source_device = (uint32_t)(base + offset); r.source_bytes = region;
                break;
            }
            case 6: {
                struct nbg_span row_span;
                if (!m->sections[NBG_DYNA_IO].bytes || p.index < 0 ||
                    !nbg_model_table(m, NBG_DYNA_IO_TABLE, (uint32_t)p.index, &row_span)) return 0;
                const uint64_t offset = (uint64_t)(row_span.data - (m->data + m->sections[NBG_DYNA_IO].offset));
                if ((uint64_t)extra + offset > UINT32_MAX) return 0;
                region = row_span.bytes;
                r.source_device = (uint32_t)(extra + offset); r.source_bytes = region;
                break;
            }
            case 8:
                if (p.index != -1) return 0;
                region = m->header.vip_sram_bytes;
                r.source_device = 0x400000; r.source_bytes = region;
                break;
            default: return 0;
            }
            r.referenced_bytes = 1;
            /* S7, S8: a patched word that opens a tensor descriptor (two 4-operand uniform packets) names an extent. */
            struct shader_packet next;
            if (p.source != 6 && packet.first == p.destination / 4 && packet.count == 4 && !((reg - UNIFORM_FIRST) & 3) &&
                shader_packet_at(state.data, n, reg + 4, 4, &next)) {
                const uint32_t stride = word(state.data + 4 * (packet.first + 1)), hw = word(state.data + 4 * (packet.first + 2));
                const uint32_t plane = word(state.data + 4 * next.first), depth = word(state.data + 4 * (next.first + 1));
                /* The channel stride is the row stride times the height, and the word hw holds the width in its low
                 * half and the height in its high half. The compiler computes width + (height << 16) in 32 bits without
                 * masking, so a tensor with a width or a height of 65,536 or more has a word that reads as another shape
                 * (T = 184 whole-length attention: width 67,528 and four rows give 0x000507c8, which the shader reads as
                 * width 1,992 and height 5). The engine's shader reads 16-bit fields, so it cannot run such a tensor
                 * correctly and the output depends on what the pool held. The SDK simulator computes with the 32-bit
                 * values and agrees with the compiler, so its agreement does not cover this. The height is the channel
                 * stride over the row stride; the loader refuses a width or height of 65,536 or more with the reason
                 * NBG_REFUSED_DIMENSION, and below that requires hw >> 16 to be the height. */
                if (!stride || !depth || plane % stride) return 0;
                const uint64_t height = plane / stride;
                const uint32_t width = hw - (uint32_t)(height << 16);
                if (!height || width > stride) return 0;
#ifndef NBG_ALLOW_LONG_DIMENSION   /* -DNBG_ALLOW_LONG_DIMENSION builds a diagnostic that admits long tensors, to vary the dimension on the board */
                if (height >= 65536 || width >= 65536) { refused(NBG_REFUSED_DIMENSION, number); return 0; }
#endif
                const uint64_t extent = (uint64_t)plane * depth;
                if ((uint64_t)p.source_offset + extent > region) return 0;
                r.referenced_bytes = extent ? extent : 1;
            }
            if (data_at < 0 && (p.source == 1 || p.source == 2)) data_at = (int)p.destination;
        }
        unsigned char cell[4] = {0};
        if (!nbg_relocate(cell, sizeof(cell), &r, 1)) return 0;
        if (out) memcpy(out + p.destination, cell, 4);
    }
    (void)program_at; (void)data_at; /* used by the fault-test hook only */
    if (!program_patch) return 0;
    if (out)
        for (uint32_t i = 0; i < n;) { /* pad words are written as 0x18000000 whatever the graph holds */
            const uint32_t h = word(state.data + 4 * i);
            if ((h >> 27) == 1) {
                const uint32_t cnt = (h >> 16) & 0x7ff;
                i += 1 + cnt;
                if ((1 + cnt) & 1) put(out + 4 * i++, 0x18000000);
            } else i += 2;
        }
    image->command_bytes = (uint32_t)state.bytes; image->nn_slot = UINT32_MAX;
#ifdef SUNSTONE_NPU_FAULT_HOOK
    /* NEGATIVE-TEST IMAGES ONLY (docs/reference-backsolve-npu.md, "NPU MMU exceptions"). Hook slot 100 aims the first
     * tensor descriptor's address word of the first shader operation, and slot 101 its program address (register
     * 0x040a), at an address the NPU page tables do not map (apps/os/npu.c sets the slot before each LOAD). */
    extern int sunstone_npu_hook_slot;
    extern uint32_t sunstone_npu_hook_address;
    extern unsigned char sunstone_npu_canary[];
    if (number == 1 && (sunstone_npu_hook_slot == 100 || sunstone_npu_hook_slot == 101)) {
        const int at = sunstone_npu_hook_slot == 100 ? data_at : program_at;
        if (at < 0) return 0; /* a test build must never silently skip its fault */
        if (out) put(out + at, sunstone_npu_hook_address ? sunstone_npu_hook_address :
            (uint32_t)(uintptr_t)(sunstone_npu_canary + 0x10000));
    }
#endif
    *count = op->patch_count;
    return 1;
}

static int operation(const struct nbg_model *m, uint32_t number, uint32_t first,
                     const struct nbg_assembly_buffer *buffers,
                     const struct nbg_assembly_layout *plan, struct operation_image *image,
                     uint32_t *count, unsigned char *command_out, size_t command_room, uint32_t nn_slot)
{
    struct nbg_operation op;
    struct nbg_span span;
    int directory = m->header.version == NBG_MODEL_DIRECTORY_VERSION;
    /* Kind 2 is a neural-network operation. Observed only in the directory format: kind 8, a neural
     * transpose step, and kind 7, a neural-network step with an activation table in the weight slot,
     * use the same 192-byte descriptor and pointer fields (kind 8 without coefficients); kind 6, a
     * stream-processor step, precedes the same tail with a program (docs/designs/npu-encoder-loader.md). */
    if (!nbg_model_operation(m, number, &op)) return 0;
    if (op.kind == 1)
        return directory && shader_operation(m, number, first, buffers, plan, &op, image, count, command_out, command_room);
    /* The descriptor of an engine operation is numbered among the engine operations: shader operations have none. The
     * caller counts the engine operations before this one (a count by scanning the operations before it made a graph of
     * n operations cost n squared steps: seconds for the 8,000 operations of a layer of 512 frames). */
    image->nn_slot = nn_slot;
    const int engine = op.kind == 2 || (directory && (op.kind == 8 || op.kind == 7 || op.kind == 6));
    if (!engine ||
        op.index != nn_slot ||
        op.state != number || op.first_patch != first || !op.patch_count ||
        op.patch_count > POINTER_COUNT + 1 || op.auxiliary[0] ||
        !nbg_model_table(m, NBG_LOAD_STATES_TABLE, op.state, &span)) return 0;
    int last = number == plan->nn_operations;
    /* Whether the descriptor has a second input: both of its pointer fields are patched. */
    int second = 0;
    for (uint32_t i = 0; i < op.patch_count; ++i) {
        struct nbg_patch q;
        if (!nbg_model_patch(m, first + i, &q)) return 0;
        if (q.kind == 1 && q.destination == 136) second = 1;
    }
    uint32_t tail = 0, command_destination = 20;
    if (op.kind == 6) {
        /* Program: 0x08010e5b and its operand, then alternating 0x08010e5c and 0x08010e5d words,
         * then the 32-byte tail. The pair words are the program; no other register is written. */
        if (!directory || span.bytes < 8 + 16 + 32 || span.bytes > MAX_STATE_BYTES || (span.bytes & 15) != 8 ||
            word(span.data) != 0x08010e5b) return 0;
        tail = (uint32_t)span.bytes - 32;
        /* The operand of 0x08010e5b is the position of the program in the 256-entry program memory (the number of
         * instructions of the preceding stream-processor operations, modulo 256, in 2903 of 2903 operations of twelve
         * graphs). The program words are not interpreted: whatever they are, the engine reaches memory only through the
         * page tables, which map the job's own buffers, as the unmapped-address runs on a board show for the stream
         * processor too (docs/designs/npu-loader-admission.md). The operand must fit the memory. */
        if (word(span.data + 4) > 255) return 0;
        for (uint32_t at = 8; at < tail; at += 8)
            if (word(span.data + at) != (((at - 8) & 8) ? 0x08010e5d : 0x08010e5c)) return 0;
        if (((tail - 8) / 8) & 1) return 0; /* complete pairs */
        command_destination = tail + 20;
    } else {
        /* The 24-byte state sets word 5 and runs straight into the next operation. The 32-byte state
         * clears it and appends a second pair; the compiler emits it for the last operation and, in
         * the directory format, for every operation of graphs that contain transpose steps. */
        if (span.bytes != 32 && (last || span.bytes != 24)) return 0;
        if (span.bytes == 32 && !last && !directory) return 0;
    }
    image->command_bytes = (uint32_t)span.bytes;
    const unsigned tail_words = op.kind == 6 ? 8 : (unsigned)span.bytes / 4;
    for (unsigned i = 0; i < tail_words; ++i) {
        uint32_t expected = i == 5 && (span.bytes == 32 || op.kind == 6) ? 0 : state[i];
        // The observed directory profile changes bit 17 of this state value.
        // Its hardware meaning remains unresolved; require the captured bytes.
        // The 16-bit graphs set bit 21 and a second input sets bit 12 (observed, not documented).
        if (i == 1 && directory) {
            const uint32_t seen = word(span.data + tail + 4);
            if (seen != 0x0493e040 && seen != 0x04b3e040 && seen != 0x04b3f040) return 0;
            if (!!(seen & 0x1000) != second) return 0;
            expected = seen;
        }
        if (word(span.data + tail + i * 4) != expected) return 0;
    }
    memcpy(image->command, span.data, span.bytes);
    memcpy(image->nn, m->data + m->sections[NBG_NN].offset + op.index * 192, 192);
    struct nbg_relocation nn[POINTER_COUNT], command = {0};
    uint32_t slot[POINTER_COUNT], source[POINTER_COUNT];
    int32_t tensor_index[POINTER_COUNT];
    for (unsigned i = 0; i < POINTER_COUNT; ++i) slot[i] = UINT32_MAX;
    uint32_t nn_count = 0, command_count = 0;
    for (uint32_t i = 0; i < op.patch_count; ++i) {
        struct nbg_patch p;
        if (!nbg_model_patch(m, first + i, &p) || !patch_fields(&p)) return 0;
        struct nbg_relocation r = {0};
        if (p.kind == 0) {
            if (command_count || p.source || p.index || p.source_offset ||
                p.destination != command_destination || p.transformed != 1) return 0;
            command = (struct nbg_relocation){plan->buffers[NBG_ALLOC_NN].payload_bytes,
                192, buffers ? buffers[NBG_ALLOC_NN].device : 0x1000,
                op.index * 192, command_destination, NBG_ADDRESS_FLAGS6};
            ++command_count;
        } else {
            if (p.kind != 1 || nn_count == POINTER_COUNT) return 0;
            unsigned field;
            switch (p.destination) {
            case 20:
                /* Weights (kind 2), the activation table (kind 7) or the stream-processor constants (kind 6). */
                if ((op.kind != 2 && op.kind != 7 && op.kind != 6) || p.source != 5 || p.transformed != 1) return 0;
                field = WEIGHTS; break;
            case 136: case 140:
                /* Second input and its end: observed only in VIP SRAM (source 8), 17 kind 2 and 1 kind 6
                 * operations of the layer graph; the end is an address shifted by 6 as for the first input. */
                /* A graph input (source 1), a range of the memory pool (source 3; the compiler puts the second input of an
                 * attention product there once the scores no longer fit VIP SRAM) or a dynamic input (source 6, kind 6)
                 * has no end field; an input in VIP SRAM (source 8) has one. */
                if (p.destination == 136 ? (p.source != 8 && p.source != 1 && p.source != 2 && p.source != 3 && !(p.source == 6 && op.kind == 6)) :
                    p.source != 8) return 0;
                if (p.transformed != (p.destination == 140)) return 0;
                field = p.destination == 136 ? INPUT2 : INPUT2_END; break;
            case 24:
                /* A graph input, the pool, a dynamic input, VIP SRAM, or a graph output that an earlier operation wrote and a
                 * later one reads again (source 2: a tensor that is both an intermediate and a result of the graph). */
                if ((p.source != 1 && p.source != 2 && p.source != 3 && p.source != 6 && p.source != 8) || p.transformed) return 0;
                if (p.source == 6 && op.kind != 6) return 0;
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
            tensor_index[nn_count] = p.index;
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
            case 6: {
                /* Dynamic input: one table record of the model's dynamic-input section, copied into
                 * the last allocation. The record bounds the reference. */
                if (!plan->dynamic || p.index < 0 ||
                    !nbg_model_table(m, NBG_DYNA_IO_TABLE, (uint32_t)p.index, &span)) return 0;
                uint64_t offset = (uint64_t)(span.data - (m->data + m->sections[NBG_DYNA_IO].offset));
                uint64_t base = buffers ? buffers[nbg_layout_dynamic_index(plan)].device : 0x1000;
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
    /* Second input (descriptor words 32 to 35, all observed in 441 operations of the layer graphs and
     * single-operation graphs; docs/hardware/npu-command-formats.md). Without one, word 33 is zero. An input
     * that is not in VIP SRAM has word 33 equal to 1 (0 for a batched matrix product) and a disabled end word. An input in VIP SRAM is a ring
     * like the first input's: word 33 holds its size in 32-byte units, the end field its limit, and the
     * start lies inside [limit - size, limit). */
    if (second != (slot[INPUT2] != UINT32_MAX)) return 0;
    if (!second) {
        if (word(image->nn + 132)) return 0;
    } else if (source[slot[INPUT2]] != 8) {
        if (slot[INPUT2_END] != UINT32_MAX || word(image->nn + 132) > 1 ||
            (word(image->nn + 140) & 0x03ffffff) != 0x03ffffff) return 0;
    } else {
        if (slot[INPUT2_END] == UINT32_MAX) return 0;
        struct nbg_relocation *start = &nn[slot[INPUT2]], *end = &nn[slot[INPUT2_END]];
        const uint64_t size = (uint64_t)(word(image->nn + 132) & 0x03ffffff) << 5, limit = end->source_offset;
        if (!size || size > limit || limit > end->source_bytes || start->source_device != end->source_device ||
            start->source_bytes != end->source_bytes || start->source_offset < limit - size ||
            start->source_offset >= limit) return 0;
        end->source_offset = (uint32_t)(limit - size);
        end->referenced_bytes = size;
        end->encoding = NBG_ADDRESS_END_SHIFT6;
    }
    /* An input taken from a graph input names no more bytes than the tensor holds (word 16 for the first
     * input, word 32 for the second: the byte count in the upper half). Two graph inputs of one operation
     * agree in type and size; their fixed-point positions may differ. */
    for (unsigned which = 0; which < 2; ++which) {
        const unsigned field = which ? INPUT2 : INPUT, word_index = which ? 32 : 16;
        if (slot[field] == UINT32_MAX || source[slot[field]] != 1) continue;
        struct nbg_tensor t;
        if (!nbg_model_tensor(m, NBG_INPUT, (uint32_t)tensor_index[slot[field]], &t) ||
            (word(image->nn + word_index * 4) >> 16) > t.logical_bytes) return 0;
    }
    if (slot[INPUT] != UINT32_MAX && slot[INPUT2] != UINT32_MAX && source[slot[INPUT]] == 1 &&
        source[slot[INPUT2]] == 1) {
        struct nbg_tensor a, b;
        if (!nbg_model_tensor(m, NBG_INPUT, (uint32_t)tensor_index[slot[INPUT]], &a) ||
            !nbg_model_tensor(m, NBG_INPUT, (uint32_t)tensor_index[slot[INPUT2]], &b) ||
            a.format != b.format || a.quantization != b.quantization ||
            a.logical_bytes != b.logical_bytes) return 0;
    }
    if ((slot[WEIGHTS] == UINT32_MAX && word(image->nn + 20) >= NBG_ASSEMBLY_LOW_RESERVED) ||
        (slot[INPUT2] == UINT32_MAX && word(image->nn + 136) >= NBG_ASSEMBLY_LOW_RESERVED)) return 0;
    if (!ring(image->nn, nn, slot, source, INPUT, INPUT_END, 20) ||
        !ring(image->nn, nn, slot, source, OUTPUT, OUTPUT_END, 18) ||
        aliased(nn, slot, source, op.kind) ||
        command_count != 1 || !nbg_relocate(image->command, image->command_bytes, &command, 1) ||
        (nn_count && !nbg_relocate(image->nn, sizeof(image->nn), nn, nn_count))) return 0;
#ifdef SUNSTONE_NPU_FAULT_HOOK
    /* NEGATIVE-TEST IMAGES ONLY (docs/reference-backsolve-npu.md, "NPU MMU exceptions"). Point one descriptor
     * pointer of the first operation at an address that the NPU page tables do not map, to show that the NPU's own
     * access faults. The image sets the pointer slot (sunstone_npu_hook_slot: 1 first input, 2 output, 5 second
     * input; negative for none) and the address before each LOAD (apps/os/npu.c). Address 0 means 64 KiB into the
     * image's canary buffer, a DRAM region that the tables do not map. A pointer with an end field keeps its extent:
     * the end moves by the same distance. The new values are encoded as the relocation of the field would. */
    extern int sunstone_npu_hook_slot;
    extern uint32_t sunstone_npu_hook_address;
    extern unsigned char sunstone_npu_canary[];
    if (number == 1 && sunstone_npu_hook_slot >= 0 && sunstone_npu_hook_slot < 100) {
        const unsigned field = (unsigned)sunstone_npu_hook_slot;
        if (field >= POINTER_COUNT || slot[field] == UINT32_MAX) return 0; /* a test build must never silently skip its fault */
        const unsigned end_field = field == INPUT ? INPUT_END : field == OUTPUT ? OUTPUT_END :
            field == INPUT2 ? INPUT2_END : POINTER_COUNT;
        const struct nbg_relocation *r = &nn[slot[field]];
        unsigned char *d = image->nn + r->destination_offset;
        const int shifted = r->encoding == NBG_ADDRESS_SHIFT6 || r->encoding == NBG_ADDRESS_END_SHIFT6;
        const uint32_t before = word(d), target = sunstone_npu_hook_address ? sunstone_npu_hook_address :
            (uint32_t)(uintptr_t)(sunstone_npu_canary + 0x10000);
        uint32_t value = shifted ? (before & UINT32_C(0xfc000000)) | (target >> 6) : target;
        d[0] = (unsigned char)value; d[1] = (unsigned char)(value >> 8);
        d[2] = (unsigned char)(value >> 16); d[3] = (unsigned char)(value >> 24);
        if (end_field != POINTER_COUNT && slot[end_field] != UINT32_MAX) {
            unsigned char *e = image->nn + nn[slot[end_field]].destination_offset;
            const uint32_t old_start = shifted ? (before & 0x03ffffffu) : before >> 6, old_end = word(e) & 0x03ffffffu;
            const uint32_t end_units = old_end + (target >> 6) - old_start;
            value = (word(e) & UINT32_C(0xfc000000)) | (end_units & 0x03ffffffu);
            e[0] = (unsigned char)value; e[1] = (unsigned char)(value >> 8);
            e[2] = (unsigned char)(value >> 16); e[3] = (unsigned char)(value >> 24);
        }
    }
#endif
    if (command_out) {
        if (image->command_bytes > command_room) return 0;
        memcpy(command_out, image->command, image->command_bytes);
    }
    *count = op.patch_count;
    return 1;
}

static int inspect(const struct nbg_model *m, const struct nbg_assembly_buffer *b,
                   struct nbg_assembly_layout *p)
{
    uint32_t first = 1, slot = 0;
    for (uint32_t i = 1; i <= p->nn_operations; ++i) {
        struct operation_image scratch;
        uint32_t count;
        if (!operation(m, i, first, b, p, &scratch, &count, NULL, 0, slot)) return 0;
        first += count;
        if (scratch.nn_slot != UINT32_MAX) ++slot;
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
    refusal_reason = NBG_REFUSED_NONE; refusal_operation = 0;
    if (!result_space(m, out, sizeof(*out)) || !profile(m, &p, 1) || !inspect(m, NULL, &p)) return 0;
    *out = p;
    return 1;
}

/* A buffer is exactly its allocation, except the command buffer: 87afaecf sized the layout's command allocation from the graph's own
 * stream, so a service with a fixed hardware-maximum command buffer (the convolution's reserved layout) passes a larger one, up to
 * the fetch limit rounded to a page. The assembler fills all of it with END pairs and the controller fetches all of it. */
#define NBG_ASSEMBLY_COMMAND_LIMIT ((A733_NPU_FETCH_BYTES + 4095u) & ~4095u)
static inline int nbg_assembly_bytes_fit(unsigned index, uint64_t bytes, uint64_t allocation)
{
    return index == NBG_ALLOC_COMMAND ? bytes >= allocation && bytes <= NBG_ASSEMBLY_COMMAND_LIMIT : bytes == allocation;
}

/* Placement rules shared by the one-call and stepwise assemblers. Forced
 * inlining keeps the one-call assembler as compact as before the stepwise
 * form existed; only images that link the stepwise form carry the copies. */
static inline __attribute__((always_inline)) int placed(const struct nbg_model *m, const struct nbg_assembly_buffer *b, size_t count,
                  const struct nbg_assembly_layout *p, const void *out, size_t out_bytes)
{
    const size_t metadata_bytes = count * sizeof(*b);
    for (unsigned i = 0; i < count; ++i) {
        if (!p->buffers[i].allocation_bytes) {
            if (b[i].cpu || b[i].bytes || b[i].device) return 0;
            continue;
        }
        if (!nbg_assembly_bytes_fit(i, b[i].bytes, p->buffers[i].allocation_bytes) || !a733_span_excl_nonempty(b[i].cpu, b[i].bytes) ||
            ((uintptr_t)b[i].cpu & 4095) || b[i].device < NBG_ASSEMBLY_LOW_RESERVED || (b[i].device & 4095) ||
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
    if (p.buffers[NBG_ALLOC_COEFF].payload_bytes)
        memcpy(b[NBG_ALLOC_COEFF].cpu, m->data + m->sections[NBG_COEFF].offset,
               p.buffers[NBG_ALLOC_COEFF].payload_bytes);
    if (p.dynamic) {
        if (m->sections[NBG_DYNA_IO].bytes)
            memcpy(b[nbg_layout_dynamic_index(&p)].cpu, m->data + m->sections[NBG_DYNA_IO].offset,
                   m->sections[NBG_DYNA_IO].bytes);
        if (p.program_bytes)
            memcpy((unsigned char *)b[nbg_layout_dynamic_index(&p)].cpu + p.program_offset,
                   m->data + m->sections[NBG_PPU_INSTR].offset, p.program_bytes);
    }
    unsigned char *commands = b[NBG_ALLOC_COMMAND].cpu;
    for (size_t i = 0; i < b[NBG_ALLOC_COMMAND].bytes; i += 8) put(commands + i, 0x10000000);
    words(commands, prefix, sizeof(prefix) / 4);
    uint32_t first = 1, offset = (uint32_t)sizeof(prefix), slot = 0;
    uint64_t segment = 0;
    for (uint32_t i = 1; i <= p.nn_operations; ++i) {
        struct operation_image scratch;
        struct nbg_span state_span;
        uint32_t used = 0;
        (void)nbg_model_table(m, NBG_LOAD_STATES_TABLE, i, &state_span);
        const uint32_t at = (uint32_t)segment_place(offset, &segment, state_span.bytes);
        if (at != offset) write_link(commands, b[NBG_ALLOC_COMMAND].bytes, b[NBG_ALLOC_COMMAND].device, offset, at);
        offset = at;
        (void)operation(m, i, first, b, &p, &scratch, &used, commands + offset, p.command_body_bytes - offset, slot);
        if (scratch.nn_slot != UINT32_MAX) { memcpy((unsigned char *)b[NBG_ALLOC_NN].cpu + scratch.nn_slot * 192, scratch.nn, 192); ++slot; }
        first += used;
        offset += scratch.command_bytes;
    }
    {
        const uint32_t at = (uint32_t)segment_place(offset, &segment, sizeof(footer));
        if (at != offset) write_link(commands, b[NBG_ALLOC_COMMAND].bytes, b[NBG_ALLOC_COMMAND].device, offset, at);
        offset = at;
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
    *cursor = (struct nbg_assembly_cursor){1, 1, (uint32_t)sizeof(prefix), 0, 0};
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
        struct nbg_span state_span;
        uint32_t used = 0;
        uint64_t segment = cursor->segment;
        if (cursor->offset > p->command_body_bytes ||
            !nbg_model_table(m, NBG_LOAD_STATES_TABLE, cursor->operation, &state_span)) return -1;
        const uint64_t at = segment_place(cursor->offset, &segment, state_span.bytes);
        if (at > p->command_body_bytes) return -1;
        if (at != cursor->offset)
            write_link(commands, b[NBG_ALLOC_COMMAND].bytes, b[NBG_ALLOC_COMMAND].device, cursor->offset, (uint32_t)at);
        if (!operation(m, cursor->operation, cursor->patch, b, p, &scratch, &used, commands + at,
                       p->command_body_bytes - at, cursor->slot)) return -1;
        if (scratch.nn_slot != UINT32_MAX) { memcpy((unsigned char *)b[NBG_ALLOC_NN].cpu + scratch.nn_slot * 192, scratch.nn, 192); ++cursor->slot; }
        cursor->patch += used; cursor->offset = (uint32_t)at + scratch.command_bytes; cursor->segment = (uint32_t)segment;
        ++cursor->operation;
    }
    if (cursor->operation <= p->nn_operations) return 0;
    if (cursor->offset == p->command_body_bytes + sizeof(footer)) return 1;
    unsigned patch_bytes = m->header.version == NBG_MODEL_DIRECTORY_VERSION ? 28 : 40;
    uint64_t footer_segment = cursor->segment;
    const uint64_t footer_at = segment_place(cursor->offset, &footer_segment, sizeof(footer));
    if (footer_at != p->command_body_bytes || cursor->offset > footer_at ||
        cursor->patch != m->sections[NBG_PATCH].bytes / patch_bytes) return -1;
    if (footer_at != cursor->offset)
        write_link(commands, b[NBG_ALLOC_COMMAND].bytes, b[NBG_ALLOC_COMMAND].device, cursor->offset, (uint32_t)footer_at);
    words(commands + footer_at, footer, sizeof(footer) / 4);
    cursor->offset = (uint32_t)footer_at + (uint32_t)sizeof(footer);
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
