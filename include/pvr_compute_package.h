#ifndef SUNSTONE_PVR_COMPUTE_PACKAGE_H
#define SUNSTONE_PVR_COMPUTE_PACKAGE_H
#include <stddef.h>
#include <stdint.h>
#define PVR_COMPUTE_PACKAGE_MAGIC UINT32_C(0x50435650)
#define PVR_COMPUTE_PACKAGE_CODE 4032u
#define PVR_COMPUTE_PACKAGE_INPUT 4096u
#define PVR_COMPUTE_PACKAGE_OUTPUT 3968u
/* Trusted PCO usclib profile: 32 lanes, one group, two pointer arguments.
 * Input begins with LEu32 count. No kernel barriers, atomics, local/shared
 * memory, scratch, spilling, textures or library linkage. This is a producer
 * contract, not executable validation. Admission must establish source/build
 * provenance; hashes and this header cannot prove arbitrary USC behavior.
 * All integer fields are little endian on the wire. */
struct pvr_compute_package {
    uint32_t magic, version, bytes, profile;
    uint64_t bvnc;
    uint32_t code_bytes, input_bytes, output_bytes, count;
    uint32_t entry_offset, scratch, spilled_temps, unsupported;
    uint32_t local_size[3], reserved0;
    unsigned char precomp[8], source_digest[32], compiler_digest[32];
    uint32_t input_header_bytes, input_element_bytes, output_element_bytes, reserved;
    unsigned char code[PVR_COMPUTE_PACKAGE_CODE], input[PVR_COMPUTE_PACKAGE_INPUT];
};
_Static_assert(offsetof(struct pvr_compute_package,code)==160,"package wire header");
_Static_assert(sizeof(struct pvr_compute_package)==8288,"package wire size");
int pvr_compute_package_valid(const struct pvr_compute_package *);
unsigned pvr_compute_package_temps(const struct pvr_compute_package *);
#endif
