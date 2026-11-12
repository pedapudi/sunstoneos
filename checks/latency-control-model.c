#include <stdint.h>

/* platform/qemu-smp/platform.c maps the virt GICv3 redistributors at this
 * base with a 128 KiB stride. Supply the same runtime lookup used on A733;
 * the model does not represent the A733 redistributor discovery protocol. */
struct { uint64_t epoch, startup_deadline, diagnostic_deadline, redistributor, reserved[12]; }
a733_cached_runtime[8] = {
    [0] = {.redistributor = 0x080a0000},
    [1] = {.redistributor = 0x080c0000},
    [2] = {.redistributor = 0x080e0000},
    [3] = {.redistributor = 0x08100000},
    [4] = {.redistributor = 0x08120000},
    [5] = {.redistributor = 0x08140000},
    [6] = {.redistributor = 0x08160000},
    [7] = {.redistributor = 0x08180000}
};
