#include "counter_clock.h"
#include "aarch64_device_memory.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const uint32_t frequencies[] = {1, 3, 19200000, 24000000, 62500000, UINT32_MAX};
    const uint64_t deltas[] = {0, 1, 1, 17, 999999, UINT32_MAX, (UINT64_C(1) << 55) - 1};
    const uint64_t mask = (UINT64_C(1) << 56) - 1;
    for (unsigned f = 0; f < sizeof(frequencies) / sizeof(frequencies[0]); ++f) {
        struct counter_clock c = {0};
        uint64_t raw = mask - 50;
        __uint128_t elapsed = 0;
        assert(counter_clock_start(&c, frequencies[f], raw));
        for (unsigned round = 0; round < 4; ++round)
            for (unsigned d = 0; d < sizeof(deltas) / sizeof(deltas[0]); ++d) {
                elapsed += deltas[d]; raw += deltas[d];
                assert(counter_clock_sample(&c, raw));
                assert(c.microseconds == (uint64_t)(elapsed * 1000000 / frequencies[f]));
                assert(c.remainder == (uint64_t)(elapsed * 1000000 % frequencies[f]));
            }
        struct counter_clock saved = c;
        assert(!counter_clock_sample(&c, raw - 1));
        assert(!memcmp(&c, &saved, sizeof(c)));
        assert(!counter_clock_sample(&c, raw + (UINT64_C(1) << 55)));
        assert(!memcmp(&c, &saved, sizeof(c)));
        assert(!counter_clock_start(&c, frequencies[f], raw));
        assert(!memcmp(&c, &saved, sizeof(c)));
    }
    struct counter_clock c = {0};
    assert(!counter_clock_start(&c, 0, 0) && !counter_clock_sample(&c, 1));
    assert(counter_clock_start(&c, 3, 0));
    c.microseconds = UINT64_MAX - 1;
    assert(counter_clock_sample(&c, 3) && c.microseconds == 999998);
    assert(aarch64_cache_stride_from_ctr((4u << 24) | (4u << 16)) == 64);
    assert(aarch64_cache_stride_from_ctr((4u << 24) | (3u << 16)) == 32);
    assert(!aarch64_cache_stride_from_ctr(4u << 16));
    assert(!aarch64_cache_stride_from_ctr((5u << 24) | (4u << 16)));
    assert(!aarch64_cache_stride_from_ctr((3u << 24) | (4u << 16)));
    puts("PASS fractional counter conversion, counter/time wrap, backward refusal, and cache ownership geometry");
    return 0;
}
