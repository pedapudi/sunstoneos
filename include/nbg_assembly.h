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
struct nbg_assembly_buffer {
    void *cpu;
    size_t bytes;
    uint32_t device;
};

/* Offline assembly experiment for the observed single-core A733 NN profile.
 * Requires an immutable successful nbg_model_open view and file. Supports one
 * UINT8 input/output, INIT followed by 1..1024 sequential 192-byte NN records,
 * the observed load-state templates, and patch sources 0/1/2/3/5/8. Other
 * layouts are refused. Full profile validation precedes publishing requirements.
 * Minimum sizes reproduce observed SDK padding; allocation sizes round to 4 KiB
 * and reserve the controller's complete command fetch. Total <= 256 MiB.
 * Returns 1 on success, 0 on refusal. Result must not overlap view or file.
 * Refusal leaves result unchanged. No allocation, MMIO, cache, or locking.
 *
 * SUCCESS IS NOT SUBMISSION AUTHORIZATION. Descriptor data references check
 * one addressed byte only; full access extents, checksum, SRAM mapping, and
 * descriptor semantics remain unverified. No controller token is produced. */
int nbg_assembly_requirements(const struct nbg_model *, struct nbg_assembly_plan *);

/* Supply six exclusively owned offline buffers in enum order, with exact
 * planned sizes, 4 KiB aligned CPU/device bases, nonoverlapping CPU/device
 * ranges, and no device overlap with SRAM 0x400000..0x47ffff. Physical aliases
 * and ownership are the caller's responsibility; this layer cannot check them.
 * Metadata/result/view/file must not overlap output buffers; result must also
 * not overlap metadata, view, or file. Keep all inputs stable during the call.
 *
 * Validates every operation and relocation before changing any caller bytes.
 * Initializes all six allocations, including input/output and padding; supply
 * input data AFTER assembly. Adapts the SDK completion event to event 0 and
 * fills unused fetch space with END pairs. Refusal preserves buffers and result.
 * Reuses bounded stack scratch per operation; no heap or borrowed output spans.
 * Returns the same plan as requirements. Never submit these bytes until the
 * separate admission and platform requirements above are resolved. */
int nbg_assemble(const struct nbg_model *,
                 const struct nbg_assembly_buffer[NBG_ALLOCATION_COUNT],
                 struct nbg_assembly_plan *);
#endif
