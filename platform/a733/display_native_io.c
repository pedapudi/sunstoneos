#include "a733_display_native.h"
#include "a733_platform_locks.h"
#include "aarch64_device_memory.h"
#include "task.h"
int a733_display_native_owner(void *task)
{
    uint64_t el;__asm__ volatile("mrs %0, CurrentEL":"=r"(el));
    return el==4&&task&&xTaskGetSchedulerState()==taskSCHEDULER_RUNNING&&xTaskGetCurrentTaskHandle()==task;
}
int a733_display_native_mapping(uintptr_t cpu,uint64_t physical,unsigned attributes)
{
    struct aarch64_translation r,w;
    return aarch64_device_translate(cpu,0,&r)&&aarch64_device_translate(cpu,1,&w)&&
        r.physical==physical&&w.physical==physical&&r.attributes==attributes&&w.attributes==attributes&&
        r.shareability==w.shareability&&(!attributes||r.shareability==2||r.shareability==3);
}
int a733_display_native_enter(struct a733_platform_locks *l,uint64_t d)
{return a733_platform_lock_enter(l,A733_PLATFORM_REGISTER_LOCK,d);}
void a733_display_native_leave(struct a733_platform_locks *l)
{a733_platform_lock_leave(l,A733_PLATFORM_REGISTER_LOCK);}
void a733_display_native_retain(struct a733_platform_locks *l)
{(void)a733_platform_lock_retain(l,A733_PLATFORM_REGISTER_LOCK);}
int a733_display_native_barrier(void)
{__asm__ volatile("dsb sy":::"memory");return 1;}
int a733_display_native_read(uintptr_t a,uint32_t *out)
{a733_display_native_barrier();*out=*(volatile const uint32_t *)a;return a733_display_native_barrier();}
int a733_display_native_write(uintptr_t a,uint32_t value)
{a733_display_native_barrier();*(volatile uint32_t *)a=value;return a733_display_native_barrier();}
