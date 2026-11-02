/* SPDX-License-Identifier: GPL-2.0-only */
/* Host-only producer: exercises the result layout of the interconnect study with synthetic values. */
#include "result.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void)
{
    static struct dn_result r;
    uint64_t *buffer = aligned_alloc(64, DN_BUFFER_BYTES);
    if (!buffer) return 1;
    memcpy(r.magic, DN_MAGIC, 8);
    r.counter_hz = 24000000;
    r.buffer_base = 0x44000000u;
    r.window_ticks = DN_WINDOW_TICKS;
    r.warm = DN_WARM;
    r.reps = DN_REPS;
    r.readiness = 15;
    for (unsigned s = 0; s < DN_CORES; ++s) {
        uint64_t digest = 0;
        for (size_t i = 0; i < DN_SLOT_BYTES / 8; ++i) {
            const uint64_t w = dn_word((uint64_t)s * (DN_SLOT_BYTES / 8) + i);
            buffer[(size_t)s * (DN_SLOT_BYTES / 8) + i] = w;
            digest ^= w;
        }
        r.fill_digest[s] = digest;
    }
    snprintf(r.clock_before, sizeof r.clock_before, "CLOCK cpu_l_mhz=1794 cpu_b_mhz=2002 cpu_l_mv=1000 cpu_b_mv=1050 transitions=3 dsu_mhz=1274 sid=12340400 bin=0 core=6 pattern=0 parked_max_us=40\n");
    memcpy(r.clock_after, r.clock_before, sizeof r.clock_before);
    static const struct dn_variant variants[DN_CONFIGS] = DN_VARIANTS;
    for (unsigned phase = 0; phase < DN_PHASES; ++phase)
        for (unsigned c = 0; c < DN_CONFIGS; ++c) {
            struct dn_config *config = &r.config[phase][c];
            config->mask = variants[c].mask;
            config->mode = variants[c].mode;
            config->stagger = variants[c].stagger;
            const uint32_t bytes = dn_chunk_bytes(config->mode);
            for (unsigned rep = 0; rep < DN_REPS; ++rep) {
                config->counters[rep].ticks = 300;
                for (unsigned t = 0; t < 2; ++t) {
                    config->counters[rep].rq_rd[t] = 1000 + c; config->counters[rep].dt_rd[t] = 4000 + c;
                    config->counters[rep].la_rd[t] = 90000 + c;
                }
                for (unsigned core = 0; core < DN_CORES; ++core) {
                    if (!(config->mask >> core & 1u)) continue;
                    const uint32_t chunks = 3000 + 7 * c + 13 * core + 5 * rep;
                    config->sample[rep][core].chunks = chunks;
                    config->sample[rep][core].ticks = DN_WINDOW_TICKS + 100 * core + rep;
                    if (rep != DN_REPS - 1) continue;
                    uint64_t digest = 0;
                    if (config->mode == 5) {
                        const unsigned n = (unsigned)__builtin_popcount(config->mask);
                        const unsigned rank = (unsigned)__builtin_popcount(config->mask & ((1u << core) - 1u));
                        for (uint32_t j = 0; j < chunks; ++j)
                            digest ^= dn_read(buffer + (size_t)(((uint64_t)j * n + rank) % DN_SHARED_CHUNKS) * (bytes / 8), bytes);
                    } else {
                        const uint64_t *region = buffer + (size_t)core * (DN_SLOT_BYTES / 8)
                                                 + dn_region_offset(core, config->stagger) / 8;
                        for (uint32_t j = 0; j < chunks; ++j)
                            digest ^= dn_read(region + (size_t)dn_chunk_index(config->mode, j) * (bytes / 8), bytes);
                    }
                    config->digest[core] = digest;
                }
            }
        }
    static const uint32_t values[DN_WORDS] = DN_WORD_VALUES;
    for (unsigned i = 0; i < DN_WORDS; ++i) { r.probe.words_before[i] = i; r.probe.words_written[i] = values[i]; }
    r.probe.write_enabled = 1;
    return fwrite(&r, sizeof r, 1, stdout) != 1;
}
