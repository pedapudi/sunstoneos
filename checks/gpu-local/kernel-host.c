/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t uint2 __attribute__((ext_vector_type(2)));
typedef uint32_t uint4 __attribute__((ext_vector_type(4)));
static _Thread_local unsigned lane;
static unsigned group_x,group_y,rows,columns,depth,barrier_calls[32];
static pthread_barrier_t task_barrier;
static uint32_t arguments[5];
static float input[4096],output[4096];
static unsigned writes[4096];
void cs_queue_gemm_common(void);
size_t local_id(unsigned dim) __asm__("_Z12get_local_idj");
size_t group_id(unsigned dim) __asm__("_Z12get_group_idj");
void local_barrier(unsigned flags) __asm__("_Z7barrierj");
float fp_fma(float a,float b,float c) __asm__("_Z3fmafff");
size_t local_id(unsigned dim){return dim==0?lane%4:dim==1?lane/4:0;}
size_t group_id(unsigned dim){return dim==0?group_x:dim==1?group_y:0;}
float fp_fma(float a,float b,float c){return fmaf(a,b,c);}
void local_barrier(unsigned flags)
{
    assert(flags==1);++barrier_calls[lane];
    int rc=pthread_barrier_wait(&task_barrier);
    assert(!rc||rc==PTHREAD_BARRIER_SERIAL_THREAD);
}
static uintptr_t address(uint2 p){return p[0]|((uintptr_t)p[1]<<32);}
uint32_t host_vtxin(uint32_t offset)
{
    uintptr_t p=offset<2?(uintptr_t)arguments:(uintptr_t)(output+16);
    assert(offset<4);return (uint32_t)(p>>(32*(offset&1)));
}
uint2 nir_uadd64_32(uint32_t low,uint32_t high,uint32_t offset)
{
    uintptr_t p=((uintptr_t)high<<32|low)+offset;
    return (uint2){(uint32_t)p,(uint32_t)(p>>32)};
}
uint32_t host_dma1(uint2 p)
{
    uintptr_t a=address(p);
    assert(a==(uintptr_t)(arguments+4)||(a>=(uintptr_t)input&&a+4<=(uintptr_t)(input+rows*depth+depth*columns)));
    assert(!(a&3));uint32_t value;memcpy(&value,(void *)a,4);return value;
}
uint4 host_dma4(uint2 p)
{assert(address(p)==(uintptr_t)arguments);uint4 value;memcpy(&value,arguments,16);return value;}
void host_store(uint2 p,uint32_t value)
{
    uintptr_t a=address(p);assert(a>=(uintptr_t)(output+16)&&a+4<=(uintptr_t)(output+16+rows*columns));
    unsigned at=(unsigned)((a-(uintptr_t)output)/4);assert(!writes[at]);writes[at]=1;memcpy((void *)a,&value,4);
}
static void *work_item(void *arg){lane=(unsigned)(uintptr_t)arg;cs_queue_gemm_common();return NULL;}
static float left(unsigned row,unsigned k,unsigned sequence)
{return (float)((int)((row*3+k+sequence)%7)-3);}
static float right(unsigned k,unsigned col,unsigned sequence)
{return (float)((int)((k+col*5+sequence*2)%9)-4);}
static void run(unsigned m,unsigned n,unsigned k,unsigned sequence)
{
    rows=m;columns=n;depth=k;assert(m*k+k*n<=4096&&m*n+32<=4096);
    uintptr_t p=(uintptr_t)input;arguments[0]=(uint32_t)p;arguments[1]=(uint32_t)(p>>32);
    arguments[2]=m;arguments[3]=n;arguments[4]=k;
    for(unsigned i=0;i<m;++i)for(unsigned t=0;t<k;++t)input[i*k+t]=left(i,t,sequence);
    for(unsigned t=0;t<k;++t)for(unsigned j=0;j<n;++j)input[m*k+t*n+j]=right(t,j,sequence);
    for(unsigned i=0;i<4096;++i)output[i]=NAN;
    memset(writes,0,sizeof(writes));
    unsigned barriers=0;
    for(group_y=0;group_y<(m+7)/8;++group_y)for(group_x=0;group_x<(n+3)/4;++group_x) {
        memset(barrier_calls,0,sizeof(barrier_calls));pthread_t threads[32];
        assert(!pthread_barrier_init(&task_barrier,NULL,32));
        for(unsigned l=0;l<32;++l)assert(!pthread_create(&threads[l],NULL,work_item,(void *)(uintptr_t)l));
        for(unsigned l=0;l<32;++l){assert(!pthread_join(threads[l],NULL));assert(barrier_calls[l]==
#ifdef SUNSTONE_GPU_GEMM_CONTROL
        0
#else
        2*((k+7)/8)
#endif
        );}
        assert(!pthread_barrier_destroy(&task_barrier));barriers+=barrier_calls[0];
    }
    for(unsigned i=0;i<m;++i)for(unsigned j=0;j<n;++j) {
        float expected=0;
        for(unsigned t=0;t<k;++t)expected=fmaf(left(i,t,sequence),right(t,j,sequence),expected);
        assert(writes[16+i*n+j]==1&&output[16+i*n+j]==expected);
    }
    for(unsigned i=0;i<4096;++i)if(i<16||i>=16+m*n)assert(!writes[i]&&isnan(output[i]));
    printf("PASS unchanged OpenCL kernel %ux%ux%u sequence %u, %u workgroup barriers\n",m,n,k,sequence,barriers);
}
int main(void)
{
    run(1,1,1,1);run(8,4,8,1);run(16,12,16,1);run(3,7,0,1);
    for(unsigned seq=1;seq<=4;++seq)run(13,11,19,seq);
    run(9,5,7,2);return 0;
}
