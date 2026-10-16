#ifndef SUNSTONE_A733_CACHED_DEVICES_H
#define SUNSTONE_A733_CACHED_DEVICES_H
#include "a733_npu_reserved.h"
#include "a733_gpu_compute_reserved.h"

struct a733_cached_device_inputs {
    const void *npu_model;
    size_t npu_model_bytes;
    const void *gpu_firmware;
    size_t gpu_firmware_bytes;
    uint32_t gpu_clock_hz;
    uint32_t npu_owner, gpu_owner, compute_owner;
    void (*fatal)(void *);
    void *fatal_cookie;
};
struct a733_cached_devices {
    struct a733_npu_reserved npu;
    struct a733_gpu_reserved gpu;
    struct a733_gpu_compute_reserved compute;
    struct pvr_gpu_service gpu_service;
};
/* Platform-owned immutable inputs, included in the image build identity.
 * An opt-in image supplies this object and links the actual constructors.
 * Config, firmware/model and callback storage remain image-owned for its life.
 */
extern const struct a733_cached_device_inputs a733_cached_device_inputs;
/* Actual platform barrier state: all eight cores completed cache adoption and
 * the pre-scheduler barrier, primary release has not started, primary EL1 only.
 * This is software ordering, not physical coherence or recovery qualification.
 */
int a733_smp_device_preparation_admitted(void);
/* Called once by the primary before task creation. Full parent leases remain
 * reserved on success or failure. No entropy session, hardware startup, command,
 * reset or drain provider is supplied. Failed preparation cannot retry.
 * Other pool users must remain idle here and serialize all later mutations.
 */
int a733_cached_devices_prepare(const struct a733_cached_device_inputs *);
/* Acquire publication of successful offline preparation. Objects remain private
 * and noncopyable; GPU/NPU service APIs retain their existing startup contracts.
 */
struct a733_cached_devices *a733_cached_devices_prepared(void);
#endif
