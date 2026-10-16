#ifndef SUNSTONE_A733_SPI_PLATFORM_NATIVE_H
#define SUNSTONE_A733_SPI_PLATFORM_NATIVE_H
#include "a733_spi_platform.h"
struct a733_platform_locks;
struct a733_spi_platform_native {
    struct a733_spi_platform_ops operations;
    struct a733_platform_locks *locks;
    void *owner_task,*admission_cookie;
    int (*admit)(void *,const struct a733_spi_platform_config *,
        const struct a733_clock_token *,enum a733_spi_platform_admission);
    unsigned port,initialized;
};
/* Actual selected-controller aperture; FIFO addresses require byte access. */
int a733_spi_platform_native_address(unsigned port,uintptr_t,unsigned writing,unsigned width);
/* No MMIO. Binds real FreeRTOS task identity and the common scheduling-aware
 * register mutex. admit authenticates boot/electrical authority independently
 * of the actual clock/pin managers. All accesses require identity Device maps.
 * The same lock instance must back the pin manager. */
int a733_spi_platform_native_init(struct a733_spi_platform_native *,
    struct a733_platform_locks *,void *owner_task,unsigned port,
    int (*admit)(void *,const struct a733_spi_platform_config *,
        const struct a733_clock_token *,enum a733_spi_platform_admission),void *admission_cookie);
#endif
