#include "reference.h"
#include "pvr_compute_context.h"
#include <stdio.h>

_Static_assert(sizeof(struct rogue_fwif_fwcomputecontext) == PVR_COMPUTE_CONTEXT_BYTES, "compute ABI");
_Static_assert(sizeof(struct rogue_fwif_compute_ctx_state) == PVR_COMPUTE_STATE_BYTES, "register ABI");
_Static_assert(offsetof(struct rogue_fwif_fwcomputecontext, static_compute_context_state) == 152, "switch ABI");
_Static_assert(sizeof(struct rogue_fwif_cdm_registers_cswitch) == 56, "switch registers");
static unsigned char context_bytes[PVR_COMPUTE_CONTEXT_BYTES + 2];
static unsigned char state_bytes[PVR_COMPUTE_STATE_BYTES + 2];
static unsigned char control_bytes[PVR_CCCB_CONTROL_BYTES + 2];
static unsigned char ring_bytes[PVR_CCCB_MAX_BYTES + 2];
static unsigned char vm_bytes[PVR_GPU_VM_CONTEXT_BYTES + 2];
static unsigned char table_bytes[3 * 4096];
static uint64_t table_pages[] = {0x400000, 0x401000, 0x402000};
static struct pvr_compute_buffer buffers[PVR_COMPUTE_REGIONS];
static struct pvr_compute_config config;
static uint64_t time_now;
static unsigned refused;

static uint32_t word(const void *ptr)
{ const unsigned char *p = ptr;return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void setup(void)
{
    memset(context_bytes, 0xa5, sizeof(context_bytes)); memset(state_bytes, 0xa5, sizeof(state_bytes));
    memset(control_bytes, 0xa5, sizeof(control_bytes)); memset(ring_bytes, 0xa5, sizeof(ring_bytes));
    memset(vm_bytes, 0xa5, sizeof(vm_bytes));
    buffers[0] = (struct pvr_compute_buffer){context_bytes + 1, PVR_COMPUTE_CONTEXT_BYTES, 0xc0800000};
    buffers[1] = (struct pvr_compute_buffer){state_bytes + 1, PVR_COMPUTE_STATE_BYTES, 0xc0800100};
    buffers[2] = (struct pvr_compute_buffer){control_bytes + 1, PVR_CCCB_CONTROL_BYTES, 0xc0800200};
    buffers[3] = (struct pvr_compute_buffer){ring_bytes + 1, PVR_CCCB_MAX_BYTES, 0xc0810000};
    buffers[4] = (struct pvr_compute_buffer){vm_bytes + 1, PVR_GPU_VM_CONTEXT_BYTES, 0xc0800300};
    config = (struct pvr_compute_config){.bvnc = PVR_FW_A733_BVNC, .priority = 1, .context_id = 17, .owner_id = 29,
        .switch_registers = {0x345000002340, 0x1234567, 0x678000005670, 0x7654321,
                            0x3450000089a0, 0x456000002340, 0x4560000089a0}};
    const struct pvr_gpu_mapping mapping = {0x100000, 0x800000, 4096, 12, PVR_GPU_VM_READ_ONLY};
    const struct pvr_gpu_vm_tables tables = {table_bytes, sizeof(table_bytes), table_pages, 3};
    assert(pvr_gpu_vm_build(&mapping, 1, &tables, vm_bytes + 1, PVR_GPU_VM_CONTEXT_BYTES));
}
static void unchanged_refusal(void)
{
    unsigned char old_context[sizeof(context_bytes)], old_state[sizeof(state_bytes)];
    unsigned char old_control[sizeof(control_bytes)], old_ring[sizeof(ring_bytes)], old_vm[sizeof(vm_bytes)];
    memcpy(old_context, context_bytes, sizeof(old_context));memcpy(old_state,state_bytes,sizeof(old_state));
    memcpy(old_control,control_bytes,sizeof(old_control));memcpy(old_ring,ring_bytes,sizeof(old_ring));
    memcpy(old_vm,vm_bytes,sizeof(old_vm));
    struct pvr_compute_config old_config = config;
    struct pvr_compute_buffer old_buffers[PVR_COMPUTE_REGIONS];memcpy(old_buffers,buffers,sizeof(buffers));
    assert(!pvr_compute_context_build(&config,buffers));
    assert(!memcmp(context_bytes,old_context,sizeof(old_context)) && !memcmp(state_bytes,old_state,sizeof(old_state)));
    assert(!memcmp(control_bytes,old_control,sizeof(old_control)) && !memcmp(ring_bytes,old_ring,sizeof(old_ring)));
    assert(!memcmp(vm_bytes,old_vm,sizeof(old_vm)) && !memcmp(&config,&old_config,sizeof(config)));
    assert(!memcmp(buffers,old_buffers,sizeof(buffers))); ++refused;
}
static void reference_comparison(void)
{
    for (unsigned priority = 0; priority < 3; ++priority) {
        setup(); config.priority = priority;config.context_id = priority ? UINT32_MAX : 0;
        struct rogue_fwif_fwcomputecontext expected = {0};
        struct rogue_fwif_compute_ctx_state expected_state = {0};
        struct pvr_device device = {0};
        struct pvr_fw_object vm = {buffers[PVR_COMPUTE_VM].address}, state = {buffers[PVR_COMPUTE_STATE].address};
        struct pvr_context context = {&device, &vm, config.priority, config.context_id};
        struct pvr_queue queue = {.ctx = &context, .type = DRM_PVR_JOB_TYPE_COMPUTE, .reg_state_obj = &state,
            .cccb = {buffers[PVR_COMPUTE_CONTROL].address, buffers[PVR_COMPUTE_COMMANDS].address}};
        reference_pid = config.owner_id;
        init_fw_context(&queue, &expected);reg_state_init(&expected_state, &queue);
        uint64_t stream[8] = {64};memcpy(stream + 1, config.switch_registers, sizeof(config.switch_registers));
        struct drm_pvr_ioctl_create_context_args args = {(uintptr_t)stream, sizeof(stream)};
        assert(!init_compute_fw_objs(&context, &args, &expected));
        unsigned char old_vm[PVR_GPU_VM_CONTEXT_BYTES];memcpy(old_vm, vm_bytes + 1, sizeof(old_vm));
        assert(pvr_compute_context_build(&config, buffers));
        assert(!memcmp(context_bytes + 1, &expected, sizeof(expected)));
        assert(!memcmp(state_bytes + 1, &expected_state, sizeof(expected_state)));
        assert(!memcmp(vm_bytes + 1, old_vm, sizeof(old_vm)));
        for (unsigned i=0;i<sizeof(control_bytes);++i) assert(control_bytes[i]==0xa5);
        for (unsigned i=0;i<sizeof(ring_bytes);++i) assert(ring_bytes[i]==0xa5);
        assert(context_bytes[0]==0xa5 && context_bytes[sizeof(context_bytes)-1]==0xa5);
        assert(state_bytes[0]==0xa5 && state_bytes[sizeof(state_bytes)-1]==0xa5);
        assert(vm_bytes[0]==0xa5 && vm_bytes[sizeof(vm_bytes)-1]==0xa5);
    }
}
static unsigned char kernel_regions[5][PVR_CCB_HOST_SLOTS * PVR_CCB_COMMAND_BYTES];
static int kernel_read(void *ctx,enum pvr_ccb_region r,uint32_t off,void *out,size_t n)
{ (void)ctx;assert(r<5 && off+n<=pvr_ccb_region_bytes(r));memcpy(out,kernel_regions[r]+off,n);return 1; }
static int kernel_write(void *ctx,enum pvr_ccb_region r,uint32_t off,const void *in,size_t n)
{ (void)ctx;assert(r<5 && off+n<=pvr_ccb_region_bytes(r));memcpy(kernel_regions[r]+off,in,n);return 1; }
static int kernel_barrier(void *ctx,enum pvr_ccb_barrier op){(void)ctx;(void)op;return 1;}
static int kernel_kick(void *ctx,uint32_t off,uint32_t value){(void)ctx;(void)off;(void)value;assert(0);return 0;}
static int clock_now(void *ctx,uint64_t *out){assert(ctx==buffers);*out=++time_now;return 1;}
static int client_read(void *ctx,enum pvr_cccb_region r,uint32_t off,void *out,size_t n)
{
    assert(ctx==buffers && r<=PVR_CCCB_COMMANDS);
    struct pvr_compute_buffer *b = &buffers[r==PVR_CCCB_CONTROL?PVR_COMPUTE_CONTROL:PVR_COMPUTE_COMMANDS];
    assert(off+n<=b->bytes);memcpy(out,(unsigned char *)b->cpu+off,n);return 1;
}
static int client_write(void *ctx,enum pvr_cccb_region r,uint32_t off,const void *in,size_t n)
{
    assert(ctx==buffers && r<=PVR_CCCB_COMMANDS);
    struct pvr_compute_buffer *b = &buffers[r==PVR_CCCB_CONTROL?PVR_COMPUTE_CONTROL:PVR_COMPUTE_COMMANDS];
    assert(off+n<=b->bytes);memcpy((unsigned char *)b->cpu+off,in,n);return 1;
}
static void integration(void)
{
    setup(); assert(pvr_compute_context_build(&config,buffers));
    struct pvr_ccb kernel = {0};struct pvr_cccb client = {0};
    const struct pvr_ccb_ops ko = {kernel_read,kernel_write,kernel_barrier,kernel_kick};
    const struct pvr_cccb_ops co = {clock_now,client_read,client_write,kernel_barrier};
    assert(pvr_ccb_init(&kernel,&ko,NULL,7)==PVR_CCB_OK);
    assert(pvr_cccb_init(&client,&co,buffers,&kernel,buffers[PVR_COMPUTE_COMMANDS].bytes,
                       buffers[PVR_COMPUTE_CONTEXT].address,100));
    for (unsigned i=0;i<4 && client.phase!=PVR_CCCB_IDLE;++i)assert(pvr_cccb_poll(&client)!=PVR_CCCB_FAULT);
    assert(client.phase==PVR_CCCB_IDLE && client.firmware_context==buffers[PVR_COMPUTE_CONTEXT].address);
    assert(word(context_bytes+1)==buffers[PVR_COMPUTE_CONTROL].address);
    assert(word(context_bytes+5)==buffers[PVR_COMPUTE_COMMANDS].address);
    assert(word(context_bytes+137)==buffers[PVR_COMPUTE_VM].address);
    assert(word(control_bytes+1)==0 && word(control_bytes+5)==0 && word(control_bytes+9)==0);
    assert(word(control_bytes+13)==PVR_CCCB_MAX_BYTES-1);
    assert(word(vm_bytes+1)==table_pages[0] && word(vm_bytes+9)==UINT32_MAX);
}
int main(int argc, char **argv)
{
    assert(argc == 1 || (argc == 2 && !strcmp(argv[1], "--vm-root-only")));
    setup(); uint64_t excluded_root = 0x1fc00000;
    memcpy(vm_bytes + 1, &excluded_root, sizeof(excluded_root));unchanged_refusal();
    if (argc == 2) {
        puts("PASS offline compute context: excluded native VM root preserves all output bytes");
        return 0;
    }
    reference_comparison();integration();
    setup();config.bvnc++;unchanged_refusal();
    setup();config.priority=3;unchanged_refusal();
    for(unsigned i=0;i<PVR_COMPUTE_REGIONS;++i){
        setup();buffers[i].bytes--;unchanged_refusal();
        setup();buffers[i].address++;unchanged_refusal();
        setup();buffers[i].address=0xbffffff8;unchanged_refusal();
        setup();buffers[i].address=0xc1000000;unchanged_refusal();
        setup();buffers[i].cpu=NULL;unchanged_refusal();
    }
    for(unsigned i=1;i<PVR_COMPUTE_REGIONS;++i){
        setup();buffers[i].cpu=buffers[0].cpu;unchanged_refusal();
        setup();buffers[i].address=buffers[0].address;unchanged_refusal();
    }
    setup();buffers[0].cpu=&config;unchanged_refusal();
    setup();buffers[0].cpu=buffers;unchanged_refusal();
    setup();memset(vm_bytes+1,0,8);unchanged_refusal();
    setup();vm_bytes[1]=1;unchanged_refusal();
    setup();vm_bytes[5]=16;unchanged_refusal();
    setup();vm_bytes[9]=0;unchanged_refusal();
    setup();vm_bytes[13]=1;unchanged_refusal();
    setup();buffers[PVR_COMPUTE_COMMANDS].bytes=256;
    assert(pvr_compute_context_build(&config,buffers));
    printf("PASS offline compute context: complete reference bytes, native VM/client formatting, %u atomic refusals\n",refused);
    return 0;
}
