#ifndef SUNSTONE_GPU_APPLICATION_SESSION_H
#define SUNSTONE_GPU_APPLICATION_SESSION_H
#include "boot.h"
#include "a733_board_application.h"
struct gpu_boot_contract {struct a733_smp_contract base;uint8_t session_digest[32];};
const uint8_t *gpu_application_contract_digest(const struct a733_smp_contract *);
int gpu_application_seal_session(const struct a733_smp_contract *);
#endif
