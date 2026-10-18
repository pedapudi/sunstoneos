/* Native C guard execution with modeled EL/task/mutex state and anonymous
 * host MMIO. The actual service reaches write authority through backup/seal;
 * execution stops before the first modeled card-media write. */
#define _GNU_SOURCE
#include "a733_platform_locks.h"
#include "task.h"
#include <setjmp.h>
#include <sys/mman.h>
static void native_boundary(uintptr_t,uint32_t);
#include "storage-native-service-model.h"
static struct a733_mmc_platform_native adapter;
static struct a733_platform_locks locks;
static void *task=(void *)1,*holder=(void *)1;
uint64_t native_model_el=4,native_model_flags;
uint64_t ullPortInterruptNesting;
volatile uint64_t ullCriticalNesting;
static jmp_buf complete;
static unsigned command_checked;
BaseType_t xTaskGetSchedulerState(void){return taskSCHEDULER_RUNNING;}
TaskHandle_t xTaskGetCurrentTaskHandle(void){return task;}
TaskHandle_t xQueueGetMutexHolder(QueueHandle_t queue){(void)queue;return holder;}
int a733_platform_lock_enter(struct a733_platform_locks *p,enum a733_platform_lock which,uint64_t deadline)
{(void)p;(void)which;(void)deadline;return 1;}
void a733_platform_lock_leave(struct a733_platform_locks *p,enum a733_platform_lock which){(void)p;(void)which;}
int a733_platform_lock_retain(struct a733_platform_locks *p,enum a733_platform_lock which)
{(void)p;(void)which;return 1;}
_Noreturn void platform_panic(const char *file,unsigned line){fprintf(stderr,"%s:%u\n",file,line);abort();}
static void refuse(uintptr_t address,uint32_t value)
{
    uint32_t before=*(volatile uint32_t *)address;
    assert(!adapter.operations.write(&adapter,address,value));
    assert(*(volatile uint32_t *)address==before);
}
static void native_boundary(uintptr_t address,uint32_t value)
{
    assert(a733_storage_update_mmio(&service,address,value));
    task=(void *)2;refuse(address,value);task=(void *)1;
    holder=(void *)2;refuse(address,value);holder=(void *)1;
    native_model_el=0;refuse(address,value);native_model_el=4;
    native_model_flags=0x80;refuse(address,value);native_model_flags=0;
    ullPortInterruptNesting=1;refuse(address,value);ullPortInterruptNesting=0;
    atomic_store(&locks.retained,1);refuse(address,value);atomic_store(&locks.retained,0);
    adapter.update=NULL;refuse(address,value);adapter.update=&service;
    assert(!adapter.operations.write(&adapter,address+0x1000,value));
    assert(adapter.operations.write(&adapter,address,value));
    assert(*(volatile uint32_t *)address==value);
    if(address==m.platform.base+0x18){assert(!command_checked);command_checked=1;return;}
    assert(command_checked&&address==m.platform.base+0x200&&!fifo_writes);
    assert(!m.platform.core.result.confirmed_blocks);
    longjmp(complete,1);
}
static void put(uint8_t *p,uint64_t value,unsigned bytes)
{for(unsigned i=0;i<bytes;++i)p[i]=(uint8_t)(value>>(8*i));}
static void request_operation(unsigned operation,const uint8_t *payload)
{
    uint8_t request[A733_STORAGE_BODY]={0};memcpy(request,service.grant.nonce,32);
    put(request+32,service.sequence+1,4);put(request+36,operation,4);
    if(operation==A733_STORAGE_SEAL)put(request+40,2,8);
    if(operation==A733_STORAGE_SEAL||operation==A733_STORAGE_WRITE)memset(request+56,7,32);
    size_t bytes=88;
    if(payload){put(request+48,1024,4);memcpy(request+120,payload,1024);bytes=1144;}
    assert(a733_storage_channel_submit(&channel,request,bytes));
    uint8_t response[A733_STORAGE_BODY];size_t n=0;
    for(unsigned i=0;i<10000&&!n;++i){progress();assert(!atomic_load(&channel.lost));a733_storage_channel_response(&channel,response,sizeof(response),&n);}
    assert(n&&operation!=A733_STORAGE_WRITE);
}
int main(void)
{
    void *page=mmap((void *)0x04020000,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    assert(page==(void *)0x04020000);
    storage_setup(SUNXI_MMC_SD);field(m.card.csd,84,12,21);field(m.card.csd,22,4,9);ready_storage();
    struct a733_storage_grant grant={.mode=1,.kind=1,.sectors=33554432,.first=2,.blocks=2,.deadline_us=9000000};
    memcpy(grant.cid,m.platform.core.card.cid,16);memset(grant.nonce,1,32);memset(grant.allocation,2,32);memset(grant.recovery,3,32);
    uint8_t candidate[1024],header[48]={0};struct storage_digest hash;
    for(unsigned i=0;i<1024;++i){media[i]=(uint8_t)(i*17+9);candidate[i]=(uint8_t)(i*31+11);}
    storage_digest_init(&hash);assert(storage_digest_add(&hash,media,sizeof(media)));storage_digest_end(&hash,grant.backup);
    put(header+8,1024,4);storage_digest_init(&hash);assert(storage_digest_add(&hash,header,sizeof(header)));
    assert(storage_digest_add(&hash,candidate,sizeof(candidate)));storage_digest_end(&hash,grant.candidate);
    a733_storage_channel_init(&channel);assert(a733_storage_update_init(&service,&m.platform,&grant,1000000));
    locks.initialized=1;locks.now=storage_now;locks.context=&m;locks.mutex[0]=(void *)1;atomic_init(&locks.retained,0);
    assert(a733_mmc_platform_native_init(&adapter,&locks,(void *)1,SUNXI_MMC_SD,storage_admit,&m));
    /* The platform model supplies real provider admission. Temporarily expose
     * the exact native endpoint solely for its one-time owner-binding check. */
    struct a733_mmc_platform_ops old=m.platform.ops;void *cookie=m.platform.cookie;
    m.platform.cookie=&adapter;m.platform.ops.write=adapter.operations.write;
    assert(a733_mmc_platform_native_bind_update(&adapter,&service));
    m.platform.cookie=cookie;m.platform.ops=old;
    refuse(m.platform.base+0x200,0x1234);refuse(m.platform.base+0x18,0x80006758);
    request_operation(A733_STORAGE_BACKUP,NULL);request_operation(A733_STORAGE_SEAL,NULL);
    if(!setjmp(complete))request_operation(A733_STORAGE_WRITE,candidate);
    assert(command_checked&&issued==1&&!fifo_writes&&!atomic_load(&channel.lost));
    assert(!munmap(page,4096));
    puts("PASS: actual service authority reaches native CMD24/FIFO guards; wrong task/mutex/EL/aperture and retained/default ownership refuse");
    return 0;
}
