/* Reuse firmware preparation, register models and privileged task checks. */
#define kernel_main gpu_poll_fixture_main
#include "pvr-gpu-service.c"
#undef kernel_main

static struct pvr_free_list_config list_config;
static struct pvr_free_list_page list_pages[44];
static _Alignas(4096) unsigned char list_backing[44][4096];
static _Alignas(8) unsigned char list_table[44*4], list_record[64];
static uint32_t acquired, page_published, table_published, list_responses, recovered;
static uint32_t hold_return, held_slot, finish_close, retire_event, clock_bias;
static int list_now(void *ctx,uint64_t *out)
{int result=now(ctx,out);*out+=get(&clock_bias);return result;}

static int list_allocate(void *ctx,uint64_t epoch,uint64_t owner_id,uint64_t token,
                         unsigned slot,struct pvr_free_list_page *out)
{
    callback(ctx);configASSERT(epoch==1 && owner_id==1 && token==100 && slot<44);
    configASSERT(slot==get(&acquired));
    *out=(struct pvr_free_list_page){list_backing[slot],(uintptr_t)list_backing[slot],slot+1};
    __atomic_fetch_add(&acquired,1,__ATOMIC_RELEASE);return 1;
}
static int list_publish_page(void *ctx,const struct pvr_free_list_page *page)
{
    callback(ctx);configASSERT(page->lease && service.free_lists.lists[0].acquired==get(&acquired));
    for(unsigned i=0;i<4096;++i)configASSERT(!((unsigned char *)page->cpu)[i]);
    __asm__ volatile("dsb sy" ::: "memory");
    __atomic_fetch_add(&page_published,1,__ATOMIC_RELEASE);return 1;
}
static int list_read(void *ctx,uint64_t token,enum pvr_free_list_memory kind,uint32_t off,void *out,size_t n)
{
    callback(ctx);configASSERT(token==100 && kind==PVR_FREE_LIST_FIRMWARE && off==28 && n==4);
    memcpy(out,list_record+off,n);return 1;
}
static int list_write(void *ctx,uint64_t token,enum pvr_free_list_memory kind,uint32_t off,const void *in,size_t n)
{
    callback(ctx);configASSERT(token==100 && off+n<=(kind==PVR_FREE_LIST_TABLE ? sizeof(list_table) : sizeof(list_record)));
    memcpy((kind==PVR_FREE_LIST_TABLE ? list_table : list_record)+off,in,n);return 1;
}
static int list_publish(void *ctx,uint64_t token,enum pvr_free_list_memory kind,uint32_t off,size_t n)
{
    callback(ctx);configASSERT(token==100);(void)off;(void)n;
    if(kind==PVR_FREE_LIST_TABLE) {
        if(gpu_service_case==2 && running)return 0;
        __atomic_fetch_add(&table_published,1,__ATOMIC_RELEASE);
    }
    if(gpu_service_case==4 && kind==PVR_FREE_LIST_FIRMWARE && off==0 && n==64)
        set(&clock_bias,2000000); /* Final publication completes after the operation deadline. */
    __asm__ volatile("dsb sy" ::: "memory");return 1;
}
static int list_recover(void *ctx,uint64_t token)
{callback(ctx);configASSERT(token==100);__atomic_fetch_add(&recovered,1,__ATOMIC_RELEASE);return 1;}
static int list_enable(void *ctx)
{
    configASSERT(get(&acquired)==28 && get(&page_published)==28 && get(&table_published)==2);
    configASSERT(word(list_record+28,4)==24 && word(list_record+48,4)==4);
    configASSERT(service.free_lists.phase==PVR_FREE_LIST_IDLE);return enable(ctx);
}
static void list_event(unsigned kind)
{
    unsigned next=get((uint32_t *)resource(3));unsigned char *command=resource(4)+next*88;
    memset(command,0,88);put(command,4,PVR_CCB_MAGIC|kind);
    if(kind==104) {put(command+8,4,1);put(command+16,4,7);}
    else put(command+8,4,kind==101 ? 0x1234 : 7);
    __asm__ volatile("dsb sy" ::: "memory");set((uint32_t *)resource(3),(next+1)&31);
}
static int list_kick(void *ctx,uint32_t off,uint32_t value)
{
    unsigned next=(unsigned)word(resource(0),4),slot=(next-1)&127;
    unsigned char *command=resource(1)+slot*88;unsigned type=(unsigned)word(command,4);
    if(type!=(PVR_CCB_MAGIC|110u) && type!=(PVR_CCB_MAGIC|112u)) {
        return kick(ctx,off,value);
    }
    callback(ctx);configASSERT(running && off==0xb00 && !value);
    if(type==(PVR_CCB_MAGIC|110u)) {
        configASSERT(word(command+8,4)==list_config.firmware_address && word(command+12,4)==8);
        put(list_record+28,4,word(command+16,4)-word(command+20,4));put(list_record+48,4,word(command+20,4));
    } else configASSERT(word(command+8,4)==1 && word(command+12,4)==7 && get(&recovered));
    put(resource(0)+128,4,next);
    if(get(&hold_return))set(&held_slot,slot);
    else put(resource(2)+slot*4,4,1);
    __atomic_fetch_add(&list_responses,1,__ATOMIC_RELEASE);return 1;
}
static int list_sync_read(void *ctx,uint32_t *out)
{
    int result=sync_read(ctx,out);
    /* Publish after the stop controller's last control poll, immediately before
     * its RETIRE stage. The service must poll once more and finish this action. */
    if(gpu_service_case==1 && result && *out && service.stop.phase==PVR_GPU_STOP_WAIT_OFF) {
        set(&retire_event,1);list_event(104);
    }
    return result;
}
static void list_peer(void *arg)
{
    (void)arg;wait_flag(&finish_close,1);vTaskDelay(2);
    configASSERT(service.phase==PVR_GPU_SERVICE_DRAINING && service.free_lists.request_count==1);
    set(&hold_return,0);set((uint32_t *)(resource(2)+get(&held_slot)*4),1);
    vTaskSuspend(NULL);
}
static void verify_retained(void)
{
    struct pvr_gpu_service_reply r=request(PVR_GPU_SERVICE_STATUS,0);
    configASSERT(r.phase==PVR_GPU_SERVICE_FAILED && r.owns_session && r.free_list_pending==1);
    configASSERT(r.error==(gpu_service_case==2 ? PVR_GPU_SERVICE_FREE_LISTS : PVR_GPU_SERVICE_STOP));
    if(gpu_service_case==2)configASSERT(r.free_list_error==PVR_FREE_LIST_PUBLICATION && !get(&list_responses));
    else configASSERT(get(&list_responses)==1 && service.stop.phase==PVR_GPU_STOP_NEW);
    configASSERT(get(&acquired)==36 && get(&page_published)==36 && !get(&isolated));
    unsigned previous=get(&calls);vTaskDelay(4);
    configASSERT(request(PVR_GPU_SERVICE_OPEN,100000).result==PVR_GPU_SERVICE_FAULT);
    configASSERT(request(PVR_GPU_SERVICE_CLOSE,100000).result==PVR_GPU_SERVICE_FAULT);
    configASSERT(previous==get(&calls));
}
static void list_owner(void *arg)
{
    (void)arg;
    struct pvr_gpu_service_reply r=request(PVR_GPU_SERVICE_OPEN,gpu_service_case==4 ? 5000000 : 1000000);
    configASSERT(r.result==PVR_GPU_SERVICE_OK && r.phase==PVR_GPU_SERVICE_BUILDING);
    r=request(PVR_GPU_SERVICE_STATUS,0);configASSERT(r.phase==PVR_GPU_SERVICE_BUILDING && !get(&enabled));
    configASSERT(request(PVR_GPU_SERVICE_CLOSE,100000).result==PVR_GPU_SERVICE_BUSY);
    if(gpu_service_case==4) {
        r=wait_phase(PVR_GPU_SERVICE_FAILED);
        configASSERT(r.error==PVR_GPU_SERVICE_FREE_LISTS && r.owns_session && !r.free_list_pending);
        configASSERT(service.free_lists.phase==PVR_FREE_LIST_IDLE && get(&acquired)==28 && get(&page_published)==28);
        configASSERT(!get(&enabled) && !get(&published) && list_pages[27].lease==28);
        unsigned before=get(&calls);vTaskDelay(4);
        configASSERT(request(PVR_GPU_SERVICE_OPEN,100000).result==PVR_GPU_SERVICE_FAULT && get(&calls)==before);
        platform_puts("PASS GPU free-list service: case ");platform_hex(gpu_service_case);
        platform_puts("; late final publication retains leases before firmware startup\n");platform_exit(0);
    }
    configASSERT(wait_phase(PVR_GPU_SERVICE_READY).owns_session);
    configASSERT(get(&cores)==3 && get(&published)==1);
    if(gpu_service_case==3)set(&hold_return,1);
    list_event(103);
    if(gpu_service_case==2) {wait_phase(PVR_GPU_SERVICE_FAILED);verify_retained();}
    else {
        wait_flag(&list_responses,1);
        if(gpu_service_case==3) {
            r=request(PVR_GPU_SERVICE_CLOSE,20000);
            configASSERT(r.result==PVR_GPU_SERVICE_OK && r.phase==PVR_GPU_SERVICE_DRAINING && r.free_list_pending==1);
            vTaskDelay(50);verify_retained();
        } else {
            configASSERT(gpu_service_case==1);
            list_event(101);wait_flag(&handled,1); /* Other platform events still dispatch. */
            memset(list_table+(44-36)*4,0xa5,36*4);list_event(104);wait_flag(&list_responses,2);
            configASSERT(get(&recovered)==1);
            for(unsigned i=0;i<28;++i)configASSERT(word(list_table+(44-28+i)*4,4)==((uintptr_t)list_backing[i]>>12));
            for(unsigned i=0;i<8;++i)configASSERT(word(list_table+(44-36+i)*4,4)==((uintptr_t)list_backing[28+i]>>12));
            set(&hold_return,1);list_event(103);wait_flag(&list_responses,3);
            r=request(PVR_GPU_SERVICE_CLOSE,1000000);
            configASSERT(r.result==PVR_GPU_SERVICE_OK && r.phase==PVR_GPU_SERVICE_DRAINING && r.free_list_pending==1);
            configASSERT(request(PVR_GPU_SERVICE_OPEN,100000).result==PVR_GPU_SERVICE_BUSY);
            set(&finish_close,1);r=wait_phase(PVR_GPU_SERVICE_STOPPED);
            configASSERT(!r.owns_session && !r.free_list_pending && get(&isolated)==1);
            configASSERT(get(&retire_event) && get(&recovered)==2 && get(&list_responses)==4);
            configASSERT(get(&acquired)==44 && list_pages[43].lease==44);
            configASSERT(request(PVR_GPU_SERVICE_OPEN,1000000).result==PVR_GPU_SERVICE_BUSY);
            configASSERT(service.epoch==1 && get(&enabled)==1);
        }
    }
    for(unsigned i=0;i<22;++i)for(unsigned j=0;j<4096;++j)configASSERT(guards[i][j]==0xa5);
    platform_puts("PASS GPU free-list service: case ");platform_hex(gpu_service_case);
    platform_puts("; bounded construction, privileged event work, retained response ownership\n");platform_exit(0);
}
void kernel_main(void)
{
    platform_init();memset(arena,0xa5,sizeof(arena));memset(list_backing,0xa5,sizeof(list_backing));
    config.firmware=gpu_firmware;config.firmware_bytes=(size_t)(gpu_firmware_end-gpu_firmware);
    config.state=(struct pvr_fw_state_config){PVR_FW_A733_BVNC,123456789,1,16};
    config.queue=(struct pvr_ccb_ops){read_mem,write_mem,barrier,list_kick};
    config.start=(struct pvr_gpu_start_ops){list_now,read_reg,write_reg,started,publish};
    config.control=(struct pvr_gpu_control_ops){list_now,irq_read,irq_write,compat,event};
    config.stop=(struct pvr_gpu_stop_ops){list_now,read_reg,write_reg,sync_write,list_sync_read,isolate};
    config.enable=list_enable;config.context=&config;
    unsigned byte=0,page=0;
    for(unsigned i=0;i<22;++i) {
        unsigned n=i<2 ? (i ? 20480 : 102400) : i==2 ? 16384 :
            pvr_fw_state_layout((enum pvr_fw_resource)(i-3))->allocation_bytes;
        struct pvr_mips_buffer *b=i<3 ? &config.firmware_buffers[i] : &config.resources[i-3];
        *b=(struct pvr_mips_buffer){{arena+byte,n},pages+page,n/4096};
        for(unsigned p=0;p<n/4096;++p)pages[page++]=(uintptr_t)(arena+byte+p*4096);
        guards[i]=arena+byte+n;byte+=n+4096;
    }
    list_config=(struct pvr_free_list_config){{PVR_GPU_RESOURCE_FREELIST,7,1,100},0x400000000,0xc0200000,28,44,8,25,list_pages};
    config.free_lists=(struct pvr_gpu_service_free_list_config){&list_config,1,25,1000000,
        {list_allocate,list_publish_page,list_read,list_write,list_publish,list_recover}};
    list_config.pages=(void *)config.resources[0].storage.cpu;
    configASSERT(!pvr_gpu_service_init(&service,&config,4,1) && !get(&calls));
    list_config.pages=list_pages;
    config.free_lists.lists=(void *)config.resources[0].storage.cpu;
    configASSERT(!pvr_gpu_service_init(&service,&config,4,1) && !get(&calls));
    config.free_lists.lists=&list_config;
    configASSERT(pvr_gpu_service_init(&service,&config,4,1) && !get(&calls));
    TaskHandle_t a=xTaskCreateStaticAffinitySet(list_owner,"list-owner",4096,NULL,5,stacks[0],&tasks[0],1);
    configASSERT(a && pvr_gpu_service_bind(&service,0,a));
    configASSERT(xTaskCreateStaticAffinitySet(list_peer,"list-peer",4096,NULL,5,stacks[1],&tasks[1],2));
    configASSERT(xTaskCreateStaticAffinitySet(supervisor,"supervisor",4096,NULL,6,stacks[2],&tasks[2],2));
    vTaskStartScheduler();platform_panic("scheduler returned",0);
}
