#ifndef CPU_NPU_GEMM_H
#define CPU_NPU_GEMM_H
#include <stdint.h>
struct cpu_npu_matrix { volatile float a[16][16],b[16][16],c[16][16]; };
static void cpu_npu_matrix_init(struct cpu_npu_matrix *m,unsigned core)
{
    for(unsigned r=0;r<16;++r)for(unsigned s=0;s<16;++s){
        m->a[r][s]=(float)(r+core+2+s);m->b[r][s]=(float)((int)r-(int)s);
    }
}
/* Return the first bad cell, preserving its exact float32 operands. */
static int cpu_npu_gemm(struct cpu_npu_matrix *m,unsigned core,uint64_t *digest,
                        unsigned *cell,float *got,float *expected)
{
    for(unsigned r=0;r<16;++r)for(unsigned s=0;s<16;++s)m->c[r][s]=1048576.0f;
    for(unsigned r=0;r<16;++r)for(unsigned s=0;s<16;++s){
        float sum=0.0f;
        for(unsigned k=0;k<16;++k)sum=sum+m->a[r][k]*m->b[k][s];
        m->c[r][s]=sum;
    }
    uint64_t unit=0;
    for(unsigned r=0;r<16;++r)for(unsigned s=0;s<16;++s){
        const float actual=m->c[r][s];
        const float want=(float)(((int)r+(int)core+2)*(120-16*(int)s)+1240-120*(int)s);
        if(actual!=want){*cell=16*r+s;*got=actual;*expected=want;return 0;}
        unit+=(uint64_t)((int64_t)(1+16*r+s)*(int64_t)actual);
    }
    *digest+=unit;return 1;
}
#endif
