#ifndef SUNSTONE_OS_CPU_RETURN_SPI_H
#define SUNSTONE_OS_CPU_RETURN_SPI_H
#include <stdint.h>
/* The distributor state of one routed interrupt before the OS runs, saved for the CPU return (apps/os/cpu-return-spi.c). */
struct os_return_irq {
    uint64_t route;
    uint32_t group,modifier,config,priority,valid,intid;
};
/* Generated with the interrupt routes: one record for each, in the EL3 monitor's memory. */
extern struct os_return_irq os_return_irq_saved[];
extern const unsigned os_return_irq_capacity;
#endif
