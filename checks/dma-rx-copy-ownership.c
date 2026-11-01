/* CPU staging ownership only; DMA completion and cache effects are modeled. */
#include "a733_dma_controller.h"
#include "a733_dma_regs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { PERIODS=4, BYTES=64 };
static struct a733_dma_controller controller;
static struct a733_dma_transfer transfers[2];
static struct a733_dma_segment segments[2][PERIODS];
static struct a733_dma_mapping mappings[2][PERIODS];
static _Alignas(64) struct a733_dma_descriptor descriptors[2][PERIODS];
static _Alignas(64) unsigned char payload[2][PERIODS][BYTES];
/* Two neighboring allocations make exact endpoint comparisons observable. */
static _Alignas(64) unsigned char adjacent[2][BYTES];
static unsigned reads, caches, barriers, writes, validations, times;
static const uintptr_t base=0x04601000;

static uint32_t read_reg(void *unused,uintptr_t address)
{
    (void)unused;++reads;
    switch(address-base-A733_DMA_REG_CHANNELS) {
    case A733_DMA_REG_PACKAGES:return 2;
    case A733_DMA_REG_DESCRIPTOR:return (uint32_t)transfers[0].descriptors.bus+3*64;
    case A733_DMA_REG_DESTINATION:return (uint32_t)mappings[0][2].bus;
    case A733_DMA_REG_REMAINING:return BYTES;
    case A733_DMA_REG_PENDING:return 0;
    default:assert(0);return 0;
    }
}
static void write_reg(void *unused,uintptr_t address,uint32_t value)
{(void)unused;(void)address;(void)value;++writes;}
static void cache(void *unused,enum a733_dma_cache op,void *cpu,size_t bytes)
{
    (void)unused;assert(op==A733_DMA_INVALIDATE && cpu==payload[0][0] && bytes==BYTES);++caches;
}
static void barrier(void *unused){(void)unused;++barriers;}
static uint64_t ticks(void *unused){(void)unused;++times;return 1;}
static int mapping(void *unused,const struct a733_dma_mapping *m)
{(void)unused;(void)m;++validations;return 1;}
static int fifo(void *unused,const struct a733_dma_endpoint *e,enum a733_dma_access access)
{(void)unused;(void)e;(void)access;++validations;return 1;}

static void setup(void)
{
    memset(&controller,0,sizeof(controller));
    for(unsigned ch=0;ch<2;++ch) {
        for(unsigned i=0;i<PERIODS;++i) {
            uint64_t bus=0x45000000+ch*4096+i*BYTES;
            memset(payload[ch][i],0x31+ch+i,BYTES);
            mappings[ch][i]=(struct a733_dma_mapping){payload[ch][i],bus,BYTES,
                ch?A733_DMA_READ:A733_DMA_WRITE};
            segments[ch][i]=(struct a733_dma_segment){
                {ch?bus:0x02032020,ch?0:3,4,8},
                {ch?0x02032020:bus,ch?3:0,4,8},BYTES};
        }
        transfers[ch]=(struct a733_dma_transfer){
            {descriptors[ch],0x44000000+ch*4096,sizeof(descriptors[ch]),A733_DMA_DESCRIPTORS},
            segments[ch],PERIODS,mappings[ch],PERIODS};
    }
    const struct a733_dma_operations ops={read_reg,write_reg,cache,barrier,ticks,mapping,fifo};
    assert(a733_dma_controller_init(&controller,&ops,NULL,base,3,100)==A733_DMA_CONTROL_OK);
    /* Enter after admitted submission. Registers and completion are modeled;
     * this fixture tests only output admission and the actual copy path. */
    for(unsigned ch=0;ch<2;++ch)controller.channels[ch]=(struct a733_dma_channel){
        .claimed=1,.transfer=&transfers[ch],.cyclic=ch?1:2,.status=A733_DMA_ACTIVE,
        .timeout=100,.package_last=2,.packages=2};
    reads=caches=barriers=writes=validations=times=0;
}

static void rejected(void *out,size_t bytes)
{
    struct a733_dma_controller before=controller;
    unsigned before_callbacks=reads+caches+barriers+writes+validations+times;
    assert(a733_dma_cyclic_rx_copy(&controller,0,0,out,bytes)==-1);
    assert(!memcmp(&controller,&before,sizeof(before)));
    assert(reads+caches+barriers+writes+validations+times==before_callbacks);
}

int main(void)
{
    setup();
    struct a733_dma_transfer saved_transfers[2];
    struct a733_dma_segment saved_segments[2][PERIODS];
    struct a733_dma_mapping saved_mappings[2][PERIODS];
    unsigned char saved_payload[sizeof(payload)],saved_descriptors[sizeof(descriptors)];
    memcpy(saved_transfers,transfers,sizeof(transfers));
    memcpy(saved_segments,segments,sizeof(segments));memcpy(saved_mappings,mappings,sizeof(mappings));
    memcpy(saved_payload,payload,sizeof(payload));memcpy(saved_descriptors,descriptors,sizeof(descriptors));
    unsigned cases=0;
    for(unsigned ch=0;ch<2;++ch) {
        rejected(&transfers[ch],BYTES);++cases;
        rejected(segments[ch],BYTES);++cases;
        rejected(mappings[ch],BYTES);++cases;
        rejected(descriptors[ch],BYTES);++cases;
        rejected((unsigned char *)descriptors[ch]+sizeof(descriptors[ch])-1,BYTES);++cases;
        for(unsigned i=0;i<PERIODS;++i){rejected(payload[ch][i],BYTES);++cases;}
    }
    for(unsigned state=A733_DMA_STOPPING;state<=A733_DMA_FAILED;++state) {
        controller.channels[1].status=(enum a733_dma_control_status)state;
        rejected(payload[1][0],BYTES);++cases;
    }
    controller.channels[1].status=A733_DMA_RECOVERY;
    rejected(payload[1][0],BYTES);++cases;
    rejected(&controller,BYTES);++cases;
    rejected((void *)(UINTPTR_MAX-BYTES+1),BYTES);++cases;
    rejected(adjacent[1],0);++cases;
    assert(!memcmp(saved_transfers,transfers,sizeof(transfers)));
    assert(!memcmp(saved_segments,segments,sizeof(segments)));
    assert(!memcmp(saved_mappings,mappings,sizeof(mappings)));
    assert(!memcmp(saved_payload,payload,sizeof(payload)));
    assert(!memcmp(saved_descriptors,descriptors,sizeof(descriptors)));
    /* A read-only peer allocation ending exactly at output is disjoint. */
    mappings[1][0].cpu=adjacent[0];
    memset(adjacent,0xa5,sizeof(adjacent));
    assert(a733_dma_cyclic_rx_copy(&controller,0,0,adjacent[1],BYTES)==1);
    assert(!memcmp(adjacent[1],payload[0][0],BYTES));
    for(unsigned i=0;i<BYTES;++i)assert(adjacent[0][i]==0xa5);
    assert(caches==1 && barriers==2 && writes==0 && validations==0 && times==2 && reads==15);
    printf("PASS %u DMA output refusals preserve leases and callbacks; adjacent staging copies one period\n",cases);
}
