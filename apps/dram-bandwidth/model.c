/* SPDX-License-Identifier: GPL-2.0-only */
/* Host-only producer: runs the same fill pattern and read kernel as the image on
 * a host buffer and writes a result with synthetic durations. It exercises the
 * result layout and the digests; its durations prove no board result. */
#include "result.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void)
{
    static struct bw_result r;
    uint64_t *buffer = aligned_alloc(64, BW_BUFFER_BYTES);
    if (!buffer) return 1;
    memcpy(r.magic, BW_MAGIC, 8);
    r.counter_hz = 24000000;
    r.slice_bytes = BW_SLICE_BYTES;
    r.slices = BW_SLICES;
    r.warm = BW_WARM;
    r.reps = BW_REPS;
    r.readiness = 15;
    for (unsigned s = 0; s < BW_SLICES; ++s) {
        uint64_t digest = 0;
        for (size_t i = 0; i < BW_SLICE_BYTES / 8; ++i) {
            const uint64_t w = bw_word((uint64_t)s * (BW_SLICE_BYTES / 8) + i);
            buffer[(size_t)s * (BW_SLICE_BYTES / 8) + i] = w;
            digest ^= w;
        }
        r.fill_digest[s] = digest;
    }
    snprintf(r.clock_before, sizeof r.clock_before, "CLOCK cpu_l_mhz=1794 cpu_b_mhz=2002 cpu_l_mv=1000 cpu_b_mv=1050 transitions=3 dsu_mhz=1274 sid=12340400 bin=0 core=6 pattern=0 parked_max_us=40\n");
    memcpy(r.clock_after, r.clock_before, sizeof r.clock_before);
    static const uint32_t masks[BW_CONFIGS] = BW_CONFIG_MASKS;
    for (unsigned c = 0; c < BW_CONFIGS; ++c) {
        r.config[c].mask = masks[c];
        r.config[c].cores = (uint32_t)__builtin_popcount(masks[c]);
        for (unsigned rep = 0; rep < BW_REPS; ++rep) {
            const unsigned pass = BW_WARM + rep + 1;
            uint64_t digest = 0;
            for (unsigned rank = 0; rank < r.config[c].cores; ++rank)
                digest ^= bw_read(buffer + (size_t)((rank + pass) % BW_SLICES) * (BW_SLICE_BYTES / 8), BW_SLICE_BYTES);
            r.config[c].samples[rep].ticks = 100000 + 1000 * rep + c;
            r.config[c].samples[rep].digest = digest;
        }
    }
    return fwrite(&r, sizeof r, 1, stdout) != 1;
}
