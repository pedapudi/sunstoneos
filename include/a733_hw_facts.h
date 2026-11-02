/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SUNSTONE_A733_HW_FACTS_H
#define SUNSTONE_A733_HW_FACTS_H
/* The hardware facts table (docs/hardware/a733-facts.md).
 *
 * A fixed binary layout that the facts probe fills at EL1 on every core before the scheduler starts
 * and that the facts application returns as its result. tools/hw_facts.py describes the same layout
 * and decodes it; checks/hw-facts.py compares the two. Little-endian. The header is 64 bytes; then
 * HWF_SHARED_SLOTS slots read once by core 0; then HWF_CORES blocks of HWF_CORE_SLOTS slots, one
 * block per core. A slot is 16 bytes. */
#include <stddef.h>
#include <stdint.h>

#define HWF_MAGIC UINT64_C(0x3146574833373341) /* "A733HWF1" */
#define HWF_VERSION 1u
#define HWF_CORES 8u
#define HWF_CORE_SLOTS 64u
#define HWF_SHARED_SLOTS 96u
#define HWF_PLATFORM_A733 1u
#define HWF_PLATFORM_QEMU_OS 2u
/* Slot flags. */
#define HWF_VALID 1u    /* the register was read */
#define HWF_SKIPPED 2u  /* a precondition for a safe read was not met; no read was made */
#define HWF_ANOMALY 4u  /* the value on entry did not read back after the restoring write */

struct hwf_slot {
    uint16_t id;
    uint8_t core;
    uint8_t flags;
    uint32_t aux;   /* the value written, for the tests that write; otherwise zero */
    uint64_t value;
};
struct hwf_header {
    uint64_t magic;
    uint32_t version, bytes, platform, cores, shared_slots, core_slots, shared_offset, core_offset, core_stride;
    uint32_t done_mask; /* one bit per core that finished; set last, with release ordering */
    uint8_t reserved[16];
};
struct hwf_table {
    struct hwf_header header;
    struct hwf_slot shared[HWF_SHARED_SLOTS];
    struct hwf_slot core[HWF_CORES][HWF_CORE_SLOTS];
};
_Static_assert(sizeof(struct hwf_slot) == 16 && sizeof(struct hwf_header) == 64, "hardware facts layout");
_Static_assert(sizeof(struct hwf_table) == 64 + 16 * HWF_SHARED_SLOTS + 16 * HWF_CORES * HWF_CORE_SLOTS, "hardware facts layout");

/* What a board supplies to the capture: the distributor, the redistributor of a core, a word read for
 * device memory, the FreeRTOS API priority mask in ICC_PMR_EL1 form, and the platform number. */
struct hwf_platform {
    uint32_t platform;
    uintptr_t distributor;
    uintptr_t (*redistributor)(unsigned core);
    uint32_t (*read32)(uintptr_t address);
    uint32_t api_mask;
    uint32_t ppi30_priority;
};
extern const struct hwf_platform hwf_platform;
/* The table lives in the facts application (apps/hw-facts/main.c), which an application may read; the capture
 * in the OS core writes it. The application defines it with this initializer. */
#define HWF_TABLE_INITIALIZER {.header = { \
    HWF_MAGIC, HWF_VERSION, sizeof(struct hwf_table), 0, HWF_CORES, HWF_SHARED_SLOTS, HWF_CORE_SLOTS, \
    offsetof(struct hwf_table, shared), offsetof(struct hwf_table, core), sizeof(((struct hwf_table *)0)->core[0]), 0, {0}}}
extern struct hwf_table hwf_table;
/* EL1 on core `core`, before the scheduler. Reads registers only; the writes it makes
 * are listed in docs/hardware/a733-facts.md. */
void hwf_capture(unsigned core);
#endif
