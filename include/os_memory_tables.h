#ifndef SUNSTONE_OS_MEMORY_TABLES_H
#define SUNSTONE_OS_MEMORY_TABLES_H
/* A virtual address in the unmapped hole below 2 GiB. Only the kernel's EL1
 * table pages are mapped here, write-back, EL1-only and execute-never. */
#define OS_KERNEL_TABLE_ALIAS_BASE 0x79e00000
#ifndef __ASSEMBLER__
#include <stdint.h>
uint64_t *os_kernel_l1_mutable(void);
uint64_t *os_kernel_l2_mutable(void);
#endif
#endif
