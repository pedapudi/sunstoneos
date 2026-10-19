#ifndef SUNSTONE_GPU_FINITE_H
#define SUNSTONE_GPU_FINITE_H
/* Device adapter only. The common returning platform still owns authenticated
 * session retention, memory mappings, scheduler stop and safe RAM restoration. */
int gpu_finite_bind_pmic(void);
void gpu_finite_pmic_checkpoint(void);
_Noreturn void gpu_finite_complete(void);
#endif
