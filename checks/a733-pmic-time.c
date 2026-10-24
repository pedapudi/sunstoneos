/* SPDX-License-Identifier: Apache-2.0 */
/* The PMIC native adapter's time read divides the counter by its ticks per
 * microsecond with a733_pmic_divide() (include/a733_pmic_native.h) instead
 * of a UDIV instruction. This check compares it with division for every
 * divisor from 1 to 4,096, the powers of two and their neighbours up to
 * 2^32, and 2^32 itself: at 0, 1, 2^64 - 1, around every power of two,
 * around many multiples of the divisor and at pseudo-random values. It also
 * requires the setup to refuse 0 and divisors above 2^32.
 * Build: clang -std=c11 -Wall -Wextra -Werror -Iinclude checks/a733-pmic-time.c */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "a733_pmic_native.h"

static uint64_t state = 0x9e3779b97f4a7c15u;
static uint64_t next(void)
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}
static unsigned long long checked;
static void one(uint64_t d, const struct a733_pmic_divisor *v, uint64_t n)
{
    assert(a733_pmic_divide(n, v) == n / d);
    ++checked;
}
static void divisor(uint64_t d)
{
    struct a733_pmic_divisor v;
    assert(a733_pmic_divisor_init(d, &v));
    const uint64_t edges[] = {0, 1, 2, 3, UINT64_MAX, UINT64_MAX - 1, UINT64_MAX - d};
    for (unsigned i = 0; i < sizeof edges / sizeof edges[0]; ++i)
        one(d, &v, edges[i]);
    for (unsigned b = 1; b < 64; ++b)
        for (int k = -2; k <= 2; ++k)
            one(d, &v, (UINT64_C(1) << b) + (uint64_t)(int64_t)k);
    for (unsigned i = 0; i < 64; ++i) {
        const uint64_t q = next() / d;
        for (int k = -1; k <= 1; ++k)
            one(d, &v, q * d + (uint64_t)(int64_t)k);
    }
    for (unsigned i = 0; i < 256; ++i)
        one(d, &v, next());
}
int main(void)
{
    struct a733_pmic_divisor v;
    assert(!a733_pmic_divisor_init(0, &v) && !a733_pmic_divisor_init((UINT64_C(1) << 32) + 1, &v));
    for (uint64_t d = 1; d <= 4096; ++d)
        divisor(d);
    for (unsigned b = 12; b < 32; ++b)
        for (int k = -1; k <= 1; ++k)
            divisor((UINT64_C(1) << b) + (uint64_t)(int64_t)k);
    divisor(UINT64_C(1) << 32);
    /* The board counter: 24 MHz, 24 ticks per microsecond. */
    assert(a733_pmic_divisor_init(24, &v) && a733_pmic_divide(UINT64_C(22019), &v) == 917);
    printf("PASS PMIC time division: %llu comparisons with integer division\n", checked);
    return 0;
}
