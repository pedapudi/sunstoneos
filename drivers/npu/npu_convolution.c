#include "npu_convolution_layout.h"
#include "aarch64_device_memory.h"
#include "a733_range.h"
#include "a733_mapping_check.h"
#include <string.h>
#if defined(SUNSTONE_NPU_LARGE_GRAPH)
#include "nbg_sh_nt.h"
#endif

/* SHA-256, FIPS 180-4 sections 4.1.2, 4.2.2, 5.1.1, 5.3.3 and 6.2.
 * Private artifact identity helper. Inputs are at most the 1 MiB model
 * capacity; the largest hashed allocation is 892,928 bytes of coefficients.
 * https://csrc.nist.gov/pubs/fips/180-4/upd1/final */
static uint32_t rotate(uint32_t x,unsigned n) { return x>>n | x<<(32-n); }
static uint32_t big(const unsigned char *p)
{return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];}
static void hash_block(uint32_t h[8],const unsigned char block[64])
{
    static const uint32_t k[64]={
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t w[64];
    for(unsigned i=0;i<16;++i)w[i]=big(block+4*i);
    for(unsigned i=16;i<64;++i) {
        uint32_t x=w[i-15],y=w[i-2];
        w[i]=(rotate(x,7)^rotate(x,18)^(x>>3))+w[i-16]+
             (rotate(y,17)^rotate(y,19)^(y>>10))+w[i-7];
    }
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],v=h[7];
    for(unsigned i=0;i<64;++i) {
        uint32_t t=v+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&f)^(~e&g))+k[i]+w[i];
        uint32_t u=(rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&b)^(a&c)^(b&c));
        v=g;g=f;f=e;e=d+t;d=c;c=b;b=a;a=t+u;
    }
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=v;
}
static void digest(const void *data,size_t bytes,unsigned char out[32])
{
    const unsigned char *p=data;
    uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint64_t bits=(uint64_t)bytes*8;
    while(bytes>=64){hash_block(h,p);p+=64;bytes-=64;}
    unsigned char tail[128]={0};
    memcpy(tail,p,bytes);tail[bytes]=0x80;
    size_t final=bytes<56 ? 64 : 128;
    for(unsigned i=0;i<8;++i)tail[final-1-i]=(unsigned char)(bits>>(8*i));
    hash_block(h,tail);if(final==128)hash_block(h,tail+64);
    for(unsigned i=0;i<32;++i)out[i]=(unsigned char)(h[i/4]>>(24-8*(i%4)));
}
struct supported_model {
    unsigned char digest[32];
    uint32_t model_bytes,input_bytes,output_bytes,vip_sram_bytes,coeff_bytes;
    uint32_t input[12],output[12];
    uint32_t nn_operations,descriptor_patches,state_patches,command_body_bytes;
#if defined(SUNSTONE_NPU_LARGE_GRAPH)
    uint32_t family;
#endif
};
/* Only privileged build-time catalogue entries are executable input. The table
 * fixes descriptor/control/coefficients together; CRC or caller hashes cannot
 * add entries. Effective hardware confinement remains an admission prerequisite. */
#if defined(SUNSTONE_NPU_LARGE_GRAPH)
#define NPU_MODEL_FAMILY(v) ,v##u
#else
#define NPU_MODEL_FAMILY(v)
#endif
static const struct supported_model models[]={
#include "npu_models.inc"
};
#undef NPU_MODEL_FAMILY
static const unsigned immutable[4]={NPU_CONVOLUTION_NN,NPU_CONVOLUTION_COEFF,
    NPU_CONVOLUTION_COMMAND,NPU_CONVOLUTION_BOOTSTRAP};
static unsigned immutable_slot(const struct npu_convolution *c,unsigned j)
{
#if defined(SUNSTONE_NPU_LARGE_GRAPH)
    if(j==1 && models[c->model_index].family==1)return NPU_CONVOLUTION_PPU;
#else
    (void)c;
#endif
    return immutable[j];
}
static int fault(struct npu_convolution *c)
{atomic_store_explicit(&c->failed,1,memory_order_release);return 0;}
static int good(const struct npu_convolution *c)
{return c && c->prepared && c->model_index<sizeof(models)/sizeof(*models) &&
    c->model_bytes==models[c->model_index].model_bytes &&
    c->input_bytes==models[c->model_index].input_bytes &&
    c->output_bytes==models[c->model_index].output_bytes &&
    !atomic_load_explicit(&c->failed,memory_order_acquire);}
static int private_overlap(const struct npu_convolution *c,uintptr_t p,size_t n)
{
    if(!a733_span_incl_nonempty((const void *)p,n) || a733_extents_intersect(p,n,(uintptr_t)c,sizeof(*c)) ||
       a733_extents_intersect(p,n,c->native.memory.arena,c->native.memory.capacity) ||
       a733_extents_intersect(p,n,c->native.registers,4096))return 1;
    if(c->startup && a733_extents_intersect(p,n,(uintptr_t)c->startup,sizeof(*c->startup)))return 1;
    for(unsigned i=0;i<NPU_CONVOLUTION_BUFFERS;++i)
        if(a733_extents_intersect(p,n,c->buffers[i].cpu,c->buffers[i].bytes))return 1;
    return 0;
}
/* Command streams are a short payload followed by END pairs to the end of
 * the mapped fetch span. The prefix digest plus an END check of every later
 * eight-byte unit binds the same bytes as a digest of the whole allocation. */
typedef uint64_t __attribute__((may_alias)) stream_word;
static int stream_intact(const struct npu_convolution *c,unsigned k)
{
    const struct a733_npu_buffer *b=&c->buffers[k ? NPU_CONVOLUTION_BOOTSTRAP : NPU_CONVOLUTION_COMMAND];
    size_t bytes=c->stream_bytes[k];
    unsigned char actual[32];
    if(!bytes || (bytes&7) || bytes>b->bytes)return 0;
    const stream_word *tail=(const stream_word *)b->cpu;
    for(size_t i=bytes/8;i<b->bytes/8;++i)if(tail[i]!=UINT64_C(0x10000000))return 0;
    digest((void *)b->cpu,bytes,actual);
    return !memcmp(actual,c->stream_digest[k],sizeof(actual));
}
/* Integrity policy. The shared SMP mappings cannot stop another privileged
 * writer, so bytes the NPU reads are rechecked against digests taken at
 * preparation. Every check covers descriptors, coefficients or shader, and
 * the job command stream. Preparation-time checks (startup, activation,
 * bootstrap publication, stop) also cover the private model copy and the
 * bootstrap stream. A job publication omits those two: the device never
 * reads the model copy, and the bootstrap stream runs only at OPEN. */
static int unchanged(struct npu_convolution *c,int job)
{
    unsigned char actual[32];
    if(!good(c))return 0;
    if(c->model_index>=sizeof(models)/sizeof(*models) ||
       c->model_bytes!=models[c->model_index].model_bytes ||
       c->input_bytes!=models[c->model_index].input_bytes ||
       c->output_bytes!=models[c->model_index].output_bytes)return fault(c);
    if(!job) {
        digest(c->model,c->model_bytes,actual);
        if(memcmp(actual,models[c->model_index].digest,sizeof(actual)))return fault(c);
    }
    for(unsigned j=0;j<2;++j) {
        const struct a733_npu_buffer *b=&c->buffers[immutable_slot(c,j)];
        digest((void *)b->cpu,b->bytes,actual);
        if(memcmp(actual,c->immutable_digest[j],sizeof(actual)))return fault(c);
    }
    for(unsigned k=0;k<(job ? 1u : 2u);++k)if(!stream_intact(c,k))return fault(c);
    return 1;
}
static void little(unsigned char *p,uint32_t v)
{for(unsigned i=0;i<4;++i)p[i]=(unsigned char)(v>>(8*i));}
static int bootstrap_image(struct a733_npu_buffer *b)
{
    unsigned char *p=(void *)b->cpu;
    for(size_t i=0;i<b->bytes;i+=8){little(p+i,0x10000000);little(p+i+4,0);}
    return a733_npu_bootstrap(p,b->bytes);
}
static int tensor_profile(const struct nbg_tensor *t,const uint32_t raw[12],size_t bytes)
{
    if(t->dimensions!=raw[0] || t->format!=raw[7] || t->buffer_type!=raw[8] ||
       t->quantization!=raw[9] || t->scale_bits!=raw[10] ||
       t->zero_point!=(int32_t)raw[11] || t->logical_bytes!=bytes)return 0;
    for(unsigned i=0;i<6;++i)if(t->sizes[i]!=raw[i+1])return 0;
    return 1;
}
/* Assembly allocation size fixed by the shared layout; zero when unused. */
static size_t allocation_bytes(unsigned i)
{
    unsigned slot=npu_convolution_allocation[i];
    return slot<NPU_CONVOLUTION_BUFFERS ? npu_convolution_layout[slot].bytes : 0;
}
static int profile(const struct nbg_model *m,const struct nbg_assembly_plan *p,
    const struct supported_model *v)
{
#if defined(SUNSTONE_NPU_NN_POOL_GRAPH)
    struct nbg_tensor input,output;
    if(v->family!=2 || m->header.version!=0x1001e || m->header.target!=0x1000003b ||
       m->header.operation_count!=126 || m->header.pool_bytes!=1092352 ||
       m->header.vip_sram_bytes!=518144 || v->vip_sram_bytes!=518144 ||
       m->sections[NBG_COEFF].bytes!=892608 || v->coeff_bytes!=892608 ||
       p->nn_operations!=125 || p->descriptor_patches!=534 || p->state_patches!=125 ||
       p->command_body_bytes!=3056 || v->nn_operations!=125 || v->descriptor_patches!=534 ||
       v->state_patches!=125 || v->command_body_bytes!=3056 ||
       !nbg_model_tensor(m,NBG_INPUT,0,&input)||!nbg_model_tensor(m,NBG_OUTPUT,0,&output)||
       !tensor_profile(&input,v->input,v->input_bytes)||!tensor_profile(&output,v->output,v->output_bytes))return 0;
    for(unsigned i=0;i<NBG_ALLOCATION_COUNT;++i)if(p->buffers[i].allocation_bytes!=allocation_bytes(i))return 0;
    return 1;
#elif defined(SUNSTONE_NPU_LARGE_GRAPH)
    if(v->family!=1 || v->nn_operations!=3 || v->descriptor_patches!=6 ||
       v->state_patches!=6 || v->command_body_bytes!=656 ||
       m->header.operation_count!=5 || m->header.pool_bytes!=1047808 ||
       m->header.vip_sram_bytes!=v->vip_sram_bytes || v->vip_sram_bytes!=524288 ||
       m->sections[NBG_PPU_INSTR].bytes!=512 || m->sections[NBG_COEFF].bytes ||
       p->nn_operations!=3 || p->descriptor_patches!=6 || p->state_patches!=6 ||
       p->command_body_bytes!=656)return 0;
    struct nbg_tensor input,output;
    if(!nbg_model_tensor(m,NBG_INPUT,0,&input)||!nbg_model_tensor(m,NBG_OUTPUT,0,&output)||
       !tensor_profile(&input,v->input,v->input_bytes)||
       !tensor_profile(&output,v->output,v->output_bytes))return 0;
    for(unsigned i=0;i<NBG_ALLOCATION_COUNT;++i)if(p->buffers[i].allocation_bytes!=allocation_bytes(i))return 0;
    return 1;
#else
    struct nbg_tensor input,output;
    if(m->header.version!=0x20000 || m->header.target!=0x1000003b ||
       m->header.core_count!=1 || m->header.operation_count!=v->nn_operations+1 ||
       m->header.nn_bytes!=192 || m->header.pool_bytes || m->header.axi_sram_bytes ||
       m->header.vip_sram_bytes!=v->vip_sram_bytes || v->vip_sram_bytes>0x80000 ||
       m->sections[NBG_COEFF].bytes!=v->coeff_bytes || p->nn_operations!=v->nn_operations ||
       p->descriptor_patches!=v->descriptor_patches || p->state_patches!=v->state_patches ||
       p->command_body_bytes!=v->command_body_bytes ||
       !nbg_model_tensor(m,NBG_INPUT,0,&input) || !nbg_model_tensor(m,NBG_OUTPUT,0,&output) ||
       !tensor_profile(&input,v->input,v->input_bytes) ||
       !tensor_profile(&output,v->output,v->output_bytes))return 0;
    for(unsigned i=0;i<NBG_ALLOCATION_COUNT;++i)
        if(p->buffers[i].allocation_bytes!=allocation_bytes(i) ||
           p->buffers[i].minimum_bytes>allocation_bytes(i) ||
           p->buffers[i].payload_bytes>p->buffers[i].minimum_bytes)return 0;
    return 1;
#endif
}
int npu_convolution_prepare(struct npu_convolution *c,const struct npu_convolution_config *config)
{
    if(!c || !config || c->prepared || atomic_load(&c->failed) ||
       xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED)return 0;
    struct npu_convolution_config v=*config;
    if(v.register_physical!=0x03600000 ||
       (!v.model_bytes || v.model_bytes>NPU_CONVOLUTION_MODEL_CAPACITY) ||
       !a733_span_incl_nonempty(v.model,v.model_bytes) ||
       !a733_span_incl_nonempty(c,sizeof(*c)) || !a733_span_incl_nonempty(v.arena,v.arena_bytes) ||
       a733_extents_intersect((uintptr_t)c,sizeof(*c),(uintptr_t)v.model,v.model_bytes) ||
       a733_extents_intersect((uintptr_t)c,sizeof(*c),(uintptr_t)v.arena,v.arena_bytes) ||
       a733_extents_intersect((uintptr_t)v.arena,v.arena_bytes,(uintptr_t)v.model,v.model_bytes) ||
       a733_extents_intersect((uintptr_t)config,sizeof(*config),(uintptr_t)v.arena,v.arena_bytes))return 0;
    for(unsigned i=0;i<NPU_CONVOLUTION_BUFFERS;++i) {
        const struct a733_npu_buffer *b=&v.buffers[i];
        const struct npu_convolution_slot *l=&npu_convolution_layout[i];
        if(!l->bytes){
            if(b->cpu||b->physical||b->bytes||b->device||b->role!=A733_NPU_BUFFER_READ)return 0;
            continue;
        }
        if(b->bytes!=l->bytes || b->role!=l->role ||
           !a733_span_incl_nonempty((const void *)b->cpu,b->bytes) ||
           a733_extents_intersect(b->cpu,b->bytes,(uintptr_t)c,sizeof(*c)) ||
           a733_extents_intersect(b->cpu,b->bytes,(uintptr_t)config,sizeof(*config)) ||
           a733_extents_intersect(b->cpu,b->bytes,(uintptr_t)v.model,v.model_bytes))return 0;
    }
    atomic_init(&c->failed,0);
    memset(c->model,0,sizeof(c->model));
    c->model_bytes=v.model_bytes;
    memcpy(c->model,v.model,v.model_bytes);
    unsigned char actual[32];digest(c->model,c->model_bytes,actual);
    unsigned selected=0;
    while(selected<sizeof(models)/sizeof(*models) &&
        (models[selected].model_bytes!=v.model_bytes || memcmp(actual,models[selected].digest,32)))++selected;
    if(selected==sizeof(models)/sizeof(*models))return fault(c);
    c->model_index=selected;c->input_bytes=models[selected].input_bytes;
    c->output_bytes=models[selected].output_bytes;
    struct nbg_model model;struct nbg_assembly_plan plan;
    if(nbg_model_open(c->model,c->model_bytes,&model)!=NBG_DECODE_OK ||
#if defined(SUNSTONE_NPU_LARGE_GRAPH) && !defined(SUNSTONE_NPU_NN_POOL_GRAPH)
       !nbg_sh_nt_requirements(&model,&plan)
#else
       !nbg_assembly_requirements(&model,&plan)
#endif
       || !profile(&model,&plan,&models[c->model_index]))return fault(c);
    memcpy(c->buffers,v.buffers,sizeof(c->buffers));
#if defined(SUNSTONE_NPU_LARGE_GRAPH)
    struct a733_npu_buffer mapped[NPU_CONVOLUTION_BUFFERS-1];unsigned mapped_count=0;
    for(unsigned i=0;i<NPU_CONVOLUTION_BUFFERS;++i)if(c->buffers[i].bytes)mapped[mapped_count++]=c->buffers[i];
    const struct a733_npu_buffer *native_buffers=mapped;size_t native_count=mapped_count;
#else
    const struct a733_npu_buffer *native_buffers=c->buffers;size_t native_count=NPU_CONVOLUTION_BUFFERS;
#endif
    if(!a733_npu_native_init(&c->native,v.register_cpu,v.register_physical,v.arena,v.arena_bytes,
        v.arena_physical,native_buffers,native_count,v.fatal,v.fatal_cookie))return fault(c);
    static const struct a733_mapping_rule private_buffer={A733_MAPPING_WRITE,1,
        {{A733_MAIR_NORMAL_WB,A733_SH_INNER}},0,1};
    for(unsigned i=0;i<NPU_CONVOLUTION_BUFFERS;++i) {
        const struct a733_npu_buffer *b=&c->buffers[i];
        for(size_t offset=0;offset<b->bytes;offset+=4096)
            if(!a733_mapping_check(aarch64_device_translate,b->cpu+offset,b->physical+offset,&private_buffer))
                return fault(c);
    }
    struct nbg_assembly_buffer buffers[NBG_ALLOCATION_COUNT]={0};
    for(unsigned i=0;i<NBG_ALLOCATION_COUNT;++i) {
        unsigned slot=npu_convolution_allocation[i];
        if(slot<NPU_CONVOLUTION_BUFFERS)buffers[i]=(struct nbg_assembly_buffer){
            (void *)c->buffers[slot].cpu,c->buffers[slot].bytes,c->buffers[slot].device};
    }
#if defined(SUNSTONE_NPU_LARGE_GRAPH) && !defined(SUNSTONE_NPU_NN_POOL_GRAPH)
    if(!nbg_sh_nt_assemble(&model,buffers,&plan) ||
#else
    if(!nbg_assemble(&model,buffers,&plan) ||
#endif
       !bootstrap_image(&c->buffers[NPU_CONVOLUTION_BOOTSTRAP]) ||
       !a733_npu_memory_command(&c->native.memory,c->buffers[NPU_CONVOLUTION_BOOTSTRAP].device,&c->commands[0]) ||
       !a733_npu_memory_command(&c->native.memory,c->buffers[NPU_CONVOLUTION_COMMAND].device,&c->commands[1]))return fault(c);
    for(unsigned j=0;j<4;++j) {
        const struct a733_npu_buffer *b=&c->buffers[immutable_slot(c,j)];
        digest((void *)b->cpu,b->bytes,c->immutable_digest[j]);
    }
    c->stream_bytes[0]=plan.buffers[NBG_ALLOC_COMMAND].payload_bytes;
    c->stream_bytes[1]=A733_NPU_BOOTSTRAP_BYTES;
    for(unsigned k=0;k<2;++k) {
        const struct a733_npu_buffer *b=&c->buffers[k ? NPU_CONVOLUTION_BOOTSTRAP : NPU_CONVOLUTION_COMMAND];
        if(!c->stream_bytes[k] || (c->stream_bytes[k]&7) || c->stream_bytes[k]>b->bytes)return fault(c);
        digest((void *)b->cpu,c->stream_bytes[k],c->stream_digest[k]);
        if(!stream_intact(c,k))return fault(c);
    }
    c->prepared=1;return 1;
}
static int read_reg(void *cookie,unsigned offset,uint32_t *out)
{struct npu_convolution *c=cookie;return good(c) && a733_npu_native_operations.read(&c->native,offset,out);}
static int write_reg(void *cookie,unsigned offset,uint32_t v)
{struct npu_convolution *c=cookie;return good(c) && a733_npu_native_operations.write(&c->native,offset,v);}
static uint64_t now(void *cookie)
{struct npu_convolution *c=cookie;return a733_npu_native_operations.microseconds(&c->native);}
static void delay(void *cookie,unsigned us)
{struct npu_convolution *c=cookie;a733_npu_native_operations.delay(&c->native,us);}
static void barrier(void *cookie)
{struct npu_convolution *c=cookie;a733_npu_native_operations.barrier(&c->native);}
static int same_command(const struct a733_npu_command *a,const struct a733_npu_command *b)
{return a->device==b->device && a->readable_bytes==b->readable_bytes && a->token==b->token;}
static int publish(void *cookie,const struct a733_npu_mmu_image *image,
    const struct a733_npu_command *command,int bootstrap)
{
    struct npu_convolution *c=cookie;
    if((bootstrap!=0 && bootstrap!=1) || !same_command(command,&c->commands[bootstrap ? 0 : 1]) ||
       !unchanged(c,!bootstrap))return fault(c);
    return a733_npu_native_operations.publish(&c->native,image,command,bootstrap);
}
static int finish(void *cookie,const struct a733_npu_command *command,int canceled)
{struct npu_convolution *c=cookie;return good(c) && a733_npu_native_operations.finish(&c->native,command,canceled);}
static const struct a733_npu_ops operations={read_reg,write_reg,now,delay,barrier,publish,finish};
static int metadata(const struct npu_convolution *c,const struct a733_npu_ops *ops,void *cookie,
    const struct a733_npu_mmu_image *image,const struct a733_npu_command *commands,unsigned count)
{
    const struct a733_npu_mmu_image *own=&c->native.memory.image;
    return ops && image && commands && count==2 && cookie==c &&
        ops->read==read_reg && ops->write==write_reg && ops->microseconds==now &&
        ops->delay==delay && ops->barrier==barrier && ops->publish==publish && ops->finish==finish &&
        image->physical==own->physical && image->bytes==own->bytes &&
        image->pd_entry==own->pd_entry && image->tables==own->tables &&
        same_command(&commands[0],&c->commands[0]) && same_command(&commands[1],&c->commands[1]);
}
/* Service activation, on the startup's task: the service's copy of the
 * operations, image and catalogue is the only one, checked here once. */
static int prepared(void *cookie,const struct a733_npu_ops *ops,void *backend,
    const struct a733_npu_mmu_image *image,const struct a733_npu_command *commands,unsigned count)
{
    struct npu_convolution *c=cookie;
    if(!good(c) || !metadata(c,ops,backend,image,commands,count)) {
        fault(c);return -1;
    }
    int result=a733_npu_startup_prepared(c->startup);
    if(result<0)fault(c);
    return result;
}
int npu_convolution_bind(struct npu_convolution *c,struct a733_npu_startup *startup,
    TaskHandle_t client,UBaseType_t priority,UBaseType_t affinity)
{
    if(!good(c) || c->bound || !startup || !startup->initialized || !startup->task ||
       !client || client==startup->task || private_overlap(c,(uintptr_t)startup,sizeof(*startup)) ||
       xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED)return 0;
    c->startup=startup;c->client=client;
    const struct npu_service_activation activation={startup->task,prepared,c};
    if(!npu_service_init_deferred(&c->service,&operations,c,&c->native.memory.image,c->commands,2,
        priority,affinity,&activation) || !npu_service_bind(&c->service,0,client,2))return fault(c);
    c->bound=1;return 1;
}
static enum a733_npu_platform_result platform_step(struct npu_convolution *c,int stop)
{
    if(!c || !c->bound || !c->startup || xTaskGetSchedulerState()!=taskSCHEDULER_RUNNING ||
       xTaskGetCurrentTaskHandle()!=c->startup->task || !good(c))return A733_NPU_PLATFORM_RETAINED;
    if(stop && !atomic_load_explicit(&c->closed,memory_order_acquire))return A733_NPU_PLATFORM_RETAINED;
    enum a733_npu_platform_result result=stop ? a733_npu_startup_stop(c->startup) :
        a733_npu_startup_step(c->startup);
    if(result==A733_NPU_PLATFORM_RETAINED)fault(c);
    return result;
}
enum a733_npu_platform_result npu_convolution_startup_poll(struct npu_convolution *c)
{return platform_step(c,0);}
static int client(const struct npu_convolution *c)
{return c && c->prepared && c->bound && xTaskGetSchedulerState()==taskSCHEDULER_RUNNING &&
    xTaskGetCurrentTaskHandle()==c->client;}
int npu_convolution_send(struct npu_convolution *c,enum npu_service_operation op,
    const void *input,size_t bytes,uint64_t timeout,TickType_t wait,uint64_t *ticket)
{
    if(!client(c) || !good(c) || atomic_load_explicit(&c->closed,memory_order_acquire) || c->pending || !ticket || private_overlap(c,(uintptr_t)ticket,sizeof(*ticket)) ||
       wait==portMAX_DELAY || (op!=NPU_SERVICE_SUBMIT && (input || bytes)))return 0;
    struct npu_service_request r={op,0,timeout};
    switch(op) {
    case NPU_SERVICE_OPEN:
        if(c->opened || !timeout || timeout>INT64_MAX)return 0;
        break;
    case NPU_SERVICE_STATUS:
    case NPU_SERVICE_WAIT:
        if(!c->opened || timeout)return 0;
        break;
    case NPU_SERVICE_SUBMIT:
        if(!c->ready || c->job || !input || bytes!=c->input_bytes ||
           !timeout || timeout>INT64_MAX || private_overlap(c,(uintptr_t)input,bytes))return 0;
#if defined(SUNSTONE_NPU_LARGE_GRAPH)
        /* Restore the entire intermediate allocation, including padding, only
         * while the previous job is reaped and the memory callback is idle.
         * Existing WRITE-role publication cleans/invalidates these CPU writes.
         * No pool contents are exposed or retained as numerical state. */
        if(c->native.memory.state!=A733_NPU_MEMORY_IDLE)return fault(c);
        memset((void *)c->buffers[NPU_CONVOLUTION_POOL].cpu,0,c->buffers[NPU_CONVOLUTION_POOL].bytes);
#endif
        memcpy((void *)c->buffers[NPU_CONVOLUTION_INPUT].cpu,input,bytes);r.command=1;
        break;
    case NPU_SERVICE_REAP:
        if(!c->output_ready || !c->job || timeout)return 0;
        break;
    case NPU_SERVICE_CLOSE:
        if(!c->opened || !c->ready || c->job || c->output_ready || timeout)return 0;
        break;
    default:return 0;
    }
    uint64_t t;
    if(!npu_service_send(&c->service,&r,wait,&t))return 0;
    c->pending=t;c->pending_operation=op;
    if(op==NPU_SERVICE_OPEN)c->opened=1;
    if(op==NPU_SERVICE_SUBMIT){c->job=t;c->ready=0;c->output_ready=0;}
    *ticket=t;return 1;
}
int npu_convolution_collect(struct npu_convolution *c,uint64_t ticket,
    struct npu_service_reply *out,TickType_t wait)
{
    if(!client(c) || !ticket || c->pending!=ticket || !out ||
       private_overlap(c,(uintptr_t)out,sizeof(*out)) || wait==portMAX_DELAY)return 0;
    struct npu_service_reply reply;
    if(!npu_service_collect(&c->service,ticket,&reply,wait))return 0;
    enum npu_service_operation op=c->pending_operation;c->pending=0;c->observed=reply;
    if(op==NPU_SERVICE_CLOSE) {
        if(reply.result!=NPU_SERVICE_OK || reply.fault!=A733_NPU_NO_FAULT ||
           reply.state!=A733_NPU_CLOSED || reply.owns_session || reply.canceled || reply.job_ticket)fault(c);
        else {
            c->ready=0;
            atomic_store_explicit(&c->closed,1,memory_order_release);
        }
    } else if(reply.result!=NPU_SERVICE_OK || reply.fault!=A733_NPU_NO_FAULT || !reply.owns_session ||
       reply.canceled || (c->job && reply.job_ticket!=c->job))fault(c);
    else if(op==NPU_SERVICE_REAP) {
        if(reply.state!=A733_NPU_READY)fault(c);
        else {c->job=0;c->output_ready=0;c->ready=1;}
    } else if(reply.state==A733_NPU_READY && !c->job)c->ready=1;
    else if(reply.state==A733_NPU_DONE && c->job)c->output_ready=1;
    else if(reply.state!=A733_NPU_BOOTING && reply.state!=A733_NPU_RUNNING)fault(c);
    *out=reply;return 1;
}
int npu_convolution_output(struct npu_convolution *c,void *out,size_t bytes)
{
    if(!client(c) || !good(c) || c->pending || !c->output_ready || !c->job ||
       bytes!=c->output_bytes || private_overlap(c,(uintptr_t)out,bytes))return 0;
    memcpy(out,(void *)c->buffers[NPU_CONVOLUTION_OUTPUT].cpu,bytes);return 1;
}

/* The copied successful CLOSE reply publishes the worker's completed stop.
 * Only the startup's task can use its platform callbacks and shared locks. */
enum a733_npu_platform_result npu_convolution_stop_poll(struct npu_convolution *c)
{return platform_step(c,1);}
