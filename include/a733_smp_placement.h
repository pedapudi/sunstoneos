/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_A733_SMP_PLACEMENT_H
#define SUNSTONE_A733_SMP_PLACEMENT_H
/* Core roles of the cached eight-core images, as preprocessor constants for
 * xTaskCreateStaticAffinitySet and configTASK_DEFAULT_CORE_AFFINITY
 * (docs/designs/smp-heterogeneous.md, docs/designs/cpu-a76-dvfs.md). Cores
 * 0-5 are Cortex-A55 and cores 6-7 Cortex-A76; cached entry checks MIDR_EL1
 * once. Every task without an explicit placement stays on the A55 cluster.
 * Recognition and its inference worker run on the A76 pair once
 * A733_A76_ADMITTED is defined; the GPU linear build's recognizer runs on A55
 * core 2 until then, and the inference core exists only with admission. */
#define A733_SMP_A55_MASK 0x3Fu
#ifdef A733_A76_ADMITTED
#define A733_SMP_RECOGNITION_CORE 6u
#define A733_SMP_INFERENCE_CORE 7u
#else
#define A733_SMP_RECOGNITION_CORE 2u
#endif
#ifdef configNUMBER_OF_CORES
#if (A733_SMP_A55_MASK >> configNUMBER_OF_CORES) || (A733_SMP_RECOGNITION_CORE >= configNUMBER_OF_CORES)
#error "A733 core roles name a core the scheduler does not start"
#endif
#if defined(A733_SMP_INFERENCE_CORE) && A733_SMP_INFERENCE_CORE >= configNUMBER_OF_CORES
#error "A733 core roles name a core the scheduler does not start"
#endif
#endif
#endif
