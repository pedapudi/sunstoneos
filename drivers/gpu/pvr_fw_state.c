/* SPDX-License-Identifier: MIT
 * Firmware initialization semantics adapted from pvr_fw.c, pvr_fw_trace.c and
 * pvr_rogue_fwif.h. Copyright (c) 2023 Imagination Technologies Ltd.
 * See LICENSE.pvr for retained terms.
 */
#include "pvr_fw_state.h"
#include "a733_range.h"
#include "pvr_ccb.h"
#include <string.h>

static const struct pvr_fw_resource_layout layouts[PVR_FW_RESOURCE_COUNT] = {
    {0xc0100000, PVR_CCB_CONTROL_BYTES, 4096},
    {0xc0101000, PVR_CCB_HOST_SLOTS * PVR_CCB_COMMAND_BYTES, 12288},
    {0xc0104000, PVR_CCB_HOST_SLOTS * 4, 4096},
    {0xc0105000, PVR_CCB_CONTROL_BYTES, 4096},
    {0xc0106000, PVR_CCB_FIRMWARE_SLOTS * PVR_CCB_COMMAND_BYTES, 4096},
    {0xc0107000, 4, 4096},
    {0xc0108000, 2336, 4096},
    {0xc0109000, 4, 4096},
    {0xc010a000, 3624, 4096},
    {0xc010b000, 4096, 4096},
    {0xc010c000, 10280, 12288},
    {0xc010f000, 72, 4096},
    {0xc0110000, 864, 4096},
    {0xc0111000, 48000, 49152},
    {0xc011d000, 48000, 49152},
    {0xc0129000, 584, 4096},
    {0xc0fd0000, 16, 4096},
    {0xc0fe0000, 104, 4096},
    {0xc0ff0000, 544, 4096}
};
_Static_assert(PVR_FW_RESOURCE_COUNT == 19, "firmware resource layout");
_Static_assert((int)PVR_FW_EVENTS == (int)PVR_CCB_FIRMWARE_COMMANDS, "queue resource IDs");

const struct pvr_fw_resource_layout *pvr_fw_state_layout(enum pvr_fw_resource id)
{
    return (unsigned)id < PVR_FW_RESOURCE_COUNT ? &layouts[id] : NULL;
}
static int separate_outputs(const struct pvr_mips_buffer *b, const struct pvr_mips_object *out,
                            const void *input, size_t n)
{
    if (!a733_span_excl_nonempty(input, n) ||
        !a733_extents_disjoint_prechecked((uintptr_t)out, sizeof(*out) * PVR_FW_RESOURCE_COUNT, (uintptr_t)input, n)) return 0;
    for (unsigned i = 0; i < PVR_FW_RESOURCE_COUNT; ++i)
        if (!a733_extents_disjoint_prechecked((uintptr_t)b[i].storage.cpu, b[i].storage.bytes, (uintptr_t)input, n)) return 0;
    return 1;
}
static int physical_page(uint64_t p)
{
    return p && !(p & 4095) && p < (UINT64_C(1) << 36) &&
           !(p >= UINT64_C(0x1fc00000) && p < UINT64_C(0x1fc04000));
}
static void put(void *buffer, uint32_t offset, uint64_t value, unsigned n)
{
    unsigned char *p = (unsigned char *)buffer + offset;
    for (unsigned i = 0; i < n; ++i) p[i] = (unsigned char)(value >> (i * 8));
}

int pvr_fw_state_prepare(const struct pvr_mips_buffer b[PVR_FW_RESOURCE_COUNT],
                         const struct pvr_fw_state_config *config,
                         struct pvr_mips_object out[PVR_FW_RESOURCE_COUNT])
{
    if (!a733_span_excl_nonempty(b, sizeof(*b) * PVR_FW_RESOURCE_COUNT) ||
        !a733_span_excl_nonempty(config, sizeof(*config)) ||
        !a733_span_excl_nonempty(out, sizeof(*out) * PVR_FW_RESOURCE_COUNT) ||
        config->bvnc != PVR_FW_A733_BVNC || !config->gpu_clock_hz ||
        config->clusters != 1 || config->slc_kilobytes != 16) return 0;
    for (unsigned i = 0; i < PVR_FW_RESOURCE_COUNT; ++i) {
        if (b[i].storage.bytes != layouts[i].allocation_bytes ||
            b[i].page_count != layouts[i].allocation_bytes / 4096 ||
            !a733_span_excl_nonempty(b[i].storage.cpu, b[i].storage.bytes) ||
            !a733_extents_disjoint_prechecked((uintptr_t)b[i].storage.cpu, b[i].storage.bytes, (uintptr_t)out, sizeof(*out) * PVR_FW_RESOURCE_COUNT))
            return 0;
        for (unsigned j = 0; j < i; ++j)
            if (!a733_extents_disjoint_prechecked((uintptr_t)b[i].storage.cpu, b[i].storage.bytes, (uintptr_t)b[j].storage.cpu, b[j].storage.bytes))
                return 0;
    }
    if (!separate_outputs(b, out, b, sizeof(*b) * PVR_FW_RESOURCE_COUNT) ||
        !separate_outputs(b, out, config, sizeof(*config))) return 0;
    for (unsigned i = 0; i < PVR_FW_RESOURCE_COUNT; ++i) {
        if (!separate_outputs(b, out, b[i].pages, b[i].page_count * sizeof(uint64_t))) return 0;
        for (uint32_t page = 0; page < b[i].page_count; ++page) {
            uint64_t address = b[i].pages[page];
            if (!physical_page(address)) return 0;
            for (unsigned j = 0; j <= i; ++j) {
                uint32_t end = i == j ? page : b[j].page_count;
                for (uint32_t k = 0; k < end; ++k)
                    if (address == b[j].pages[k]) return 0;
            }
        }
    }
    /* All checks precede writes, including zeroing and mapping publication. */
    for (unsigned i = PVR_FW_POWER_SYNC; i < PVR_FW_RESOURCE_COUNT; ++i)
        memset(b[i].storage.cpu, 0, b[i].storage.bytes);
    for (unsigned offset = 0; offset < 4096; offset += 4)
        put(b[PVR_FW_FAULT_PAGE].storage.cpu, offset, 0xdeadbee0, 4);

    void *os = b[PVR_FW_OS_INIT].storage.cpu;
    for (unsigned i = 0; i < 5; ++i) put(os, i * 4, layouts[i].address, 4);
    put(os, 28, layouts[PVR_FW_RECOVERY_INFO].address, 4);
    put(os, 36, layouts[PVR_FW_OS_DATA].address, 4);
    put(os, 40, 3, 4); /* Hardware compatibility record layout version. */
    put(os, 56, 3, 4); /* Firmware compatibility record layout version. */
    put(b[PVR_FW_OS_DATA].storage.cpu, 572, layouts[PVR_FW_POWER_SYNC].address, 4);

    void *system = b[PVR_FW_SYSTEM_INIT].storage.cpu;
    put(system, 0, b[PVR_FW_FAULT_PAGE].pages[0], 8);
    put(system, 8, UINT64_C(0xda00000000), 8); /* PDS GPU execution heap. */
    put(system, 16, UINT64_C(0xe000000000), 8); /* USC GPU execution heap. */
    put(system, 316, layouts[PVR_FW_RUNTIME_CONFIG].address, 4);
    put(system, 320, layouts[PVR_FW_TRACE_CONTROL].address, 4);
    put(system, 324, layouts[PVR_FW_SYSTEM_DATA].address, 4);
    put(system, 328, layouts[PVR_FW_UTILIZATION].address, 4);
    put(system, 344, config->gpu_clock_hz, 4);
    put(system, 356, 1, 4);

    void *runtime = b[PVR_FW_RUNTIME_CONFIG].storage.cpu;
    put(runtime, 8, 1, 1); /* Persistent active-power-management latency setting. */
    put(runtime, 12, config->gpu_clock_hz, 4);
    put(runtime, 16, config->clusters, 4);
    /* The 16 KiB SLC allows DM overlap; initial system config_flags stays zero.
     * Firmware watchdog/performance counters/logging remain disabled, matching
     * the reference initializer. Independent board watchdog protection is separate.
     */
    void *trace = b[PVR_FW_TRACE_CONTROL].storage.cpu;
    put(trace, 856, 12000, 4);
    for (unsigned i = 0; i < 2; ++i) {
        put(trace, 12 + i * 424, layouts[PVR_FW_TRACE_0 + i].address, 4);
        put(trace, 16 + i * 424, (uint64_t)(uintptr_t)b[PVR_FW_TRACE_0 + i].storage.cpu, 8);
    }
    for (unsigned i = 0; i < PVR_FW_RESOURCE_COUNT; ++i)
        out[i] = (struct pvr_mips_object){b[i].pages, b[i].page_count,
                                        layouts[i].address, PVR_MIPS_UNCACHED};
    return 1;
}
