/* Execute the production offline link routine and assembler on pinned models.
 * Platform scheduling and cache publication are modeled; no device executes. */
#include "npu_runtime.h"
#include "npu_admitted_models.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned cache_calls;
BaseType_t xTaskGetSchedulerState(void){return taskSCHEDULER_NOT_STARTED;}
int aarch64_device_cache(enum aarch64_cache_op op,uintptr_t cpu,size_t bytes)
{assert(op==AARCH64_CACHE_CLEAN&&cpu&&!(cpu&63)&&bytes&&!(bytes&63));++cache_calls;return 1;}
static uint32_t fixture_crc_byte(uint32_t crc,uint8_t value)
{crc^=value;for(unsigned i=0;i<8;++i)crc=(crc>>1)^((0u-(crc&1))&0x82f63b78u);return crc;}
static uint32_t fixture_crc_word(uint32_t crc,uint64_t value)
{for(unsigned i=0;i<8;++i){crc=fixture_crc_byte(crc,(uint8_t)value);value>>=8;}return crc;}
#include SUNSTONE_LINK_ROUTINES
static struct npu_runtime rt;
static void *owned[32];static unsigned owned_count;
static void *allocation(size_t bytes)
{void *p=aligned_alloc(4096,(bytes+4095)&~(size_t)4095);assert(p&&owned_count<32);memset(p,0,bytes);owned[owned_count++]=p;return p;}
static unsigned char *read_model(const char *path,size_t *bytes)
{FILE *f=fopen(path,"rb");assert(f&&!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>0&&!fseek(f,0,SEEK_SET));unsigned char *p=malloc((size_t)n);assert(p&&fread(p,1,(size_t)n,f)==(size_t)n&&!fclose(f));*bytes=(size_t)n;return p;}
static uint32_t word(const void *p){uint32_t v;memcpy(&v,p,4);return v;}
static void prepare(unsigned index,const unsigned char *model,size_t bytes)
{
    struct npu_runtime_slot *s=&rt.slot[index];struct nbg_model view;
    assert(admit(model,bytes,&rt.entry[index],&s->layout));
    rt.catalogue[index]=(struct npu_runtime_model){model,bytes};s->model=index;
    assert(nbg_model_open(model,bytes,&view)==NBG_DECODE_OK);
    unsigned count=NBG_ALLOC_INPUT+s->layout.inputs+s->layout.outputs;
    uint32_t device=0x10000000u+index*0x1000000u;
    for(unsigned i=0;i<count;++i){size_t n=s->layout.buffers[i].allocation_bytes;if(!n)continue;
        s->targets[i]=(struct nbg_assembly_buffer){allocation(n),n,device};device+=(uint32_t)n+4096;}
    /* NN and coefficient reservations are contiguous in the real runtime. */
    size_t nn=s->targets[NBG_ALLOC_NN].bytes,coeff=s->targets[NBG_ALLOC_COEFF].bytes;
    void *constant=allocation(nn+coeff);
    s->targets[NBG_ALLOC_NN].cpu=constant;s->targets[NBG_ALLOC_COEFF].cpu=(unsigned char *)constant+nn;
    s->targets[NBG_ALLOC_COEFF].device=s->targets[NBG_ALLOC_NN].device+(uint32_t)nn;
    s->constant=(struct npu_runtime_region){(uintptr_t)constant,0,nn+coeff,s->targets[NBG_ALLOC_NN].device};
    struct nbg_assembly_cursor cursor;struct nbg_assembly_layout layout;
    assert(nbg_assembly_begin(&view,s->targets,count,&layout,&cursor));
    int result;do{result=nbg_assembly_next(&view,s->targets,&layout,&cursor,8);}while(!result);assert(result==1);
    s->layout=layout;s->command=(struct npu_runtime_region){(uintptr_t)s->targets[NBG_ALLOC_COMMAND].cpu,0,s->targets[NBG_ALLOC_COMMAND].bytes,s->targets[NBG_ALLOC_COMMAND].device};
    s->stream_bytes=cursor.offset;fill_end(s->command.cpu+cursor.offset,s->command.bytes-cursor.offset);
    for(unsigned i=0;i<s->layout.inputs+s->layout.outputs;++i){struct npu_runtime_tensor *t=&s->tensors[i];
        assert(nbg_model_tensor(&view,i<s->layout.inputs?NBG_INPUT:NBG_OUTPUT,i<s->layout.inputs?i:i-s->layout.inputs,&t->tensor));
        t->data=s->targets[NBG_ALLOC_INPUT+i].cpu;t->bytes=t->tensor.logical_bytes;}
    s->crc=UINT32_MAX;while(!npu_runtime_reference_step(&rt,s)){}atomic_init(&s->state,NPU_RUNTIME_READY);
    assert(npu_runtime_stream_intact(&rt,s));
}
int main(int argc,char **argv)
{
    assert(argc==4);unsigned which=(unsigned)strtoul(argv[3],NULL,10);size_t pn,cn;
    unsigned char *producer=read_model(argv[1],&pn),*consumer=read_model(argv[2],&cn);
    const struct npu_runtime_model models[]={{producer,pn},{consumer,cn}};
    assert(npu_runtime_requirements(models,2,2)==6291456);
    rt.prepared=1;rt.slots=2;rt.count=2;prepare(0,producer,pn);prepare(1,consumer,cn);
    struct npu_runtime_slot *p=&rt.slot[0],*s=&rt.slot[1];unsigned pi=p->layout.inputs;
    assert(!memcmp(&p->tensors[pi].tensor,&s->tensors[0].tensor,offsetof(struct nbg_tensor,name)));
    assert(word((unsigned char *)s->targets[NBG_ALLOC_NN].cpu+24)==s->targets[NBG_ALLOC_INPUT].device);
    /* Build a real offline translation image before creating the edge. */
    struct a733_npu_region regions[32];unsigned nr=0;uint64_t physical=0x60000000;
    for(unsigned slot=0;slot<2;++slot)for(unsigned i=0;i<NBG_ALLOC_INPUT+rt.slot[slot].layout.inputs+rt.slot[slot].layout.outputs;++i){
        const struct nbg_assembly_buffer *b=&rt.slot[slot].targets[i];if(!b->bytes)continue;
        regions[nr++]=(struct a733_npu_region){physical,b->bytes,b->device,A733_NPU_PAGE_4K,i==NBG_ALLOC_POOL||i>=NBG_ALLOC_INPUT+rt.slot[slot].layout.inputs};physical+=b->bytes+4096;}
    rt.native.memory.arena=(uintptr_t)allocation(65536);
    assert(a733_npu_mmu_build((void *)rt.native.memory.arena,65536,0x70000000,regions,nr,&rt.native.memory.image));
    rt.table_crc=npu_runtime_crc32c(UINT32_MAX,(void *)rt.native.memory.arena,rt.native.memory.image.bytes);
    unsigned char *before=malloc(s->command.bytes+s->constant.bytes);assert(before);
    memcpy(before,(void *)s->command.cpu,s->command.bytes);memcpy(before+s->command.bytes,(void *)s->constant.cpu,s->constant.bytes);
    struct npu_runtime_slot snapshot=*s;unsigned old_links=rt.tensor_links;
    if(which==1)++s->tensors[0].tensor.scale_bits;
    if(which==2)++s->tensors[0].tensor.sizes[0];
    if(which==3)s->targets[NBG_ALLOC_INPUT].bytes+=4096;
    if(which==4)rt.bound=1;
    if(which==5)s->targets[NBG_ALLOC_COMMAND].device=p->targets[NBG_ALLOC_INPUT+pi].device;
    int linked=npu_runtime_link_tensor(&rt,0,0,1,0);
    if(which){assert(!linked&&cache_calls==0&&rt.tensor_links==old_links&&!atomic_load(&rt.failed));
        assert(!memcmp(before,(void *)s->command.cpu,s->command.bytes)&&!memcmp(before+s->command.bytes,(void *)s->constant.cpu,s->constant.bytes));
    }else{
        assert(linked&&cache_calls==2&&rt.tensor_links&&npu_runtime_tables_intact(&rt));
        assert(s->tensors[0].data==p->tensors[pi].data);
        assert(word((unsigned char *)s->targets[NBG_ALLOC_NN].cpu+24)==p->targets[NBG_ALLOC_INPUT+pi].device);
        assert(word((unsigned char *)s->targets[NBG_ALLOC_NN].cpu+28)==s->targets[NBG_ALLOC_INPUT+s->layout.inputs].device);
        assert(snapshot.targets[NBG_ALLOC_INPUT].device!=s->targets[NBG_ALLOC_INPUT].device);
        assert(npu_runtime_stream_intact(&rt,s));
        s->span=0;s->offset=0;s->crc=UINT32_MAX;int intact;do{intact=npu_runtime_check_step(&rt,s);}while(!intact);assert(intact==1);
        ((unsigned char *)s->targets[NBG_ALLOC_NN].cpu)[24]^=1;assert(!npu_runtime_stream_intact(&rt,s));
    }
    assert(npu_runtime_tables_intact(&rt));free(before);free(producer);free(consumer);
    for(unsigned i=0;i<owned_count;++i)free(owned[i]);puts("PASS actual admitted tensor edge, assembler relocations and integrity publication");return 0;
}
