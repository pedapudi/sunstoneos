/* Reuse the existing firmware/service model; only computation is exercised. */
#define kernel_main firmware_fixture_main
#define owner firmware_fixture_owner
#define kick firmware_fixture_kick
#define event firmware_fixture_event
#include "pvr-gpu-service.c"
#undef kernel_main
#undef owner
#undef kick
#undef event
#include "pvr_compute_service.h"
#include "pvr_compute_context.h"

volatile uint32_t compute_service_case=1;
static struct pvr_compute_service compute,refused_compute;
static struct pvr_compute_service_config computation;
static struct pvr_compute_program_config programs;
static struct pvr_compute_program_result program_result;
static struct pvr_compute_job_config job_config;
static struct pvr_compute_job_image job_image;
static struct a733_gpu_vm_memory vm;
static struct pvr_gpu_mapping maps[PVR_COMPUTE_SERVICE_MAPS];
static uint64_t backing_pages[128],table_pages[PVR_COMPUTE_SERVICE_TABLE_PAGES];
static uint32_t job_kicks,peer_checked,allow_complete,event_forwarded;
static unsigned backing_used,mode_calls;
static int observed_mode(void *ctx)
{
    callback(ctx); ++mode_calls;
    configASSERT(get(&enabled)&&vm.phase==A733_GPU_VM_MEMORY_PUBLISHED&&(compute.phase==PVR_COMPUTE_SERVICE_READY||compute.phase==PVR_COMPUTE_SERVICE_COMPLETE));
    if(compute_service_case==4)return 0;
    if(compute_service_case==5&&mode_calls==2)return -1;
    return 1;
}
static int admit_program(void *ctx,const struct pvr_compute_program_config *c)
{ (void)ctx;return c==&programs&&c->execution_cores==1&&c->admission_token==17; }
static int admit_job(void *ctx,const struct pvr_compute_job_config *c)
{ (void)ctx;return c==&job_config&&c->programs==&programs&&programs.admission_token==17; }
static struct pvr_mips_buffer allocation(uintptr_t cpu,size_t bytes)
{
    size_t rounded=(bytes+4095)&~(size_t)4095;unsigned first=backing_used;
    for(size_t off=0;off<rounded;off+=4096){configASSERT(backing_used<128);backing_pages[backing_used++]=cpu+off;}
    memset((void *)cpu,0xa5,rounded);
    return (struct pvr_mips_buffer){{(void *)cpu,rounded},backing_pages+first,(uint32_t)(rounded/4096)};
}
#ifdef PVR_COMPUTE_SERVICE_PACKAGE_FIXTURE
extern const struct pvr_compute_package fixture_package;
extern const unsigned char fixture_expected[];
#endif
static void workload(void)
{
#ifdef PVR_COMPUTE_SERVICE_PACKAGE_FIXTURE
    if(compute_service_case==12)job_config.package=&fixture_package;
#endif
    programs=(struct pvr_compute_program_config){.bvnc=PVR_FW_A733_BVNC,
        .pds={UINT64_C(0x400000000),UINT64_C(1)<<32},.usc={UINT64_C(0x500000000),UINT64_C(1)<<32},
        .general={UINT64_C(0x600000000),UINT64_C(1)<<32},.excluded_count=5,
        .execution_cores=1,.admission_token=17,.admit=admit_program};
    static const unsigned sizes[5]={232,4,32,1024,32};
    struct pvr_compute_buffer contexts[5];
    for(unsigned i=0;i<6;++i) {
        computation.firmware[i]=allocation(0x46100000+i*0x10000,4096);
        computation.firmware_address[i]=0xc0800000+i*0x10000;
        if(i<5) {
            programs.excluded[i]=(struct pvr_compute_program_span){computation.firmware[i].storage.cpu,sizes[i],computation.firmware_address[i]};
            contexts[i]=(struct pvr_compute_buffer){programs.excluded[i].cpu,sizes[i],computation.firmware_address[i]};
        }
    }
    unsigned map_count=0;
    for(unsigned i=0;i<18;++i) {
        int is_program=i<10;unsigned j=is_program?i:i-10;
        if(!is_program&&j==PVR_JOB_UFO){computation.buffers[j]=computation.firmware[5];continue;}
        size_t bytes=is_program?pvr_compute_program_bytes[j]:pvr_compute_job_region_bytes(&job_config,j);
        uintptr_t cpu=0x44000000+i*0x50000;
        struct pvr_mips_buffer *b=is_program?&computation.programs[j]:&computation.buffers[j];
        *b=allocation(cpu,bytes);
        uint64_t gpu;
        if(is_program)gpu=(j<2?programs.usc.base:j<9?programs.pds.base:programs.general.base)+0x10000+i*0x50000;
        else gpu=(j==0?programs.usc.base:j<3?programs.pds.base:programs.general.base)+0x1000000+j*0x50000;
        struct pvr_compute_program_span l={(void *)cpu,bytes,gpu};
        if(is_program)programs.buffers[j]=l;else job_config.buffers[j]=l;
        int ro=is_program?j<PVR_PROGRAM_SHARED_A:j!=PVR_JOB_OUTPUT;
        for(unsigned k=0;k<b->page_count;++k) {
            configASSERT(map_count<PVR_COMPUTE_SERVICE_MAPS);
            maps[map_count++]=(struct pvr_gpu_mapping){gpu+k*4096,b->pages[k],4096,12,ro?PVR_GPU_VM_READ_ONLY:0};
        }
    }
    job_config.buffers[PVR_JOB_UFO]=(struct pvr_compute_program_span){computation.firmware[5].storage.cpu,64,computation.firmware_address[5]};
    for(unsigned i=1;i<map_count;++i){struct pvr_gpu_mapping m=maps[i];unsigned j=i;while(j&&maps[j-1].address>m.address){maps[j]=maps[j-1];--j;}maps[j]=m;}
    struct pvr_gpu_vm_requirements required;configASSERT(pvr_gpu_vm_require(maps,map_count,&required));
    configASSERT(required.table_pages<=PVR_COMPUTE_SERVICE_TABLE_PAGES);
    for(unsigned i=0;i<required.table_pages;++i)table_pages[i]=0x44c00000+i*4096;
    struct pvr_gpu_vm_tables tables={(void *)0x44c00000,required.table_bytes,table_pages,required.table_pages};
    configASSERT(a733_gpu_vm_memory_init(&vm,&tables,contexts[4].cpu,computation.firmware[4].pages[0]));
    configASSERT(a733_gpu_vm_memory_build(&vm,maps,map_count)==A733_GPU_VM_MEMORY_OK);
    configASSERT(pvr_compute_programs_build(&programs,&program_result));
    struct pvr_compute_config context={.bvnc=PVR_FW_A733_BVNC,.priority=1,.context_id=17,.owner_id=23};
    memcpy(context.switch_registers,program_result.switch_registers,sizeof(context.switch_registers));
    configASSERT(pvr_compute_context_build(&context,contexts));
    job_config.programs=&programs;job_config.program_result=&program_result;
    job_config.firmware_context=computation.firmware_address[0];job_config.firmware_ufo=computation.firmware_address[5];
    job_config.seed=17;job_config.factor=3;job_config.job_id=31;job_config.admit=admit_job;
    configASSERT(pvr_compute_job_build(&job_config,&job_image));
    computation.job=&job_config;computation.image=&job_image;computation.vm=&vm;computation.maps=maps;computation.map_count=map_count;
    /* Incorrect PA correspondence and workload aliases refuse before binding. */
    uint64_t saved=backing_pages[0];backing_pages[0]+=4096;
    configASSERT(!pvr_compute_service_bind(&refused_compute,&service,&config,&computation));backing_pages[0]=saved;
    uint64_t map_pa=maps[0].physical;maps[0].physical=computation.firmware[0].pages[0];
    configASSERT(!pvr_compute_service_bind(&refused_compute,&service,&config,&computation));
    maps[0].physical=map_pa;
    configASSERT(!refused_compute.phase&&!config.compute);
    configASSERT(!pvr_compute_service_bind(&refused_compute,(void *)((uintptr_t)&service+1),&config,&computation));
    configASSERT(!pvr_compute_service_bind(&refused_compute,&service,(void *)((uintptr_t)&config+1),&computation));
    configASSERT(pvr_compute_service_bind(&compute,&service,&config,&computation));
}
static int kick(void *ctx,uint32_t off,uint32_t value)
{
    unsigned next=(unsigned)word(resource(0),4),slot=(next-1)&127;
    unsigned char *command=resource(1)+slot*88;
    if(word(command,4)!=(PVR_CCB_MAGIC|101u))return firmware_fixture_kick(ctx,off,value);
    callback(ctx);configASSERT(off==0xb00&&!value&&compute.phase==PVR_COMPUTE_SERVICE_RUNNING);
    configASSERT(vm.phase==A733_GPU_VM_MEMORY_PUBLISHED&&get(&published));
    configASSERT(word(command+8,4)==job_config.firmware_context);
    unsigned char *ring=(unsigned char *)computation.firmware[3].storage.cpu+compute.client.cursor-184;
    configASSERT(word(ring,4)==(PVR_CCB_MAGIC|PVR_CCCB_TASK|205u));
    configASSERT(!memcmp(ring+40,job_image.compute,96)&&!memcmp(ring+176,compute.job.update,8));
    put(computation.firmware[2].storage.cpu,4,word(command+12,4));
    put(resource(0)+128,4,next);put(resource(2)+slot*4,4,1);set(&job_kicks,get(&job_kicks)+1);return 1;
}
static int event(void *ctx,const unsigned char command[88])
{ callback(ctx);configASSERT(word(command,4)==(PVR_CCB_MAGIC|106));set(&event_forwarded,1);return 1; }
static void completion_model(void)
{
    unsigned char *out=job_config.buffers[PVR_JOB_OUTPUT].cpu;
#ifdef PVR_COMPUTE_SERVICE_PACKAGE_FIXTURE
    if(compute_service_case==12) {
        memcpy(out+64,fixture_expected,fixture_package.output_bytes);
        __asm__ volatile("dsb sy":::"memory");put(computation.firmware[5].storage.cpu,4,compute.job.sequence);return;
    }
#endif
    for(unsigned i=0;i<32;++i)put(out+64+4*i,4,(17u+(i+1u)*3u)^(0x9e3779b9u+(i<<16)));
    __asm__ volatile("dsb sy":::"memory");put(computation.firmware[5].storage.cpu,4,compute.job.sequence);
}
static void compute_peer(void *unused)
{
    (void)unused;wait_flag(&job_kicks,1);
    configASSERT(request(PVR_GPU_SERVICE_COMPUTE,100000).result==PVR_GPU_SERVICE_DENIED);
    configASSERT(request(PVR_GPU_SERVICE_CLOSE,100000).result==PVR_GPU_SERVICE_DENIED);
    configASSERT(request(PVR_GPU_SERVICE_COMPUTE_OUTPUT,0).result==PVR_GPU_SERVICE_DENIED);
    configASSERT(request(PVR_GPU_SERVICE_COMPUTE_CANCEL,0).result==PVR_GPU_SERVICE_DENIED);
    set(&peer_checked,1);wait_flag(&allow_complete,1);
    if(compute_service_case==1)completion_model();
    vTaskSuspend(NULL);
}
static void compute_owner(void *unused)
{
    (void)unused;struct pvr_gpu_service_reply r=request(PVR_GPU_SERVICE_OPEN,2000000);
    configASSERT(r.result==PVR_GPU_SERVICE_OK&&r.phase==PVR_GPU_SERVICE_PUBLISHING);
    configASSERT(request(PVR_GPU_SERVICE_COMPUTE,100000).result==PVR_GPU_SERVICE_BUSY);
    if(compute_service_case==3) {
        /* Violate a retained PA descriptor after publication has begun. Native
         * AT must refuse the next data page without reaching GPU startup. */
        backing_pages[6]+=4096;
        TickType_t first=xTaskGetTickCount();
        do {r=request(PVR_GPU_SERVICE_STATUS,0);configASSERT(xTaskGetTickCount()-first<1000);vTaskDelay(1);}
        while(r.phase!=PVR_GPU_SERVICE_FAILED);
        configASSERT(r.owns_session&&compute.error==PVR_COMPUTE_SERVICE_MAPPING&&compute.buffer==6);
        configASSERT(vm.phase==A733_GPU_VM_MEMORY_PREPARED&&!get(&published)&&!get(&enabled));
        unsigned before=get(&calls);vTaskDelay(5);configASSERT(get(&calls)==before);
        platform_puts("PASS compute service case ");platform_hex(compute_service_case);
        platform_puts("; partial publication fault retains backing before GPU startup\n");platform_exit(0);
    }
    if(compute_service_case==4) {
        r=wait_phase(PVR_GPU_SERVICE_FAILED);
        configASSERT(r.owns_session&&compute.error==PVR_COMPUTE_SERVICE_ADMISSION&&mode_calls==1);
        configASSERT(get(&enabled)&&!get(&published)&&!job_kicks);
        unsigned before=get(&calls);vTaskDelay(5);configASSERT(get(&calls)==before);
        platform_puts("PASS compute service case ");platform_hex(compute_service_case);
        platform_puts("; mode refusal retains backing before firmware startup\n");platform_exit(0);
    }
    r=wait_phase(PVR_GPU_SERVICE_READY);configASSERT(r.compute_phase==PVR_COMPUTE_SERVICE_READY);
    configASSERT(vm.phase==A733_GPU_VM_MEMORY_PUBLISHED&&compute.buffer==23);
#ifdef PVR_COMPUTE_SERVICE_PACKAGE_FIXTURE
    if(compute_service_case==12) {
        configASSERT(request(PVR_GPU_SERVICE_COMPUTE,1000000).result==PVR_GPU_SERVICE_OK);
        wait_flag(&peer_checked,1);set(&allow_complete,1);completion_model();
        do{vTaskDelay(1);r=request(PVR_GPU_SERVICE_STATUS,0);}while(r.compute_phase!=PVR_COMPUTE_SERVICE_COMPLETE);
        unsigned total=fixture_package.output_bytes/4;
        for(unsigned first=0;first<total;first+=32) {
            uint64_t ticket;configASSERT(pvr_gpu_service_compute_read(&service,first,1,&ticket));
            while(!pvr_gpu_service_collect(&service,ticket,&r,1))vTaskDelay(1);
            unsigned n=total-first<32?total-first:32;
            configASSERT(r.result==PVR_GPU_SERVICE_OK&&r.compute_first_word==first&&r.compute_word_count==n&&
                r.compute_total_words==total&&r.compute_sequence==1&&!memcmp(r.compute_words,fixture_expected+first*4,n*4));
        }
        configASSERT(request(PVR_GPU_SERVICE_CLOSE,1000000).result==PVR_GPU_SERVICE_OK);
        r=wait_phase(PVR_GPU_SERVICE_STOPPED);configASSERT(r.compute_phase==PVR_COMPUTE_SERVICE_COMPLETE);
        platform_puts("PASS compute service case ");platform_hex(compute_service_case);
        platform_puts("; native package publication and paged copied output with modeled GPU writes\n");platform_exit(0);
    }
#endif
    if(compute_service_case==5) {
        configASSERT(mode_calls==1);
        r=request(PVR_GPU_SERVICE_COMPUTE,500000);
        configASSERT(r.result==PVR_GPU_SERVICE_FAULT&&r.owns_session&&mode_calls==2&&!job_kicks);
        configASSERT(compute.error==PVR_COMPUTE_SERVICE_ADMISSION&&vm.phase==A733_GPU_VM_MEMORY_PUBLISHED);
        unsigned before=get(&calls);vTaskDelay(5);configASSERT(get(&calls)==before);
        platform_puts("PASS compute service case ");platform_hex(compute_service_case);
        platform_puts("; renewed mode refusal precedes client-ring writes\n");platform_exit(0);
    }
    configASSERT(request(PVR_GPU_SERVICE_COMPUTE,500000).result==PVR_GPU_SERVICE_OK);
    if(compute_service_case==6) {
        wait_flag(&job_kicks,1);
        configASSERT(mode_calls==2&&compute.phase==PVR_COMPUTE_SERVICE_RUNNING);
        platform_puts("PASS compute service case ");platform_hex(compute_service_case);
        platform_puts("; fresh mode admission precedes startup and COMPUTE\n");platform_exit(0);
    }
    wait_flag(&job_kicks,1);wait_flag(&peer_checked,1);
    r=request(PVR_GPU_SERVICE_STATUS,0);configASSERT(r.compute_phase==PVR_COMPUTE_SERVICE_RUNNING);
    configASSERT(request(PVR_GPU_SERVICE_CLOSE,100000).result==PVR_GPU_SERVICE_BUSY);
    configASSERT(request(PVR_GPU_SERVICE_COMPUTE,100000).result==PVR_GPU_SERVICE_BUSY);
    if(compute_service_case>=7) {
        completion_model();
        do {vTaskDelay(1);r=request(PVR_GPU_SERVICE_STATUS,0);}
        while(r.compute_phase!=PVR_COMPUTE_SERVICE_COMPLETE);
        r=request(PVR_GPU_SERVICE_COMPUTE_OUTPUT,0);
        configASSERT(r.result==PVR_GPU_SERVICE_OK&&r.compute_sequence==1&&r.compute_word_count==32);
        for(unsigned i=0;i<32;++i)configASSERT(r.compute_words[i]==job_image.expected[i]);
        compute.job.sequence=UINT32_MAX;
        configASSERT(!pvr_compute_job_repeat(&compute.job,compute.job.deadline+100000));
        configASSERT(compute.job.phase==PVR_JOB_COMPLETE&&compute.client.serial==1);
        compute.job.sequence=1;
        configASSERT(request(PVR_GPU_SERVICE_COMPUTE,100000).result==PVR_GPU_SERVICE_OK);
        configASSERT(request(PVR_GPU_SERVICE_COMPUTE_OUTPUT,0).result==PVR_GPU_SERVICE_BUSY);
        wait_flag(&job_kicks,2);
        configASSERT(compute.client.serial==2&&compute.client.write==368&&compute.job.sequence==2);
        /* New publication must poison prior results before the second KICK. */
        for(unsigned i=0;i<32;++i)configASSERT(word((unsigned char *)job_config.buffers[PVR_JOB_OUTPUT].cpu+64+4*i,4)==~job_image.expected[i]);
        if(compute_service_case==7||compute_service_case==11)completion_model();
        else if(compute_service_case==9)put(computation.firmware[5].storage.cpu,4,3);
        else if(compute_service_case==10) {
            r=request(PVR_GPU_SERVICE_COMPUTE_CANCEL,0);
            configASSERT(r.result==PVR_GPU_SERVICE_OK&&r.owns_session&&r.phase==PVR_GPU_SERVICE_FAILED);
            configASSERT(compute.error==PVR_COMPUTE_SERVICE_CANCELLED);
        }
        do {vTaskDelay(1);r=request(PVR_GPU_SERVICE_STATUS,0);}
        while(r.compute_phase!=((compute_service_case==7||compute_service_case==11)?PVR_COMPUTE_SERVICE_COMPLETE:PVR_COMPUTE_SERVICE_FAILED));
        if(compute_service_case==7||compute_service_case==11) {
            r=request(PVR_GPU_SERVICE_COMPUTE_OUTPUT,0);
            configASSERT(r.result==PVR_GPU_SERVICE_OK&&r.compute_sequence==2&&r.compute_word_count==32);
            for(unsigned i=0;i<32;++i)configASSERT(r.compute_words[i]==job_image.expected[i]);
            if(compute_service_case==11) {
                unsigned char *command=resource(PVR_FW_EVENTS);memset(command,0,88);
                put(command,4,PVR_CCB_MAGIC|105u);
                __atomic_store_n((uint32_t *)resource(PVR_FW_EVENT_CONTROL),1,__ATOMIC_RELEASE);
                r=wait_phase(PVR_GPU_SERVICE_FAILED);
                configASSERT(compute.error==PVR_COMPUTE_SERVICE_RESET);
            }
        }
        if(compute_service_case!=7) {
            configASSERT(request(PVR_GPU_SERVICE_COMPUTE_OUTPUT,0).result==PVR_GPU_SERVICE_FAULT);
            configASSERT(request(PVR_GPU_SERVICE_COMPUTE,100000).result==PVR_GPU_SERVICE_FAULT);
            configASSERT(vm.phase==A733_GPU_VM_MEMORY_PUBLISHED&&!get(&isolated));
            unsigned before=get(&calls);vTaskDelay(5);configASSERT(get(&calls)==before);
            if(compute_service_case==8)configASSERT(compute.error==PVR_COMPUTE_SERVICE_CLOCK&&
                word(computation.firmware[5].storage.cpu,4)==1&&compute.job.sequence==2);
            if(compute_service_case==9)configASSERT(compute.job.error==PVR_JOB_FENCE_VALUE);
        }
        platform_puts("PASS compute service case ");platform_hex(compute_service_case);
        platform_puts("; repeated native publication and acquired copied results or retained fault\n");platform_exit(0);
    }
    unsigned char *command=resource(PVR_FW_EVENTS);memset(command,0,88);
    put(command,4,PVR_CCB_MAGIC|(compute_service_case==2?105u:106u));
    __atomic_store_n((uint32_t *)resource(PVR_FW_EVENT_CONTROL),1,__ATOMIC_RELEASE);
    if(compute_service_case==1)wait_flag(&event_forwarded,1);
    set(&allow_complete,1);
    TickType_t start=xTaskGetTickCount();
    do {r=request(PVR_GPU_SERVICE_STATUS,0);configASSERT(xTaskGetTickCount()-start<1000);vTaskDelay(1);}
    while(r.compute_phase!=(compute_service_case==1?PVR_COMPUTE_SERVICE_COMPLETE:PVR_COMPUTE_SERVICE_FAILED));
    if(compute_service_case==1) {
        configASSERT(r.result==PVR_GPU_SERVICE_OK&&r.owns_session);
        configASSERT(request(PVR_GPU_SERVICE_CLOSE,1000000).result==PVR_GPU_SERVICE_OK);
        configASSERT(!wait_phase(PVR_GPU_SERVICE_STOPPED).owns_session);
        configASSERT(request(PVR_GPU_SERVICE_OPEN,1000000).result==PVR_GPU_SERVICE_BUSY);
        configASSERT(vm.phase==A733_GPU_VM_MEMORY_PUBLISHED&&compute.phase==PVR_COMPUTE_SERVICE_COMPLETE);
    } else {
        configASSERT(r.result==PVR_GPU_SERVICE_FAULT&&r.owns_session&&compute.error==PVR_COMPUTE_SERVICE_RESET);
        unsigned before=get(&calls);vTaskDelay(5);configASSERT(get(&calls)==before);
        configASSERT(vm.phase==A733_GPU_VM_MEMORY_PUBLISHED&&!get(&isolated));
    }
    platform_puts("PASS compute service case ");platform_hex(compute_service_case);
    platform_puts("; native publication, serialized queues, coherent results or retained reset fault\n");platform_exit(0);
}
void kernel_main(void)
{
    platform_init();first_publish=0;
    config.firmware=gpu_firmware;config.firmware_bytes=(size_t)(gpu_firmware_end-gpu_firmware);
    config.state=(struct pvr_fw_state_config){PVR_FW_A733_BVNC,123456789,1,16};
    config.queue=(struct pvr_ccb_ops){read_mem,write_mem,barrier,kick};
    config.start=(struct pvr_gpu_start_ops){now,read_reg,write_reg,started,publish};
    config.control=(struct pvr_gpu_control_ops){now,irq_read,irq_write,compat,event};
    config.stop=(struct pvr_gpu_stop_ops){now,read_reg,write_reg,sync_write,sync_read,isolate,NULL};
    config.enable=enable;config.context=&config;
    unsigned byte=0,page=0;
    for(unsigned i=0;i<22;++i) {
        unsigned n=i<2?(i?20480:102400):i==2?16384:pvr_fw_state_layout((enum pvr_fw_resource)(i-3))->allocation_bytes;
        unsigned char *cpu=i<3?arena+byte:(void *)(uintptr_t)(0x46000000+byte);
        struct pvr_mips_buffer *b=i<3?&config.firmware_buffers[i]:&config.resources[i-3];
        *b=(struct pvr_mips_buffer){{cpu,n},pages+page,n/4096};
        for(unsigned k=0;k<n/4096;++k)pages[page++]=(uintptr_t)(cpu+k*4096);
        byte+=n+4096;
    }
    configASSERT(a733_gpu_memory_init(&memory,config.firmware_buffers,config.resources));
    workload();configASSERT(!pvr_gpu_service_init(&service,&config,4,1));
    config.compute_mode=observed_mode;
    configASSERT(pvr_gpu_service_init(&service,&config,4,1));
    TaskHandle_t a=xTaskCreateStaticAffinitySet(compute_owner,"compute-owner",4096,NULL,5,stacks[0],&tasks[0],1);
    TaskHandle_t b=xTaskCreateStaticAffinitySet(compute_peer,"compute-peer",4096,NULL,5,stacks[1],&tasks[1],2);
    configASSERT(a&&b&&pvr_gpu_service_bind(&service,0,a)&&pvr_gpu_service_bind(&service,1,b));
    vTaskStartScheduler();platform_panic("scheduler returned",0);
}
