/* Host instruction fixture: keep the production stream constructor and layout. */
#include "../drivers/npu/npu_convolution.c"

__attribute__((used,retain)) const uint64_t npu_active_cancel_geometry[5]={
    NPU_CONVOLUTION_BUFFERS,NPU_CONVOLUTION_RESERVED_BYTES,
    NPU_CONVOLUTION_ARENA_OFFSET,NPU_CONVOLUTION_WAIT_BYTES,
    sizeof(struct npu_convolution_slot)};
__attribute__((used,retain)) const struct npu_convolution_slot *const
    npu_active_cancel_slots=npu_convolution_layout;

int npu_active_cancel_stream(struct a733_npu_buffer *buffer)
{return diagnostic_image(buffer);}

/* The instruction model supplies CPU translation and cache geometry. The
 * native adapter, MMU builder, memory admission and MMIO helpers stay linked. */
int aarch64_device_translate(uintptr_t cpu,int write,struct aarch64_translation *out)
{(void)cpu;(void)write;(void)out;return 0;}
unsigned aarch64_device_cache_stride(void){return 0;}
int aarch64_device_cache(enum aarch64_cache_op op,uintptr_t cpu,size_t bytes)
{(void)op;(void)cpu;(void)bytes;return 0;}
static void fixture_fatal(void *unused){(void)unused;__builtin_trap();}
int npu_active_cancel_native(struct a733_npu_native *native,uintptr_t parent,
    uint64_t physical,uintptr_t registers)
{
    static _Alignas(8) uint8_t tables[A733_NPU_MEMORY_TABLES_BYTES(NPU_CONVOLUTION_BUFFERS)];
    struct a733_npu_buffer buffers[NPU_CONVOLUTION_BUFFERS];
    for(unsigned i=0;i<NPU_CONVOLUTION_BUFFERS;++i) {
        const struct npu_convolution_slot *slot=&npu_convolution_layout[i];
        buffers[i]=(struct a733_npu_buffer){parent+slot->offset,physical+slot->offset,
            slot->bytes,slot->device,slot->role};
    }
    return a733_npu_native_init(native,registers,0x03600000,
        (void *)(parent+NPU_CONVOLUTION_ARENA_OFFSET),NPU_CONVOLUTION_TABLE_BYTES,
        physical+NPU_CONVOLUTION_ARENA_OFFSET,buffers,NPU_CONVOLUTION_BUFFERS,
        fixture_fatal,NULL,tables,sizeof(tables));
}
__attribute__((used,retain)) const struct a733_npu_ops *const
    npu_active_cancel_native_operations=&a733_npu_native_operations;
