/* hello-compute: the smallest application on the SunstoneOS interface
 * (docs/designs/app-abi.md). It computes the Adler-32 checksum (RFC 1950)
 * of its one input region and returns it with ss_finish. It uses no device.
 * Status: compiles against include/sunstone_app.h and runs on the host
 * model in checks/sunstone-app-abi.py; no board image links it yet. */
#include "hello_compute-app.h"

/* The result that ss_finish returns, little-endian. */
struct hello_compute_result {
    uint32_t adler32;
    uint32_t reserved;
    uint64_t bytes;
};
_Static_assert(sizeof(struct hello_compute_result) == 16, "hello-compute result is the manifest's output_limit");

/* 5552 is the largest count for which the sums cannot overflow 32 bits
 * before the modular reduction (RFC 1950, zlib NMAX). Twelve blocks are
 * about 64 KiB, so progress is reported far inside the 1 s window. */
#define BLOCK 5552u
#define BLOCKS_PER_PROGRESS 12u

void hello_compute_main(const struct ss_grant *grant)
{
    const struct ss_span *input = &grant->regions[HELLO_COMPUTE_REGION_INPUT];
    const uint8_t *p = input->base;
    size_t left = input->bytes;
    uint32_t a = 1, b = 0;
    for (unsigned block = 0; left; ++block) {
        size_t n = left < BLOCK ? left : BLOCK;
        for (size_t i = 0; i < n; ++i) {
            a += p[i];
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
        p += n;
        left -= n;
        if (block % BLOCKS_PER_PROGRESS == 0) {
            if (ss_live() < 0) ss_fail(1);
            ss_progress();
        }
    }
    const struct hello_compute_result result = {b << 16 | a, 0, input->bytes};
    ss_finish(&result, sizeof(result));
}
