#include "a733_gpu_graphics_memory.h"
#include "platform.h"
#include <string.h>
extern const unsigned char gpu_firmware[],gpu_firmware_end[];
static struct a733_gpu_reserved firmware;
static struct a733_gpu_compute_reserved compute;
static struct a733_gpu_graphics_memory graphics;
static struct pvr_gpu_service service;
static const struct a733_gpu_graphics_profile profile={{64,64,32},28,4,25,3000000};
static StaticTask_t client_state;
static StackType_t client_stack[4096];
static unsigned char registers[0xa108];
static unsigned powered,running,kicks;
static uint64_t offset_time;
volatile unsigned gpu_render_case=0xfeed;
static uint64_t word(const void *ptr,unsigned n)
{ const unsigned char *p=ptr;uint64_t v=0;for(unsigned i=0;i<n;i++)v|=(uint64_t)p[i]<<(8*i);return v; }
static void put(void *ptr,unsigned n,uint64_t v)
{ unsigned char *p=ptr;for(unsigned i=0;i<n;i++)p[i]=(unsigned char)(v>>(8*i)); }
static unsigned char *resource(unsigned i)
{ return firmware.config.resources[i].storage.cpu; }
static void callback(void *ctx)
{
    uint64_t el;__asm__ volatile("mrs %0,CurrentEL":"=r"(el));
    configASSERT(el==4 && ctx==&graphics && xTaskGetCurrentTaskHandle()==service.worker);
}
static int now(void *ctx,uint64_t *out)
{
    callback(ctx);uint64_t counter,frequency;
    __asm__ volatile("mrs %0,cntpct_el0\n mrs %1,cntfrq_el0":"=r"(counter),"=r"(frequency));
    *out=counter/frequency*1000000+counter%frequency*1000000/frequency+offset_time;
    return 1;
}
static int read_reg(void *ctx,uint32_t off,uint32_t width,uint64_t *out)
{callback(ctx);configASSERT(off+width<=sizeof(registers));*out=word(registers+off,width);return 1;}
static int write_reg(void *ctx,uint32_t off,uint32_t width,uint64_t value)
{callback(ctx);configASSERT(powered && off+width<=sizeof(registers));put(registers+off,width,value);if(off==0x100 && !value)running=1;return 1;}
static int enable(void *ctx)
{
    callback(ctx);configASSERT(!powered && graphics.phase==A733_GPU_GRAPHICS_READY &&
        graphics.submission.phase==PVR_RENDER_SERVICE_READY && compute.compute.phase==PVR_COMPUTE_SERVICE_READY);
    const struct pvr_gpu_mapping *fence=graphics.maps;
    configASSERT(fence->address==PVR_RENDER_STATIC_GENERAL_ADDRESS && fence->bytes==4096 &&
        fence->flags==PVR_GPU_VM_SLC_BYPASS &&
        fence->physical==graphics.payload_extent.physical+graphics.render.payload_bytes+PVR_RENDER_PROGRAM_BYTES);
    configASSERT(graphics.vm.phase==A733_GPU_VM_MEMORY_PUBLISHED && service.free_lists.phase==PVR_FREE_LIST_IDLE);
    configASSERT(service.mappings[26].pages==graphics.render_pages && service.mappings[26].page_count==3);
    powered=1;return 1; /* Register/firmware model, never a physical power claim. */
}
static int mode(void *ctx)
{ callback(ctx);configASSERT(powered);return 1; /* Explicit model MC1 observation. */ }
static int publish(void *ctx)
{ callback(ctx);return a733_gpu_memory_publish(&firmware.memory); }
static int started(void *ctx,uint32_t *out)
{
    callback(ctx);
    if(running) {
        resource(PVR_FW_SYSTEM_INIT)[352]=1;
        unsigned char *c=resource(PVR_FW_OS_INIT)+40;
        put(c+8,8,PVR_FW_A733_BVNC);put(c+24,8,PVR_FW_A733_BVNC);c[56]=1;
    }
    return a733_gpu_memory_started(&firmware.memory,out);
}
static int read_queue(void *ctx,enum pvr_ccb_region r,uint32_t off,void *out,size_t n)
{callback(ctx);return a733_gpu_memory_read(&firmware.memory,r,off,out,n);}
static int write_queue(void *ctx,enum pvr_ccb_region r,uint32_t off,const void *in,size_t n)
{callback(ctx);return a733_gpu_memory_write(&firmware.memory,r,off,in,n);}
static int barrier(void *ctx,enum pvr_ccb_barrier kind)
{callback(ctx);return a733_gpu_memory_barrier(&firmware.memory,kind);}
static void commands(unsigned char *p,const struct pvr_cccb_command *expected,unsigned n)
{
    for(unsigned i=0;i<n;i++) {
        configASSERT(word(p,4)==expected[i].type && word(p+4,4)==expected[i].bytes);
        configASSERT(!memcmp(p+40,expected[i].payload,expected[i].bytes));p+=40+expected[i].bytes;
    }
}
static int kick(void *ctx,uint32_t off,uint32_t value)
{
    callback(ctx);configASSERT(running && off==0xb00 && !value);
    unsigned next=(unsigned)word(resource(0),4),slot=(next-1)&127;
    unsigned char *command=resource(1)+slot*88;
    uint32_t type=(uint32_t)word(command,4);
    if(type==(PVR_CCB_MAGIC|117)) {
        ++kicks;configASSERT(kicks==1);
        uintptr_t shared=a733_gpu_graphics_memory_render_shared(&graphics);
        const struct pvr_render_job_image *im=&graphics.submission.image;
        struct pvr_cccb_command geom[]={
            {PVR_CCB_MAGIC|PVR_CCCB_TASK|201,112,1,1,im->geometry},
            {PVR_CCB_MAGIC|213,8,0,0,im->geometry_update}};
        struct pvr_cccb_command frag[]={
            {PVR_CCB_MAGIC|215,8,0,0,im->fragment_fence},
            {PVR_CCB_MAGIC|PVR_CCCB_TASK|203,480,1,1,im->fragment},
            {PVR_CCB_MAGIC|213,8,0,0,im->fragment_update}};
        commands((void *)(shared+4096),geom,2);commands((void *)(shared+8192),frag,3);
        configASSERT(word(command+8,4)==A733_GPU_RENDER_FW && word(command+40,4)==A733_GPU_RENDER_FW+152);
        configASSERT(word(command+12,4)==200 && word(command+44,4)==616);
        configASSERT(word(command+20,4)==1 && word(command+52,4)==1);
        configASSERT(word(command+24,4)==graphics.target.data_firmware[0]+320 && word(command+56,4)==word(command+24,4));
        configASSERT(!word((void *)(shared+1408),8));
        /* Model only firmware acknowledgement/timelines. The GPU output stays
         * poisoned in every case; no CPU implementation supplies clear pixels. */
        if(gpu_render_case==2)return 0; /* Uncertain posted KICK retains both rings. */
        if(gpu_render_case==0) {
            __atomic_store_n((uint32_t *)(shared+1408),1,__ATOMIC_RELEASE);
            __atomic_store_n((uint32_t *)(shared+1412),1,__ATOMIC_RELEASE);
        }
        if(gpu_render_case==3)offset_time=1000000;
    } else configASSERT(type==PVR_CCB_HEALTH);
    put(resource(0)+128,4,next);put(resource(2)+slot*4,4,1);return 1;
}
static int irq_read(void *ctx,uint32_t off,uint32_t *out)
{callback(ctx);configASSERT(off==0x8a8);*out=0;return 1;}
static int irq_write(void *ctx,uint32_t off,uint32_t value)
{callback(ctx);configASSERT(off==0x8b0 && value==1);return 1;}
static int compat(void *ctx,uint32_t off,void *out,size_t n)
{callback(ctx);return a733_gpu_memory_compat(&firmware.memory,off,out,n);}
static int unexpected(void *ctx)
{callback(ctx);configASSERT(0);return 0;}
static int sync_read(void *ctx,uint32_t *out)
{callback(ctx);return a733_gpu_memory_sync_read(&firmware.memory,out);}
static int sync_write(void *ctx,uint32_t in)
{callback(ctx);return a733_gpu_memory_sync_write(&firmware.memory,in);}
static struct pvr_gpu_service_reply request(enum pvr_gpu_service_operation op,uint64_t duration)
{
    struct pvr_gpu_service_request q={op,duration};uint64_t ticket;struct pvr_gpu_service_reply r;
    configASSERT(pvr_gpu_service_send(&service,&q,10,&ticket));
    configASSERT(pvr_gpu_service_collect(&service,ticket,&r,1000));return r;
}
static void held(struct device_memory_pool *p,struct device_memory_lease token)
{
    struct device_memory_extent e;enum device_memory_state state;
    configASSERT(device_memory_describe(p,token,&e,&state) && state==DEVICE_MEMORY_DEVICE);
}
static void client(void *unused)
{
    (void)unused;struct pvr_gpu_service_reply r=request(PVR_GPU_SERVICE_OPEN,5000000);
    configASSERT(r.result==PVR_GPU_SERVICE_OK);
    for(unsigned i=0;i<4000;i++) {
        vTaskDelay(1);r=request(PVR_GPU_SERVICE_STATUS,0);
        configASSERT(r.result!=PVR_GPU_SERVICE_FAULT);
        if(r.phase==PVR_GPU_SERVICE_READY)break;
    }
    configASSERT(r.phase==PVR_GPU_SERVICE_READY);
    uint64_t ticket;
    configASSERT(pvr_gpu_service_render_read(&service,0,10,&ticket));
    configASSERT(pvr_gpu_service_collect(&service,ticket,&r,1000));
    configASSERT(r.result==PVR_GPU_SERVICE_BUSY && !r.render_pixel_count);
    r=request(PVR_GPU_SERVICE_RENDER,300000);configASSERT(r.result==PVR_GPU_SERVICE_OK);
    for(unsigned i=0;i<2000;i++) {
        vTaskDelay(1);r=request(PVR_GPU_SERVICE_STATUS,0);
        if(r.phase==PVR_GPU_SERVICE_FAILED)break;
    }
    configASSERT(kicks==1 && r.phase==PVR_GPU_SERVICE_FAILED && r.error==PVR_GPU_SERVICE_GRAPHICS);
    configASSERT(graphics.submission.phase==PVR_RENDER_SERVICE_FAILED);
    if(gpu_render_case==0)configASSERT(graphics.submission.error==PVR_RENDER_SERVICE_RESULT);
    if(gpu_render_case==1)configASSERT(graphics.submission.error==PVR_RENDER_SERVICE_CLOCK);
    if(gpu_render_case>=2)configASSERT(graphics.submission.error==PVR_RENDER_SERVICE_QUEUE);
    held(firmware.wb_pool,graphics.backing_lease);held(firmware.wb_pool,graphics.payload_lease);
    held(firmware.nc_pool,graphics.shared_lease);held(firmware.wb_pool,graphics.table_lease);
    configASSERT(pvr_gpu_service_render_read(&service,0,10,&ticket));
    configASSERT(pvr_gpu_service_collect(&service,ticket,&r,1000));
    configASSERT(r.result==PVR_GPU_SERVICE_FAULT && !r.render_pixel_count);
    configASSERT(pvr_render_service_poll(&graphics.submission)==-1);
    platform_puts(gpu_render_case==0?"PASS render: actual native paired submission rejects poisoned output after modeled timelines\n":
        gpu_render_case==1?"PASS render: missing GPU timelines times out with both rings and all parents retained\n":
        gpu_render_case==2?"PASS render: uncertain combined KICK retains both submitted ring owners\n":
        "PASS render: late combined KICK cannot escape request deadline\n");
    platform_exit(0);
}
void vApplicationStackOverflowHook(TaskHandle_t t,char *name)
{(void)t;platform_panic(name,0);}
void kernel_main(void)
{
    platform_init();configASSERT(gpu_render_case<4);
    configASSERT(a733_memory_reservations_init() && a733_memory_uncached_init());
    configASSERT(a733_gpu_reserved_prepare(&firmware,9,gpu_firmware,(size_t)(gpu_firmware_end-gpu_firmware),400000000));
    configASSERT(a733_gpu_compute_reserved_prepare(&compute,&firmware,&service,10));
    configASSERT(a733_gpu_graphics_memory_prepare_clear(&graphics,&firmware,&compute,&service,11,&profile));
    configASSERT(a733_gpu_graphics_memory_validate(&graphics,&firmware.config,&service));
    struct pvr_gpu_service_config *c=&firmware.config;
    c->queue=(struct pvr_ccb_ops){read_queue,write_queue,barrier,kick};
    c->start=(struct pvr_gpu_start_ops){now,read_reg,write_reg,started,publish};
    c->control=(struct pvr_gpu_control_ops){now,irq_read,irq_write,compat,NULL};
    c->stop=(struct pvr_gpu_stop_ops){now,read_reg,write_reg,sync_write,sync_read,unexpected};
    c->enable=enable;c->compute_mode=mode;
    configASSERT(pvr_gpu_service_init(&service,c,3,1));
    TaskHandle_t t=xTaskCreateStaticAffinitySet(client,"render-check",4096,NULL,2,client_stack,&client_state,2);
    configASSERT(t && pvr_gpu_service_bind(&service,0,t));vTaskStartScheduler();platform_panic("scheduler",0);
}
