#ifndef SUNSTONE_A733_SPI_NATIVE_H
#define SUNSTONE_A733_SPI_NATIVE_H
#include "a733_spi.h"

/* Immutable, privileged platform binding. lease identifies an authenticated
 * reservation owned by the platform; authenticate must check its generation,
 * controller/selected-CS route, parent clock, rail, recovery and interrupt
 * ownership. No production reservation provider is implemented by this file.
 * A caller-supplied ready flag does not establish these prerequisites.
 * The provider and its registry must be protected from application writes.
 * Native accesses require identity-mapped Device-nGnRnE SPI/CCU/GIC regions. */
struct a733_spi_native {
    struct a733_spi_format format;
    struct a733_spi_plan plan;
    uint64_t lease;
    void *provider;
    int (*authenticate)(void *, const struct a733_spi_native *);
    int (*now_us)(void *, uint64_t *);
};
/* Populate before use; retain immutable binding/context for the controller
 * lifetime. Returns -1 without MMIO for an invalid plan or absent provider.
 * The native admit operation checks actual CCU clock, reset, GIC enable and
 * active state on each state-changing public call. It never enables clocks,
 * changes shared resets, selects pins, changes security or clears interrupts. */
int a733_spi_native_ops(const struct a733_spi_native *, struct a733_spi_ops *);
#endif
