/* The size and field offsets of struct nbg_assembly_layout as the C compiler lays it out, for host tools that mirror the
 * structure (tools/npu-session-catalogue.py). Built only into host libraries, never into an image. */
#include "nbg_assembly.h"
#include <stddef.h>

unsigned long nbg_layout_abi(unsigned which)
{
    switch (which) {
    case 0: return sizeof(struct nbg_assembly_layout);
    case 1: return sizeof(struct nbg_allocation_requirement);
    case 2: return offsetof(struct nbg_assembly_layout, buffers);
    case 3: return offsetof(struct nbg_assembly_layout, inputs);
    case 4: return offsetof(struct nbg_assembly_layout, outputs);
    case 5: return offsetof(struct nbg_assembly_layout, nn_operations);
    case 6: return offsetof(struct nbg_assembly_layout, descriptor_patches);
    case 7: return offsetof(struct nbg_assembly_layout, state_patches);
    case 8: return offsetof(struct nbg_assembly_layout, command_body_bytes);
    case 9: return offsetof(struct nbg_assembly_layout, dynamic);
    case 10: return offsetof(struct nbg_assembly_layout, program_offset);
    case 11: return offsetof(struct nbg_assembly_layout, program_bytes);
    case 12: return offsetof(struct nbg_assembly_layout, capacity);
    default: return (unsigned long)-1;
    }
}
