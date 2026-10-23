#include "a733_display_native.h"
#include "a733_platform_locks.h"
#include "a733_mapping_check.h"
#include "a733_mmio.h"
#include "task.h"
int a733_display_native_owner(void *task)
{
    uint64_t el;__asm__ volatile("mrs %0, CurrentEL":"=r"(el));
    return el==4&&task&&xTaskGetSchedulerState()==taskSCHEDULER_RUNNING&&xTaskGetCurrentTaskHandle()==task;
}
int a733_display_native_mapping(uintptr_t cpu,uint64_t physical,unsigned attributes)
{
    const struct a733_mapping_rule rule={A733_MAPPING_READ_WRITE,1,
        {{(uint8_t)attributes,attributes?A733_SH_OUTER|A733_SH_INNER:A733_SH_ANY}},1,1};
    return attributes<=0xffu&&a733_mapping_check(aarch64_device_translate,cpu,physical,&rule);
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
{*out=a733_mmio_read32_dsb_sy_both(a);return 1;}
int a733_display_native_write(uintptr_t a,uint32_t value)
{a733_mmio_write32_dsb_sy_both(a,value);return 1;}

/* Pinned DE352 submit rule: no yield/interrupt between busy observation and
 * update write. The caller owns the shared register mutex. This is one bounded
 * transaction; idle returns pending without writing. Existing task critical
 * primitives provide the same local exclusion as Linux local_irq_save. */
int a733_display_native_rcq_submit(void)
{
 int submitted=0;taskENTER_CRITICAL();
 uint32_t status=a733_mmio_read32(A733_DE_BASE+0x8144);
 if(status&16){uint32_t control=a733_mmio_read32(A733_DE_BASE+0x8150);
  a733_mmio_write32(A733_DE_BASE+0x8150,control|1u);submitted=1;}
 a733_display_native_barrier();taskEXIT_CRITICAL();return submitted;
}
