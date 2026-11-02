/* SPDX-License-Identifier: GPL-2.0-only */
#include "result.h"
static void input(uint32_t *p)
{for(unsigned i=0;i<CAPACITY_WORDS;++i)p[i]=UINT32_C(0x12345678)^(i*UINT32_C(0x9e3779b9));}
static uint64_t digest(const uint32_t *p)
{
    uint64_t h=UINT64_C(0xcbf29ce484222325);
    for(unsigned i=0;i<CAPACITY_WORDS;++i)h=(h^p[i])*UINT64_C(0x100000001b3);
    return h;
}
__attribute__((noinline)) uint64_t capacity_integer(uint32_t *p)
{
    input(p);
    for(unsigned r=0;r<CAPACITY_ROUNDS;++r){
#ifdef __clang__
#pragma clang loop vectorize(disable) interleave(disable)
#endif
        for(unsigned i=0;i<CAPACITY_WORDS;++i){
            uint32_t x=p[i];x^=x>>13;x=x*UINT32_C(1664525)+UINT32_C(1013904223);p[i]=x;
        }
    }
    return digest(p);
}
__attribute__((noinline)) uint64_t capacity_neon(uint32_t *p)
{
    input(p);
    for(unsigned r=0;r<CAPACITY_ROUNDS;++r){
        for(unsigned i=0;i<CAPACITY_WORDS;i+=4){
#ifdef __aarch64__
            __asm__ volatile(
                ".arch_extension simd\n"
                "ld1 {v0.4s},[%0]\n"
                "ushr v1.4s,v0.4s,#13\n"
                "eor v0.16b,v0.16b,v1.16b\n"
                "dup v2.4s,%w1\n"
                "dup v3.4s,%w2\n"
                "mul v0.4s,v0.4s,v2.4s\n"
                "add v0.4s,v0.4s,v3.4s\n"
                "st1 {v0.4s},[%0]"
                ::"r"(p+i),"r"(UINT32_C(1664525)),"r"(UINT32_C(1013904223))
                :"v0","v1","v2","v3","memory");
#else
            for(unsigned j=0;j<4;++j){uint32_t x=p[i+j];x^=x>>13;p[i+j]=x*UINT32_C(1664525)+UINT32_C(1013904223);}
#endif
        }
    }
    return digest(p);
}
