/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_A733_SMP_PLACEMENT_H
#define SUNSTONE_A733_SMP_PLACEMENT_H
/* Core roles of the cached eight-core images, as preprocessor constants for
 * xTaskCreateStaticAffinitySet and configTASK_DEFAULT_CORE_AFFINITY
 * (docs/designs/smp-heterogeneous.md, docs/designs/cpu-a76-dvfs.md). Cores
 * 0-5 are Cortex-A55 and cores 6-7 Cortex-A76; cached entry checks MIDR_EL1
 * once. Service tasks stay on the A55 cluster, normally core 0. Recognition
 * and its inference worker run on the A76 pair once A733_A76_ADMITTED is
 * defined, and on A55 cores 2 and 3 until then. The A76 mask exists only with
 * admission, so a use without it fails to compile. */
#define A733_SMP_A55_MASK 0x3Fu
#define A733_SMP_SERVICE_CORE 0u
#ifdef A733_A76_ADMITTED
#define A733_SMP_A76_MASK 0xC0u
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
