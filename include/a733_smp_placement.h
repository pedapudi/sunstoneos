/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_A733_SMP_PLACEMENT_H
#define SUNSTONE_A733_SMP_PLACEMENT_H
/* Default application placement in cached eight-core images, as preprocessor
 * constants for xTaskCreateStaticAffinitySet and configTASK_DEFAULT_CORE_AFFINITY
 * (docs/designs/smp-heterogeneous.md, docs/designs/cpu-a76-dvfs.md). Cores
 * 0-5 are Cortex-A55 and cores 6-7 Cortex-A76; cached entry checks MIDR_EL1
 * once. Every task without explicit placement stays on the A55 cluster.
 * An A76 application's main task and first worker use the A76 pair; the
 * manifest's cluster chooses it at run time (apps/os/application.c). Shared
 * peripheral interrupts and the PMIC task stay on core 0. Other unplaced
 * tasks may run on any A55 core, including the default application pair. */
#define A733_SMP_A55_MASK 0x3Fu
#define A733_SMP_APPLICATION_CORE 6u
#define A733_SMP_APPLICATION_WORKER_CORE 7u
#ifdef configNUMBER_OF_CORES
#if (A733_SMP_A55_MASK >> configNUMBER_OF_CORES) || (A733_SMP_APPLICATION_CORE >= configNUMBER_OF_CORES) || \
    (A733_SMP_APPLICATION_WORKER_CORE >= configNUMBER_OF_CORES)
#error "A733 application placement names a core the scheduler does not start"
#endif
#endif
#endif
