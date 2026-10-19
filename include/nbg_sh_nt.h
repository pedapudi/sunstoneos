#ifndef SUNSTONE_NBG_SH_NT_H
#define SUNSTONE_NBG_SH_NT_H
#include "nbg_assembly.h"
/* Fixed FP32 relative-shift assembly profile. Slot1 is PPU instructions, not
 * coefficients; all other slot numbers match the existing six-slot plan.
 * The native wrapper authenticates the complete build-time catalogue artifact
 * before constructing a device command. Assembly alone authorizes no execution.
 * Caller lifetime, alias, aligned allocation and stable-input requirements are
 * those of nbg_assemble. Refusal preserves outputs. Full hardware access extents
 * remain an independent admission condition. */
enum { NBG_SH_NT_NN, NBG_SH_NT_PPU, NBG_SH_NT_POOL, NBG_SH_NT_COMMAND,
       NBG_SH_NT_INPUT, NBG_SH_NT_OUTPUT, NBG_SH_NT_BUFFERS };
int nbg_sh_nt_requirements(const struct nbg_model *,struct nbg_assembly_plan *);
int nbg_sh_nt_assemble(const struct nbg_model *,
    const struct nbg_assembly_buffer[NBG_SH_NT_BUFFERS],struct nbg_assembly_plan *);
#endif
