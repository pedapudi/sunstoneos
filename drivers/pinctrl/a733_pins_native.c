#include "a733_pins_native.h"

static int native_enter(void *cookie, uint64_t deadline)
{
    struct a733_pins_native *n=cookie;
    return n->locks.enter(n->locks.cookie,deadline);
}
static void native_leave(void *cookie)
{
    struct a733_pins_native *n=cookie;
    n->locks.leave(n->locks.cookie);
}
static int native_now(void *cookie, uint64_t *value)
{
    struct a733_pins_native *n=cookie;
    return n->locks.now(n->locks.cookie,value);
}
static int native_read(void *cookie, uintptr_t address, uint32_t *value)
{
    (void)cookie;
    if (!value || !a733_pins_native_address(address,0)) return 0;
    __asm__ volatile("dsb sy" ::: "memory");
    *value=*(volatile uint32_t *)address;
    __asm__ volatile("dsb sy" ::: "memory");
    return 1;
}
static int native_write(void *cookie, uintptr_t address, uint32_t value)
{
    (void)cookie;
    if (!a733_pins_native_address(address,1)) return 0;
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)address=value;
    __asm__ volatile("dsb sy" ::: "memory");
    return 1;
}
int a733_pins_native_init(struct a733_pins_native *n, const struct a733_pins_locks *locks)
{
    if (!n || n->initialized || !locks || !locks->enter || !locks->leave || !locks->now) return 0;
    n->locks=*locks;
    n->operations=(struct a733_pins_operations){native_enter,native_leave,native_now,native_read,native_write};
    n->initialized=1;
    return 1;
}
