#ifndef SUNSTONE_NBG_MODEL_H
#define SUNSTONE_NBG_MODEL_H
#include <stddef.h>
#include <stdint.h>

#define NBG_MODEL_VERSION 0x1001eu
#define NBG_MODEL_HEADER_BYTES 592u
#define NBG_MODEL_FIXED_SECTION_COUNT 27u
#define NBG_MODEL_DIRECTORY_VERSION 0x20000u
#define NBG_MODEL_DIRECTORY_HEADER_BYTES 48u
#define NBG_MODEL_MAX_BYTES (256u * 1024u * 1024u)

/* Serialized section order, independent of SDK in-memory structure order. */
enum nbg_section_id {
    NBG_INPUT, NBG_OUTPUT, NBG_LAYER, NBG_OPERATION, NBG_COEFF_HEADER_TABLE,
    NBG_COEFF_HEADER, NBG_NN, NBG_TP, NBG_SH, NBG_PATCH, NBG_LAYER_PARAM,
    NBG_SW, NBG_HW_INIT, NBG_ICDT, NBG_ICD, NBG_PPU_PARAM, NBG_OP_SYNC,
    NBG_DMA_SBI, NBG_PPU_INSTR, NBG_DYNA_IO, NBG_LOAD_STATES, NBG_DMA_ADDI,
    NBG_COEFF_TABLE, NBG_COEFF, NBG_PPU_INSTR_TABLE, NBG_DYNA_IO_TABLE,
    NBG_LOAD_STATES_TABLE, NBG_DUMP_OPERATION, NBG_DUMP_OUTPUT, NBG_DUMP_PATCH,
    NBG_DUMP_INSTR_TABLE, NBG_DUMP_INSTR, NBG_DUMP_STATES_TABLE, NBG_DUMP_STATES,
    NBG_NETWORK_INFO, NBG_CUSTOMER_DATA, NBG_MEMORY_POOL, NBG_VIP_SRAM,
    NBG_AXI_SRAM, NBG_SECTION_CHECKSUM, NBG_SECTION_COUNT
};

enum nbg_decode_result {
    NBG_DECODE_OK = 0,
    NBG_DECODE_INVALID = 1,
    NBG_DECODE_UNSUPPORTED = 2
};

struct nbg_section { uint32_t offset, bytes; };
struct nbg_span { const unsigned char *data; size_t bytes; };

struct nbg_model_header {
    uint32_t version, target, flags, core_count, checksum; /* Flags retain version-specific wire bits. */
    uint32_t layer_count, operation_count, input_count, output_count;
    uint32_t nn_bytes, tp_bytes;
    uint32_t pool_bytes, pool_alignment, pool_base;
    uint32_t axi_sram_base, axi_sram_bytes, vip_sram_base, vip_sram_bytes;
    char name[65]; /* Raw name bytes followed by a guaranteed NUL, not UTF-8 validation. */
};

struct nbg_model {
    const unsigned char *data;
    size_t bytes;
    struct nbg_model_header header;
    struct nbg_section sections[NBG_SECTION_COUNT];
};

struct nbg_tensor {
    uint32_t dimensions, sizes[6], format, buffer_type, quantization;
    int32_t fixed_position;
    uint32_t scale_bits; /* IEEE binary32 bits; no floating-point runtime required. */
    int32_t zero_point;
    uint32_t logical_bytes; /* Excludes device access padding and allocation rounding. */
    char name[65];
};

struct nbg_operation {
    uint32_t kind, index, layer, state, first_patch, patch_count;
    uint32_t auxiliary[2]; /* Preserved wire words; not interpreted by this decoder. */
};

struct nbg_patch {
    uint32_t kind, destination, source;
    int32_t index;
    uint32_t source_offset, transformed, name_word;
    uint32_t high_destination, high_mask, destination_base;
};

/* Validate a 0x1001e or 32-bit-address 0x20000 container and publish a borrowed,
 * immutable view. Directory files require one subheader, known section IDs,
 * zero section addresses/reserved fields, and the exact declared total size.
 * Extra directory sections are retained as bounded raw spans. Their internal
 * semantics, including checksum records and debug tables, remain unchecked.
 * Directory operation_count is derived from the operation section; the network
 * metadata's separate count remains available through NBG_NETWORK_INFO.
 * Directory pool alignment is the 64-byte allocation convention, not a wire field.
 * Checks section ranges/overlap, record strides/counts, table-to-payload bounds,
 * tensor dimensions/logical sizes/quantization, operation state/patch ranges,
 * known NN/TP command indices, layer indices, tensor patch indices, and pool
 * alignment. Unknown operation/source/kind values remain visible for later
 * policy decisions. Raw undecoded sections remain accessible as byte spans.
 *
 * Caller keeps the complete file and successful view immutable and alive until
 * all accessors and returned spans are finished. Use exclusively owned storage
 * for construction; changing input during decoding is unsupported. No heap,
 * device access, cache operation, or locking occurs. Input may be unaligned.
 * Refusal leaves result unchanged. Input and result may not overlap.
 *
 * Successful decoding is NOT model admission: checksum, target compatibility,
 * descriptor semantics, relocation destinations, complete device access sizes,
 * software operators, command permissions, and SRAM placement remain unchecked.
 * The stored checksum is reported without verification. The separate verifier
 * supports both formats; the assembler still refuses 0x20000. */
enum nbg_decode_result nbg_model_open(const void *data, size_t bytes,
                                      struct nbg_model *result);

/* Accessors require an unchanged view returned by nbg_model_open. Return 1 on
 * success, 0 on refusal; output is unchanged on refusal. Output must not overlap
 * the view or its file. These checks do not establish arbitrary CPU pointer
 * validity or protect against a concurrently modified view. */
int nbg_model_section(const struct nbg_model *model, enum nbg_section_id section,
                      struct nbg_span *result);
int nbg_model_tensor(const struct nbg_model *model, enum nbg_section_id section,
                     uint32_t index, struct nbg_tensor *result);
int nbg_model_operation(const struct nbg_model *model, uint32_t index,
                        struct nbg_operation *result);
int nbg_model_patch(const struct nbg_model *model, uint32_t index,
                    struct nbg_patch *result);
/* Resolve an entry in one of the six offset/length tables to its payload span.
 * Accepts COEFF_HEADER_TABLE, COEFF_TABLE, PPU_INSTR_TABLE, DYNA_IO_TABLE,
 * LOAD_STATES_TABLE, or ICDT. A zero-length entry returns a valid empty span. */
int nbg_model_table(const struct nbg_model *model, enum nbg_section_id table,
                    uint32_t index, struct nbg_span *result);
#endif
