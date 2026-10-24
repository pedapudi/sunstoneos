/* Include the production unit so its private identity helper can be checked
 * without exposing a general hashing API. The native object compiles separately. */
#include "../drivers/npu/npu_convolution.c"
#include "npu-convolution-data.h"
#include "platform.h"

static struct npu_convolution convolution,rejected;
static struct a733_npu_startup startup_record;
static _Alignas(4096) unsigned char check_arena[20480];
static _Alignas(4096) unsigned char check_small[4][4096];
static _Alignas(4096) unsigned char check_commands[2][524288];
static unsigned char check_model[NPU_CONVOLUTION_MODEL_BYTES];
static StaticTask_t check_tasks[2];
static StackType_t check_stacks[2][4096];
static TaskHandle_t check_bootstrap,check_client;
static atomic_uint check_allow,check_accesses,check_publishes,check_finishes;
static unsigned check_activation_mutation,check_prepared_calls,check_job;
static uint32_t check_registers[0x800/4];
static unsigned check_running,check_is_bootstrap;
extern const struct a733_npu_ops __real_a733_npu_native_operations;
static const struct a733_npu_ops *check_native=&__real_a733_npu_native_operations;
int __real_aarch64_device_translate(uintptr_t,int,struct aarch64_translation *);
int __wrap_aarch64_device_translate(uintptr_t address,int write,struct aarch64_translation *out)
{
    int result=__real_aarch64_device_translate(address,write,out);
    /* Model only the MMIO physical address; real CPU translation/cache policy
     * remains active for all RAM and actual hardware is never accessed. */
    if(result && address>=0x09000000 && address<0x09001000)out->physical=address-0x09000000+0x03600000;
    return result;
}
static void check_privileged(void)
{
    uint64_t level;__asm__ volatile("mrs %0,CurrentEL":"=r"(level));
    configASSERT(level==4 && xTaskGetCurrentTaskHandle()==convolution.service.worker);
}
static void check_fatal(void *unused){(void)unused;platform_panic("convolution native adapter",0);}
static int check_read(void *cookie,unsigned offset,uint32_t *out)
{
    configASSERT(cookie==&convolution.native);check_privileged();atomic_fetch_add(&check_accesses,1);
    if(check_running && (check_is_bootstrap || atomic_load(&check_allow))) {
        check_registers[0x10/4]=1;check_registers[1]=0x7fffffff;check_running=0;
    }
    *out=check_registers[offset/4];if(offset==0x10)check_registers[offset/4]=0;
    return 1;
}
static int check_write(void *cookie,unsigned offset,uint32_t value)
{
    configASSERT(cookie==&convolution.native);check_privileged();atomic_fetch_add(&check_accesses,1);
    check_registers[offset/4]=value;
    if(offset==0x3a8 && (value&1)){check_registers[0x388/4]=0;check_registers[1]=0x7fffffff;}
    if(offset==0x3a4) {
        configASSERT(value==0x1ffff && convolution.native.memory.state==A733_NPU_MEMORY_DEVICE);
        check_is_bootstrap=check_registers[0x654/4]==convolution.commands[0].device;
        check_running=1;check_registers[1]=0;
    }
    return 1;
}
static uint64_t check_now(void *cookie){check_privileged();return check_native->microseconds(cookie);}
static void check_delay(void *cookie,unsigned us){check_privileged();check_native->delay(cookie,us);}
static void check_barrier(void *cookie){check_privileged();check_native->barrier(cookie);}
static int check_publish(void *cookie,const struct a733_npu_mmu_image *image,
    const struct a733_npu_command *command,int boot)
{
    check_privileged();atomic_fetch_add(&check_publishes,1);
    if(!boot) {
        ++check_job;
        for(unsigned i=0;i<192;++i)configASSERT(check_small[2][i]==
            (check_job==1 ? fixture_input[i] : (unsigned char)(fixture_input[i]^0xff)));
    }
    return check_native->publish(cookie,image,command,boot);
}
static int check_finish(void *cookie,const struct a733_npu_command *command,int canceled)
{
    check_privileged();configASSERT(!check_running && !canceled);
    /* Device stand-in, not a numerical implementation or inference result. */
    if(!check_is_bootstrap) {
        if(check_job==1)memcpy(check_small[3],fixture_output,72);
        else memset(check_small[3],0x5a,72);
    }
    int ok=check_native->finish(cookie,command,canceled);
    if(ok)atomic_fetch_add(&check_finishes,1);
    return ok;
}
const struct a733_npu_ops __wrap_a733_npu_native_operations={
    check_read,check_write,check_now,check_delay,check_barrier,check_publish,check_finish};
/* Startup health is a modeled boundary here; checks/npu-startup.c and
 * checks/npu-startup-host.c run the sequencer. */
int __wrap_a733_npu_startup_prepared(struct a733_npu_startup *startup)
{
    configASSERT(startup==&startup_record && xTaskGetCurrentTaskHandle()==check_bootstrap);
    ++check_prepared_calls;return 1;
}
static struct npu_service_reply check_request(enum npu_service_operation op,const void *input)
{
    uint64_t ticket;struct npu_service_reply reply;
    configASSERT(npu_convolution_send(&convolution,op,input,input ? 192 : 0,
        op==NPU_SERVICE_OPEN || op==NPU_SERVICE_SUBMIT ? 5000000 : 0,100,&ticket));
    configASSERT(npu_convolution_collect(&convolution,ticket,&reply,2000));return reply;
}
static struct npu_service_reply check_wait(enum a733_npu_state state)
{
    for(unsigned i=0;i<100;++i) {
        struct npu_service_reply r=check_request(NPU_SERVICE_STATUS,NULL);
        if(r.state==state)return r;
        configASSERT(r.result==NPU_SERVICE_OK);vTaskDelay(1);
    }
    configASSERT(0);return (struct npu_service_reply){0};
}
static void check_client_run(void *unused)
{
    (void)unused;uint64_t ignored=999;unsigned char input[192],output[72];
    memset(output,0xa5,sizeof(output));
    configASSERT(!npu_convolution_send(&convolution,NPU_SERVICE_OPEN,NULL,0,5000000,0,&ignored));
    configASSERT(ignored==999 && !atomic_load(&check_accesses));
    /* Another task cannot step the startup. */
    configASSERT(npu_convolution_startup_poll(&convolution)==A733_NPU_PLATFORM_RETAINED);
    configASSERT(!atomic_load(&convolution.failed));
    xTaskNotifyGive(check_bootstrap);configASSERT(ulTaskNotifyTake(pdTRUE,2000)==1);
    if(check_activation_mutation==1) {
        /* The bootstrap publication hashes every immutable byte before the
         * device fetches the stream, so a changed command tail stops there. */
        uint64_t open;struct npu_service_reply reply;
        configASSERT(check_prepared_calls==1 && !atomic_load(&convolution.failed));
        configASSERT(npu_convolution_send(&convolution,NPU_SERVICE_OPEN,NULL,0,5000000,0,&open));
        (void)npu_convolution_collect(&convolution,open,&reply,2000);
        configASSERT(atomic_load(&convolution.failed) && !atomic_load(&check_publishes));
        configASSERT(!npu_convolution_output(&convolution,output,sizeof(output)));
        platform_puts("PASS fixed convolution activation: a mutated private command tail is refused at the bootstrap publication, before the device fetches it; reservations retained\n");
        platform_exit(0);
    }
    uint64_t ticket;struct npu_service_reply reply={.ticket=77};
    configASSERT(npu_convolution_send(&convolution,NPU_SERVICE_OPEN,NULL,0,5000000,0,&ticket));
    configASSERT(!npu_convolution_collect(&convolution,ticket,&reply,0) && reply.ticket==77);
    configASSERT(!npu_convolution_send(&convolution,NPU_SERVICE_STATUS,NULL,0,0,0,&ignored));
    configASSERT(!npu_convolution_collect(&convolution,ticket+1,&reply,0));
    configASSERT(npu_convolution_collect(&convolution,ticket,&reply,2000));
    configASSERT(reply.result==NPU_SERVICE_OK);check_wait(A733_NPU_READY);
    configASSERT(!npu_convolution_output(&convolution,output,sizeof(output)));
    memcpy(input,fixture_input,sizeof(input));
    configASSERT(!npu_convolution_send(&convolution,NPU_SERVICE_SUBMIT,input,191,5000000,0,&ignored));
    configASSERT(npu_convolution_send(&convolution,NPU_SERVICE_SUBMIT,input,192,5000000,0,&ticket));
    memset(input,0,sizeof(input)); /* Queue publication must use the private input copy. */
    configASSERT(!npu_convolution_send(&convolution,NPU_SERVICE_SUBMIT,input,192,5000000,0,&ignored));
    configASSERT(npu_convolution_collect(&convolution,ticket,&reply,2000));
    configASSERT(reply.result==NPU_SERVICE_OK && reply.state==A733_NPU_RUNNING);
    configASSERT(!npu_convolution_output(&convolution,output,sizeof(output)));
    for(unsigned i=0;i<sizeof(output);++i)configASSERT(output[i]==0xa5);
    configASSERT(!npu_convolution_send(&convolution,NPU_SERVICE_REAP,NULL,0,0,0,&ignored));
    configASSERT(!npu_convolution_send(&convolution,NPU_SERVICE_CLOSE,NULL,0,0,0,&ignored));
    atomic_store(&check_allow,1);reply=check_wait(A733_NPU_DONE);
    configASSERT(reply.job_ticket==ticket && atomic_load(&check_finishes)==2);
    configASSERT(!npu_convolution_send(&convolution,NPU_SERVICE_CLOSE,NULL,0,0,0,&ignored));
    configASSERT(npu_convolution_output(&convolution,output,sizeof(output)));
    configASSERT(!memcmp(output,fixture_output,sizeof(output)));
    configASSERT(check_request(NPU_SERVICE_REAP,NULL).state==A733_NPU_READY);
    configASSERT(!npu_convolution_output(&convolution,output,sizeof(output)));
    for(unsigned i=0;i<sizeof(input);++i)input[i]=fixture_input[i]^0xff;
    if(check_activation_mutation==5) {
        /* A job publication still binds the command padding. */
        unsigned publications=atomic_load(&check_publishes);
        check_commands[0][524280]^=1;
        reply=check_request(NPU_SERVICE_SUBMIT,input);
        configASSERT(reply.result==NPU_SERVICE_FAULT && reply.fault==A733_NPU_MEMORY);
        configASSERT(atomic_load(&convolution.failed) && atomic_load(&check_publishes)==publications);
        platform_puts("PASS convolution job integrity: mutated command padding refused before publication\n");
        platform_exit(0);
    }
    /* The device never reads the private model copy, and the bootstrap stream
     * runs only at OPEN, so a job publication omits both from its digests. */
    convolution.model[0]^=1;check_commands[1][524287]^=1;
    check_request(NPU_SERVICE_SUBMIT,input);check_wait(A733_NPU_DONE);
    convolution.model[0]^=1;check_commands[1][524287]^=1;
    configASSERT(npu_convolution_output(&convolution,output,sizeof(output)));
    for(unsigned i=0;i<sizeof(output);++i)configASSERT(output[i]==0x5a);
    check_request(NPU_SERVICE_REAP,NULL);
    if(check_activation_mutation>=3) {
        configASSERT(!npu_convolution_send(&convolution,NPU_SERVICE_CLOSE,NULL,0,1,0,&ignored));
        if(check_activation_mutation==4)check_registers[0x388/4]^=2;
        reply=check_request(NPU_SERVICE_CLOSE,NULL);
        if(check_activation_mutation==3) {
            configASSERT(reply.result==NPU_SERVICE_OK && reply.state==A733_NPU_CLOSED &&
                !reply.owns_session && !reply.job_ticket && atomic_load(&convolution.closed));
            configASSERT(!convolution.ready && !atomic_load(&convolution.failed));
            configASSERT(!check_registers[0x14/4] && !check_registers[0x388/4]);
        } else configASSERT(reply.result==NPU_SERVICE_FAULT && reply.owns_session &&
            !atomic_load(&convolution.closed) && atomic_load(&convolution.failed));
        unsigned accesses=atomic_load(&check_accesses);
        configASSERT(!npu_convolution_send(&convolution,NPU_SERVICE_OPEN,NULL,0,5000000,0,&ignored));
        configASSERT(!npu_convolution_send(&convolution,NPU_SERVICE_STATUS,NULL,0,0,0,&ignored));
        configASSERT(!npu_convolution_send(&convolution,NPU_SERVICE_CLOSE,NULL,0,0,0,&ignored));
        configASSERT(!npu_convolution_output(&convolution,output,sizeof(output)));
        configASSERT(npu_convolution_stop_poll(&convolution)==A733_NPU_PLATFORM_RETAINED);
        vTaskDelay(2);configASSERT(atomic_load(&check_accesses)==accesses);
        platform_puts(check_activation_mutation==3?"PASS convolution terminal CLOSE: matched CLOSED reply, no job/session, IRQ/MMU disabled; further admission refused\n":
            "PASS convolution CLOSE fault: exact MMU mismatch retains ownership, no closed publication or later access\n");
        platform_exit(0);
    }
    unsigned publications=atomic_load(&check_publishes),accesses=atomic_load(&check_accesses);
    check_small[1][255]^=1; /* Accidental coefficient mutation after activation. */
    reply=check_request(NPU_SERVICE_SUBMIT,input);
    configASSERT(reply.result==NPU_SERVICE_FAULT && reply.fault==A733_NPU_MEMORY && reply.owns_session);
    configASSERT(atomic_load(&convolution.failed) && convolution.job && !convolution.pending);
    configASSERT(atomic_load(&check_publishes)==publications && atomic_load(&check_accesses)==accesses);
    configASSERT(!npu_convolution_output(&convolution,output,sizeof(output)));
    configASSERT(!npu_convolution_send(&convolution,NPU_SERVICE_REAP,NULL,0,0,0,&ignored));
    vTaskDelay(2);configASSERT(atomic_load(&check_accesses)==accesses);
    platform_puts("PASS fixed convolution: exact artifact/relocations/bootstrap, native MMU/cache publication, copied inputs, matched DONE/output/REAP, varied input, mutation retained before publication\n");
    platform_exit(0);
}
static void check_bootstrap_run(void *unused)
{
    (void)unused;configASSERT(ulTaskNotifyTake(pdTRUE,2000)==1);
    if(check_activation_mutation==1)check_commands[0][524287]^=1;
    configASSERT(npu_service_activate(&convolution.service)==NPU_SERVICE_OK);
    xTaskNotifyGive(check_client);vTaskSuspend(NULL);configASSERT(0);
}
BaseType_t npu_convolution_task_privileged(TaskFunction_t entry,void *argument)
{return entry==check_bootstrap_run || npu_service_task_privileged(entry,argument);}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name)
{(void)task;platform_panic(name,0);}
static unsigned check_mode(void)
{
    char command[128]={0};uintptr_t params[2]={(uintptr_t)command,sizeof(command)};
    register uintptr_t op __asm__("x0")=0x15,args __asm__("x1")=(uintptr_t)params;
    __asm__ volatile("hlt #0xf000":"+r"(op):"r"(args):"memory");configASSERT(!op);
    for(unsigned i=0;i+11<sizeof(command);++i)if(!memcmp(command+i,"job-command",11))return 5;
    for(unsigned i=0;i+11<sizeof(command);++i)if(!memcmp(command+i,"close-fault",11))return 4;
    for(unsigned i=0;i+5<sizeof(command);++i)if(!memcmp(command+i,"close",5))return 3;
    for(unsigned i=0;i+8<sizeof(command);++i)if(!memcmp(command+i,"mutation",8))return 1;
    return 0;
}
void kernel_main(void)
{
    platform_init();check_activation_mutation=check_mode();
    unsigned char actual[32],a[64];memset(a,'a',sizeof(a));
    digest("",0,actual);configASSERT(!memcmp(actual,fixture_hashes[0],32));
    digest("abc",3,actual);configASSERT(!memcmp(actual,fixture_hashes[1],32));
    digest(a,56,actual);configASSERT(!memcmp(actual,fixture_hashes[2],32));
    digest(a,64,actual);configASSERT(!memcmp(actual,fixture_hashes[3],32));
    memcpy(check_model,fixture_model,sizeof(check_model));
    struct npu_convolution_config config={.model=check_model,.model_bytes=sizeof(check_model),
        .arena=check_arena,.arena_bytes=sizeof(check_arena),.arena_physical=(uintptr_t)check_arena,
        .register_cpu=0x09000000,.register_physical=0x03600000,.fatal=check_fatal};
    uintptr_t addresses[]={(uintptr_t)check_small[0],(uintptr_t)check_small[1],(uintptr_t)check_commands[0],
        (uintptr_t)check_small[2],(uintptr_t)check_small[3],(uintptr_t)check_commands[1]};
    const uint32_t devices[]={0xe0000000,0xe0002000,0xe0004000,0xe0085000,0xe0087000,0xe0089000};
    const size_t sizes[]={4096,4096,524288,4096,4096,524288};
    const enum a733_npu_buffer_role roles[]={A733_NPU_BUFFER_CONSTANT,A733_NPU_BUFFER_CONSTANT,
        A733_NPU_BUFFER_COMMAND,A733_NPU_BUFFER_READ,A733_NPU_BUFFER_WRITE,A733_NPU_BUFFER_BOOTSTRAP};
    for(unsigned i=0;i<6;++i)config.buffers[i]=(struct a733_npu_buffer){addresses[i],addresses[i],sizes[i],devices[i],roles[i]};
    check_model[20]^=1;
    configASSERT(!npu_convolution_prepare(&rejected,&config) && atomic_load(&rejected.failed) && !rejected.native.initialized);
    check_model[20]^=1;
    configASSERT(npu_convolution_prepare(&convolution,&config));
    memset(check_model,0,sizeof(check_model)); /* No borrowed model remains. */
    configASSERT(!memcmp(convolution.immutable_digest,fixture_immutable,sizeof(fixture_immutable)));
    configASSERT(convolution.commands[0].token!=convolution.commands[1].token);
    check_registers[0x20/4]=0x9000;check_registers[0x24/4]=0x9202;
    check_registers[0x30/4]=0x1000003b;check_registers[0x28/4]=0x20240101;check_registers[1]=0x7fffffff;
    check_bootstrap=xTaskCreateStaticAffinitySet(check_bootstrap_run,"conv-start",4096,NULL,4,check_stacks[0],&check_tasks[0],2);
    check_client=xTaskCreateStaticAffinitySet(check_client_run,"conv-client",4096,NULL,4,check_stacks[1],&check_tasks[1],1);
    startup_record.initialized=1;startup_record.task=check_bootstrap;
    configASSERT(npu_convolution_bind(&convolution,&startup_record,check_client,3,1));
    vTaskStartScheduler();platform_panic("scheduler returned",0);
}
