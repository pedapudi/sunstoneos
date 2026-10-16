#ifndef SUNSTONE_PVR_COMPUTE_CONTEXT_H
#define SUNSTONE_PVR_COMPUTE_CONTEXT_H
#include "pvr_firmware.h"
#include "pvr_cccb.h"
#include "pvr_gpu_vm.h"

#define PVR_COMPUTE_CONTEXT_BYTES 232u
#define PVR_COMPUTE_STATE_BYTES 4u
#define PVR_COMPUTE_SWITCH_REGISTERS 7u

enum pvr_compute_region { PVR_COMPUTE_CONTEXT, PVR_COMPUTE_STATE,
    PVR_COMPUTE_CONTROL, PVR_COMPUTE_COMMANDS, PVR_COMPUTE_VM, PVR_COMPUTE_REGIONS };
/* Firmware address and CPU view of one admitted object. CPU byte storage may
 * be unaligned; addresses use the A733 MIPS 16 MiB heap at 0xc0000000. These
 * logical spans do not prove page ownership, physical alias freedom or mapping. */
struct pvr_compute_buffer { void *cpu; size_t bytes; uint32_t address; };
struct pvr_compute_config {
    uint64_t bvnc;
    uint32_t priority, context_id, owner_id;
    /* Published order: context PDS0/PDS1, terminate PDS/PDS1, resume PDS0,
     * context PDS0 B, resume PDS0 B. These are trusted producer register images,
     * not GPU virtual addresses or an application command stream. */
    uint64_t switch_registers[PVR_COMPUTE_SWITCH_REGISTERS];
};

/* Build only an exclusively owned, unattached A733 compute context and its
 * four-byte firmware register state. Exact sizes: context 232, state 4,
 * control 32, VM 32; command ring is a power of two from 256 to 32768 bytes.
 * Firmware addresses require eight-byte alignment, except state needs four.
 * Every CPU span and firmware span must be disjoint from the other objects;
 * CPU spans must also be disjoint from both input record spans. Keep all inputs
 * stable. The VM record must contain an aligned, nonzero, 36-bit page catalogue
 * address outside 0x1fc00000..0x1fc03fff, UINT32_MAX register marker and
 * otherwise zero bytes, as produced by
 * pvr_gpu_vm_build. Priority 0/1/2 means low/normal/high; privileged admission
 * owns priority permission and unique context/owner identities, including zero.
 *
 * On success, zero the complete context/state and write the source ABI fields.
 * Leave control, ring and VM untouched. Returns 0 with every byte unchanged on
 * refusal. The operation is bounded by five descriptors and 236 output bytes.
 * It performs no callbacks, allocation, publication, attachment or device I/O.
 * Format control separately with pvr_cccb_init/poll, using this context address.
 *
 * Platform admission must own the backing and reserved firmware heap ranges,
 * establish CPU/physical/firmware correspondences, and exclude physical aliases.
 * It must also validate and retain the PDS store/resume/terminate programs,
 * their data and GPU context-store buffers referenced by switch_registers.
 * PDS generation, feature-specific register semantics, VM permissions and GPU
 * address admission remain trusted-producer requirements. Successful byte
 * construction is not permission to execute or evidence of functional programs.
 *
 * Keep firmware objects and all referenced resources pinned through separate
 * publication, attachment, job completion and context cleanup protocols. A
 * KICK return does not establish completion or safe reuse. This routine must
 * never overwrite a device-visible context or firmware-owned register state. */
int pvr_compute_context_build(const struct pvr_compute_config *,
                              const struct pvr_compute_buffer[PVR_COMPUTE_REGIONS]);
#endif
