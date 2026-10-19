#include "a733_dma.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static _Alignas(64) struct a733_dma_descriptor descriptors[4],saved[4];
static uint32_t word(unsigned slot,unsigned at)
{const uint8_t *p=descriptors[slot].bytes+at;return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
int main(void)
{
    struct a733_dma_segment segments[4];
    for(unsigned receive=0;receive<2;++receive){
        for(unsigned i=0;i<4;++i){struct a733_dma_endpoint ram={UINT64_C(0x200000000)+i*4096,0,2,8},fifo={0x02032020,3,2,8};
            segments[i]=(struct a733_dma_segment){receive?fifo:ram,receive?ram:fifo,4096};}
        assert(a733_dma_build_cyclic(descriptors,4,0x41000000,segments,4)==A733_DMA_OK);
        for(unsigned i=0;i<4;++i){assert(word(i,20)==0x41000000+((i+1)%4)*64);assert(word(i,12)==4096);
            assert((word(i,16)&(7u<<(receive?15:11)))==(2u<<(receive?15:11)));}
        memcpy(saved,descriptors,sizeof(saved));segments[3].bytes=2048;
        assert(a733_dma_build_cyclic(descriptors,4,0x41000000,segments,4)==A733_DMA_FORMAT);
        assert(!memcmp(saved,descriptors,sizeof(saved)));segments[3].bytes=4096;
        segments[3].source.request=segments[3].destination.request=0;
        assert(a733_dma_build_cyclic(descriptors,4,0x41000000,segments,4)==A733_DMA_FORMAT);
        assert(!memcmp(saved,descriptors,sizeof(saved)));
    }
    puts("PASS immutable cyclic DMA encoding: RX/TX links, high data bits, fixed period/fifo and unchanged output on refusal");return 0;
}
