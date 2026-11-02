#ifndef SUNSTONE_NBG_ASSEMBLY_H
#define SUNSTONE_NBG_ASSEMBLY_H
#include "nbg_model.h"

#define NBG_ASSEMBLY_MAX_NN 1024u
#define NBG_ASSEMBLY_PAGE 4096u
enum nbg_allocation_id {
    NBG_ALLOC_NN, NBG_ALLOC_COEFF, NBG_ALLOC_POOL, NBG_ALLOC_COMMAND,
    NBG_ALLOC_INPUT, NBG_ALLOC_OUTPUT, NBG_ALLOCATION_COUNT
};
struct nbg_allocation_requirement {
    uint32_t payload_bytes, minimum_bytes, allocation_bytes;
};
struct nbg_assembly_plan {
    struct nbg_allocation_requirement buffers[NBG_ALLOCATION_COUNT];
    uint32_t nn_operations, descriptor_patches, state_patches, command_body_bytes;
};
/* General plan: NN, COEFF, POOL and COMMAND as above, then one allocation per
 * input tensor in model order, then one per output tensor. With one input
 * and one output this is the six-slot plan order. */
/* The most inputs, and the most outputs, of one graph. The format does not bound them (the header counts are 32-bit
 * fields, and the vendor sources show no limit); this is the loader's own choice, fixed by the size of
 * struct nbg_assembly_layout (24 bytes a tensor, kept in the per-graph table) and by the queue handle. An image is built
 * for the largest graph its application declares (cmake -DSUNSTONE_OS_NPU_MAX_TENSORS=N); 8 is the default of an image
 * built without a declaration. A graph beyond it is refused by name at admission. */
/* The low device range [0, NBG_ASSEMBLY_LOW_RESERVED) is reserved: placed() refuses a buffer that starts below it, so a
 * pointer word that no patch sets and whose value is below it cannot address a mapped byte. A stream-processor operation
 * that keeps its result in the engine's own scratch (the steps of a softmax) carries a small offset there, up to 118,496
 * in compiled attention layers (docs/hardware/npu-command-formats.md, "Unpatched pointer words"). */
#define NBG_ASSEMBLY_LOW_RESERVED 0x100000u
#ifndef NBG_ASSEMBLY_MAX_TENSORS
#define NBG_ASSEMBLY_MAX_TENSORS 8u
#endif
#define NBG_LAYOUT_MAX_BUFFERS (NBG_ALLOC_INPUT + 2u * NBG_ASSEMBLY_MAX_TENSORS + 1u)
struct nbg_assembly_layout {
    struct nbg_allocation_requirement buffers[NBG_LAYOUT_MAX_BUFFERS];
    uint32_t inputs, outputs;
    uint32_t nn_operations, descriptor_patches, state_patches, command_body_bytes;
    /* 1 when the model has a dynamic-input section (directory format): one more allocation follows the output
     * tensors, filled from the model with that section's bytes, that patch source 6 addresses. The assembler maps
     * nothing and cannot make it read-only; the runtime places it after the coefficients in the slot's constant
     * region, which the NPU page tables map read-only (a733_npu_runtime.c targets(); checks/os-npu-service.c). */
    uint32_t dynamic;
    /* The dynamic-input allocation also holds the program section of a graph with shader operations: the program
     * section is copied at program_offset (a multiple of 256) of that allocation, program_bytes long (zero when the
     * graph has no shader operation). The allocation then exists whenever either section does. */
    uint32_t program_offset, program_bytes;
};
/* The number of allocations of a layout, in plan order: the four fixed ones, the inputs, the outputs
 * and the dynamic-input allocation when present. */
static inline uint32_t nbg_layout_buffers(const struct nbg_assembly_layout *l)
{ return NBG_ALLOC_INPUT + l->inputs + l->outputs + l->dynamic; }
static inline uint32_t nbg_layout_dynamic_index(const struct nbg_assembly_layout *l)
{ return NBG_ALLOC_INPUT + l->inputs + l->outputs; }
struct nbg_assembly_buffer {
    void *cpu;
    size_t bytes;
    uint32_t device;
};

/* Offline assembly experiment for the observed single-core A733 NN profile.
 * Requires an immutable successful nbg_model_open view and file. Supports one
 * UINT8 input/output, INIT followed by 1..1024 sequential 192-byte NN records,
 * the observed load-state templates, and patch sources 0/1/2/3/5/8. Other
 * layouts are refused. Supports the observed 0x1001e and 0x20000 profiles;
 * the applicable primary and section checksums must match. Full profile
 * validation precedes publishing requirements.
 * Pointer patches are limited to weight/input/output fields and circular ends.
 * A nonzero ring size requires its end patch, the same source as its tensor,
 * the complete ring inside that source, and the tensor address inside the ring.
 * Zero-size rings require the observed unpatched disabled-end sentinel.
 * Minimum sizes reproduce observed SDK padding; allocation sizes round to 4 KiB
 * and reserve the controller's complete command fetch. Total <= 256 MiB.
 * Returns 1 on success, 0 on refusal. Result must not overlap view or file.
 * Refusal leaves result unchanged. No allocation, MMIO, cache, or locking.
 *
 * SUCCESS IS NOT SUBMISSION AUTHORIZATION. Linear/weight references check one
 * addressed byte only. Ring geometry does not establish actual device accesses
 * or prefetch limits. Complete access extents, SRAM mapping, and
 * descriptor semantics remain unverified. No controller token is produced. */
int nbg_assembly_requirements(const struct nbg_model *, struct nbg_assembly_plan *);

/* Supply six buffer slots in enum order. An empty working pool requires
 * {NULL, 0, 0}; no allocation or memory access occurs for that slot.
 * Supply exclusively owned offline buffers for nonempty slots, with exact
 * planned sizes, 4 KiB aligned CPU/device bases, nonoverlapping CPU/device
 * ranges, and no device overlap with SRAM 0x400000..0x47ffff. Physical aliases
 * and ownership are the caller's responsibility; this layer cannot check them.
 * Metadata/result/view/file must not overlap output buffers; result must also
 * not overlap metadata, view, or file. Keep all inputs stable during the call.
 *
 * Validates every operation and relocation before changing any caller bytes.
 * Initializes all nonempty allocations, including input/output and padding; supply
 * input data AFTER assembly. Adapts the SDK completion event to event 0 and
 * fills unused fetch space with END pairs. Refusal preserves buffers and result.
 * Reuses bounded stack scratch per operation; no heap or borrowed output spans.
 * Returns the same plan as requirements. Never submit these bytes until the
 * separate admission and platform requirements above are resolved. */
int nbg_assemble(const struct nbg_model *,
                 const struct nbg_assembly_buffer[NBG_ALLOCATION_COUNT],
                 struct nbg_assembly_plan *);

/* General form of the two functions above for graphs with 1..8 inputs and
 * 1..8 outputs of affine UINT8, INT8 or INT16 tensors. In the directory
 * format it also accepts neural-transpose operations (kind 8), which the
 * compiler emits around fully connected layers, and the 32-byte operation
 * state for every operation; nn_operations then counts both kinds. The
 * relocation, ring and checksum policy is otherwise identical, and patch
 * sources 1 and 2 select the input or output allocation by tensor index. Supply
 * NBG_ALLOC_INPUT + inputs + outputs buffer slots in plan order with the same
 * placement rules. The same refusal guarantees and the same lack of
 * submission authorization apply. The six-slot functions remain limited to
 * one UINT8 input and one UINT8 output. */
int nbg_assembly_layout(const struct nbg_model *, struct nbg_assembly_layout *);
int nbg_assemble_layout(const struct nbg_model *, const struct nbg_assembly_buffer *, size_t count,
                        struct nbg_assembly_layout *);

/* Stepwise form of nbg_assemble_layout for a loader that writes into
 * memory no device or other task uses while it runs, in steps of bounded
 * length. The file must be the authenticated copy of one that
 * nbg_assembly_layout accepted at admission: begin repeats the profile and
 * placement checks but not the checksum pass, which the authentication
 * supersedes. begin writes nothing; it returns the plan, without the patch
 * counts, and a cursor at the first operation.
 *
 * Each next call validates and writes up to `operations` operations: the
 * command prefix before the first one, each NN descriptor in place and each
 * command state after the previous one. After the last operation it checks
 * that every patch was consumed, writes the footer, fills in the patch counts
 * and returns 1; it returns 0 while operations remain and -1 on refusal.
 * cursor.offset is then the number of command bytes written from the start
 * of the command allocation. Unlike nbg_assemble_layout it writes nothing
 * else: coefficients, END pairs after the stream, padding and the tensor and
 * pool allocations are the caller's, and a refusal can leave earlier
 * operations written, so the caller discards the whole assembly. */
struct nbg_assembly_cursor { uint32_t operation, patch, offset; };
int nbg_assembly_begin(const struct nbg_model *, const struct nbg_assembly_buffer *, size_t count,
                       struct nbg_assembly_layout *, struct nbg_assembly_cursor *);
int nbg_assembly_next(const struct nbg_model *, const struct nbg_assembly_buffer *,
                      struct nbg_assembly_layout *, struct nbg_assembly_cursor *, unsigned operations);
#endif
