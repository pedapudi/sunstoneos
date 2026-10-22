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
#define NBG_ASSEMBLY_MAX_TENSORS 8u
#define NBG_LAYOUT_MAX_BUFFERS (NBG_ALLOC_INPUT + 2u * NBG_ASSEMBLY_MAX_TENSORS)
struct nbg_assembly_layout {
    struct nbg_allocation_requirement buffers[NBG_LAYOUT_MAX_BUFFERS];
    uint32_t inputs, outputs;
    uint32_t nn_operations, descriptor_patches, state_patches, command_body_bytes;
};
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
#endif
