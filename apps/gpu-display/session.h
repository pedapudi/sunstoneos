#ifndef SUNSTONE_GPU_DISPLAY_SESSION_H
#define SUNSTONE_GPU_DISPLAY_SESSION_H
#include "boot.h"
#include "../hdmi/session.h"
#include "input.h"
#include "usb-authority.h"
#define GPU_DISPLAY_SESSION_MAGIC UINT64_C(0x4153373347444131)
#define GPU_DISPLAY_CONTRACT_MAGIC UINT64_C(0x4153373347444331)
struct gpu_display_session {
    uint64_t magic,version,bytes;
    struct hdmi_session hdmi;
    uint8_t gpu_initial[32],gpu_memory[32],gpu_electrical[32],gpu_supervision[32];
    uint64_t memory_authority;
    struct gpu_display_usb_authority usb;
    struct gpu_display_inputs inputs;
};
struct gpu_display_boot_contract {
    struct a733_smp_contract base;
    uint8_t session_digest[32],entry_digest[32];
};
_Static_assert(sizeof(struct gpu_display_boot_contract)==288,"display boot record");
_Static_assert(offsetof(struct gpu_display_session,inputs)%8==0,"aligned input envelope");
_Static_assert(offsetof(struct gpu_display_session,usb)==704,"USB authority offset");
_Static_assert(offsetof(struct gpu_display_session,inputs)==840,"observation input offset");
const struct gpu_display_boot_contract *gpu_display_boot_record(const struct a733_smp_contract *);
int gpu_display_session_valid(const struct gpu_display_session *);
int gpu_display_session_prepare(const struct a733_smp_contract *);
#endif
