#ifndef SUNSTONE_PVR_COMPUTE_MODE_H
#define SUNSTONE_PVR_COMPUTE_MODE_H
#include <stdint.h>
#define PVR_COMPUTE_MODE_SYSTEM 0xf308u
#define PVR_COMPUTE_MODE_CAPABILITIES 0xf300u
/* Architectural register-bank spacing; it is not permission to map additional
 * banks on A733. The published physical GPU aperture is 0x90000 bytes. */
#define PVR_COMPUTE_MODE_BANK_STRIDE 0x80000u
#define PVR_COMPUTE_MODE_MAX_CORES 4u

enum pvr_compute_mode_phase { PVR_COMPUTE_MODE_NEW, PVR_COMPUTE_MODE_COUNTED,
    PVR_COMPUTE_MODE_ADMITTED, PVR_COMPUTE_MODE_FAILED };
enum pvr_compute_mode_error { PVR_COMPUTE_MODE_OK, PVR_COMPUTE_MODE_IDENTITY,
    PVR_COMPUTE_MODE_LEASE, PVR_COMPUTE_MODE_COUNT,
    PVR_COMPUTE_MODE_MULTICORE_REQUIRED, PVR_COMPUTE_MODE_CAPABILITY,
    PVR_COMPUTE_MODE_ORDER };
struct pvr_compute_mode {
    uint64_t identity, token, epoch, capabilities;
    uint32_t cores, phase, error;
};
/* Observations belong to one powered lifecycle. Count is a 32-bit read;
 * capabilities is a 64-bit read, admitted only after count proves MC1.
 * Failure preserves observations for diagnosis and permanently faults this
 * observation. Only a new power lifecycle may initialize zero storage again.
 * These functions do no MMIO and establish no power or mapping ownership. */
int pvr_compute_mode_count(struct pvr_compute_mode *, uint64_t identity,
    uint64_t token, uint64_t epoch, uint32_t count);
int pvr_compute_mode_capabilities(struct pvr_compute_mode *, uint64_t capabilities);
/* Caller proves the current power state and exclusive lifecycle ownership.
 * Reserved capability bits are ignored as in the pinned hardware driver. */
int pvr_compute_mode_valid(const struct pvr_compute_mode *, uint64_t identity,
    uint64_t token, uint64_t epoch);
#endif
