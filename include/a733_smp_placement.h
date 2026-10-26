/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_A733_SMP_PLACEMENT_H
#define SUNSTONE_A733_SMP_PLACEMENT_H
/* Core roles of the cached eight-core images, as preprocessor constants for
 * xTaskCreateStaticAffinitySet and configTASK_DEFAULT_CORE_AFFINITY
 * (docs/designs/smp-heterogeneous.md, docs/designs/cpu-a76-dvfs.md). Cores
 * 0-5 are Cortex-A55 and cores 6-7 Cortex-A76; cached entry checks MIDR_EL1
 * once. Every task without an explicit placement stays on the A55 cluster.
 * Recognition and its first worker run on the A76 pair once
 * A733_A76_ADMITTED is defined, and on A55 cores 2 and 3 until then. The
 * admitted eight-worker OS composition adds priority-2 inference workers on
 * cores 0-5, below the fixed service tasks, without moving those services. The
 * shared-peripheral interrupts and the PMIC task are on core 0; other
 * unplaced tasks may run on any A55 core, including the recognition pair. */
#define A733_SMP_A55_MASK 0x3Fu
#ifdef A733_A76_ADMITTED
#define A733_SMP_RECOGNITION_CORE 6u
#define A733_SMP_INFERENCE_CORE 7u
#else
#define A733_SMP_RECOGNITION_CORE 2u
#define A733_SMP_INFERENCE_CORE 3u
#endif
#ifdef configNUMBER_OF_CORES
#if (A733_SMP_A55_MASK >> configNUMBER_OF_CORES) || (A733_SMP_RECOGNITION_CORE >= configNUMBER_OF_CORES) || \
    (A733_SMP_INFERENCE_CORE >= configNUMBER_OF_CORES)
#error "A733 core roles name a core the scheduler does not start"
#endif
#endif
#endif
