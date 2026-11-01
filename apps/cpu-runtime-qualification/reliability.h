#ifndef CPU_RELIABILITY_H
#define CPU_RELIABILITY_H
#include <stdint.h>
#define REL_CORES 8u
#ifndef REL_ROUNDS
#define REL_ROUNDS 4096u
#endif
#define REL_WORDS 64u
#define REL_MAGIC UINT64_C(0x31594c5243505543)
struct reliability_result {
    uint64_t magic, rounds, core_mask, elapsed_ticks;
    struct { uint64_t rounds, digest, waits; } cpu[REL_CORES];
};
_Static_assert(sizeof(struct reliability_result)==224,"reliability output");
/* A different value for every producer, round and payload position. */
static inline uint64_t reliability_word(unsigned core,unsigned round,unsigned word)
{
    uint64_t x=((uint64_t)(core+1)<<48)^((uint64_t)round<<16)^word;
    x^=x>>30;x*=UINT64_C(0xbf58476d1ce4e5b9);
    x^=x>>27;x*=UINT64_C(0x94d049bb133111eb);
    return x^(x>>31);
}
#endif
