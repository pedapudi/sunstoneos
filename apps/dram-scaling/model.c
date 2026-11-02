/* SPDX-License-Identifier: GPL-2.0-only */
/* Host-only producer: runs the same fill pattern, chunk order and read kernel as the image
 * on a host buffer and writes a result with synthetic durations. It exercises the result
 * layout and the digests; its durations prove no board result. */
#include "result.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void)
{
    static struct ds_result r;
    uint64_t *buffer = aligned_alloc(64, DS_BUFFER_BYTES);
    if (!buffer) return 1;
    memcpy(r.magic, DS_MAGIC, 8);
    r.counter_hz = 24000000;
    r.buffer_base = 0x80000000u;
    r.window_ticks = DS_WINDOW_TICKS;
    r.warm = DS_WARM;
    r.reps = DS_REPS;
    r.readiness = 15;
    for (unsigned s = 0; s < DS_CORES; ++s) {
        uint64_t digest = 0;
        for (size_t i = 0; i < DS_SLOT_BYTES / 8; ++i) {
            const uint64_t w = ds_word((uint64_t)s * (DS_SLOT_BYTES / 8) + i);
            buffer[(size_t)s * (DS_SLOT_BYTES / 8) + i] = w;
            digest ^= w;
        }
        r.fill_digest[s] = digest;
    }
    snprintf(r.clock_before, sizeof r.clock_before, "CLOCK cpu_l_mhz=1794 cpu_b_mhz=2002 cpu_l_mv=1000 cpu_b_mv=1050 transitions=3 dsu_mhz=1274 sid=12340400 bin=0 core=6 pattern=0 parked_max_us=40\n");
    memcpy(r.clock_after, r.clock_before, sizeof r.clock_before);
    static const struct ds_variant variants[DS_CONFIGS] = DS_VARIANTS;
    for (unsigned c = 0; c < DS_CONFIGS; ++c) {
        struct ds_config *config = &r.config[c];
        config->mask = variants[c].mask;
        config->mode = variants[c].mode;
        config->stagger = variants[c].stagger;
        const uint32_t bytes = ds_chunk_bytes(config->mode);
        for (unsigned rep = 0; rep < DS_REPS; ++rep)
            for (unsigned core = 0; core < DS_CORES; ++core) {
                if (!(config->mask >> core & 1u)) continue;
                const uint32_t chunks = 3000 + 7 * c + 13 * core + 5 * rep;
                config->sample[rep][core].chunks = chunks;
                config->sample[rep][core].ticks = DS_WINDOW_TICKS + 100 * core + rep;
                if (rep != DS_REPS - 1) continue;
                const uint64_t *region = buffer + (size_t)core * (DS_SLOT_BYTES / 8)
                                         + ds_region_offset(core, config->stagger) / 8;
                uint64_t digest = 0;
                for (uint32_t j = 0; j < chunks; ++j)
                    digest ^= (config->mode == 4 ? ds_read_pf : ds_read)(region + (size_t)ds_chunk_index(config->mode, j) * (bytes / 8), bytes);
                config->digest[core] = digest;
            }
    }
    return fwrite(&r, sizeof r, 1, stdout) != 1;
}
