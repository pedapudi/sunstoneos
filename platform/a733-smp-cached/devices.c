#include "a733_cached_devices.h"
#include "aarch64_device_memory.h"

static struct a733_cached_devices devices;
static unsigned preparation;
extern unsigned char __device_pool_start[];

static int image_span(uintptr_t address, size_t bytes)
{
    if (!bytes || address < 0x41000000 || address >= (uintptr_t)__device_pool_start ||
        bytes > (uintptr_t)__device_pool_start - address) return 0;
    uintptr_t last = (address + bytes - 1) & ~(uintptr_t)4095;
    for (uintptr_t page = address & ~(uintptr_t)4095; ; page += 4096) {
        struct aarch64_translation t;
        if (!aarch64_device_translate(page, 0, &t) || t.physical != page ||
            t.attributes != 0xff || t.shareability != 3) return 0;
        if (page == last) return 1;
    }
}

int a733_cached_devices_prepare(const struct a733_cached_device_inputs *in)
{
    if (!a733_smp_device_preparation_admitted() || __atomic_load_n(&preparation, __ATOMIC_RELAXED) || !in ||
        ((uintptr_t)in % _Alignof(struct a733_cached_device_inputs)) ||
        !image_span((uintptr_t)in, sizeof(*in)) || !in->fatal ||
        !image_span((uintptr_t)in->fatal, 4) ||
        (in->fatal_cookie && !image_span((uintptr_t)in->fatal_cookie, 1)) ||
        !image_span((uintptr_t)in->npu_model, in->npu_model_bytes) ||
        !image_span((uintptr_t)in->gpu_firmware, in->gpu_firmware_bytes) ||
        !in->npu_owner || !in->gpu_owner || !in->compute_owner ||
        in->npu_owner == in->gpu_owner || in->npu_owner == in->compute_owner ||
        in->gpu_owner == in->compute_owner || !in->gpu_clock_hz) return 0;
    /* Primary alone owns this transition. Failure leaves preparation retained
     * at 1 and every successfully acquired constructor parent unreleasable. */
    __atomic_store_n(&preparation, 1, __ATOMIC_RELAXED);
    if (!a733_npu_reserved_prepare(&devices.npu, in->npu_owner, in->npu_model,
            in->npu_model_bytes, 0x03600000, in->fatal, in->fatal_cookie) ||
        !a733_gpu_reserved_prepare(&devices.gpu, in->gpu_owner, in->gpu_firmware,
            in->gpu_firmware_bytes, in->gpu_clock_hz) ||
        !a733_gpu_compute_reserved_prepare(&devices.compute, &devices.gpu,
            &devices.gpu_service, in->compute_owner) ||
        !a733_smp_device_preparation_admitted()) return 0;
    __atomic_store_n(&preparation, 2, __ATOMIC_RELEASE);
    return 1;
}

struct a733_cached_devices *a733_cached_devices_prepared(void)
{
    return __atomic_load_n(&preparation, __ATOMIC_ACQUIRE) == 2 ? &devices : NULL;
}
