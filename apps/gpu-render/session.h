#ifndef SUNSTONE_GPU_APPLICATION_SESSION_H
#define SUNSTONE_GPU_APPLICATION_SESSION_H
#include "boot.h"
#include "a733_board_application.h"
#include "pvr_compute_package.h"
#if defined(SUNSTONE_GPU_RUNTIME_TEXTURE)
#include "pvr_render_texture.h"
struct gpu_texture_session {
    struct a733_board_session base;
    uint64_t magic,version,bytes,width,height;
};
#define GPU_APPLICATION_SESSION_BYTES sizeof(struct gpu_texture_session)
int gpu_application_texture_geometry(uint32_t *,uint32_t *);
unsigned gpu_application_texture_profile(void);
#elif defined(SUNSTONE_GPU_PROGRAM_APPLICATION)
struct gpu_program_session {
    struct a733_board_session base;
    uint64_t magic,version,bytes,package_bytes;
    uint8_t package_digest[32];
    struct pvr_compute_package package;
};
#define GPU_APPLICATION_SESSION_BYTES sizeof(struct gpu_program_session)
const struct pvr_compute_package *gpu_application_package(void);
#else
#define GPU_APPLICATION_SESSION_BYTES sizeof(struct a733_board_session)
#endif
#define A733_GPU_SUPERVISED_TICKS (UINT64_C(60)*A733_SMP_COUNTER_HZ)
struct gpu_boot_contract {struct a733_smp_contract base;uint8_t session_digest[32];
#ifdef SUNSTONE_GPU_SUPERVISED
    uint8_t entry_digest[32];
#endif
};
#ifdef SUNSTONE_GPU_SUPERVISED
const uint8_t *gpu_application_entry_digest(const struct a733_smp_contract *);
#endif
const uint8_t *gpu_application_contract_digest(const struct a733_smp_contract *);
const struct a733_board_session *gpu_application_session(void);
int gpu_application_seal_session(const struct a733_smp_contract *);
#endif
