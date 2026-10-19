#ifndef SUNSTONE_PVR_COMPUTE_PACKAGE_H
#define SUNSTONE_PVR_COMPUTE_PACKAGE_H
#include <stddef.h>
#include <stdint.h>
#define PVR_COMPUTE_PACKAGE_MAGIC UINT32_C(0x50435650)
#define PVR_COMPUTE_PACKAGE_CODE 4032u
#define PVR_COMPUTE_PACKAGE_INPUT 4096u
#define PVR_COMPUTE_PACKAGE_OUTPUT 3968u
#define PVR_COMPUTE_DISPATCH_LANES 32u
#define PVR_COMPUTE_DISPATCH_PDS_BYTES 4096u
#define PVR_COMPUTE_DISPATCH_PDS_SLOT 128u
#define PVR_COMPUTE_DISPATCH_PDS_CODE (PVR_COMPUTE_DISPATCH_PDS_BYTES - 128u)
#define PVR_COMPUTE_DISPATCH_MAX (PVR_COMPUTE_DISPATCH_PDS_CODE / PVR_COMPUTE_DISPATCH_PDS_SLOT)
#define PVR_COMPUTE_DISPATCH_ELEMENTS (PVR_COMPUTE_DISPATCH_MAX * PVR_COMPUTE_DISPATCH_LANES)
/* Profile/version2 supplies input/output pointers and first-element/tile-count
 * in six explicit unified input registers. One direct CDM dispatch per tile,
 * one workgroup per dispatch; a single final fence/timeline covers them all.
 * Slots reserve80 PDS data bytes each; shared code uses28 of64 bytes followed
 * by64 zero bytes. Existing one-page backing and service interfaces suffice. */
/* Profile/version3 uses a real bounded grid and pinned compiler system values.
 * The32-byte geometry header replaces only version3's first32 code-storage
 * bytes: actual code starts at192 with capacity4000; input stays at4192.
 * Unified0..3 hold pointers, unified4 holds flat local index, coefficients0..2
 * hold workgroup IDs and4..6 hold group counts. Seven coefficients are not
 * OpenCL shared memory; shared registers and scratch remain unsupported. */
/* Trusted PCO usclib profiles:32 local lanes and two pointer arguments.
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
    uint32_t local_size[3], dispatch_count;
    unsigned char precomp[8], source_digest[32], compiler_digest[32];
    uint32_t input_header_bytes, input_element_bytes, output_element_bytes, tile_elements;
    union {
        unsigned char code[PVR_COMPUTE_PACKAGE_CODE];
        struct {
            uint32_t groups[3], global_extent[3], sysval_abi, reserved;
            unsigned char code[PVR_COMPUTE_PACKAGE_CODE-32];
        } grid;
    };
    unsigned char input[PVR_COMPUTE_PACKAGE_INPUT];
};
_Static_assert(offsetof(struct pvr_compute_package,code)==160,"package wire header");
_Static_assert(sizeof(struct pvr_compute_package)==8288,"package wire size");
_Static_assert(offsetof(struct pvr_compute_package,grid.code)==192,"grid code offset");
_Static_assert(offsetof(struct pvr_compute_package,input)==4192,"all profiles input offset");
const unsigned char *pvr_compute_package_code(const struct pvr_compute_package *);
/* Checked copied-input admission for immutable package shape. */
int pvr_compute_package_input_valid(const struct pvr_compute_package *,const void *,size_t);
int pvr_compute_package_valid(const struct pvr_compute_package *);
unsigned pvr_compute_package_temps(const struct pvr_compute_package *);
#endif
