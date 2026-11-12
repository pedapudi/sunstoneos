/* Host case for the task-creation hook of include/os_space.h: the space of a new task comes from its creator's tag or from the
 * explicit creation call, never from a global tag. A direct creation in the init hook, where nothing creates, is refused. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define configNUMBER_OF_CORES 8
struct tcb { const void *pxTaskTag; };
static unsigned core;
#define portGET_CORE_ID() core
#include "os_space.h"
uint64_t os_space_kernel_ttbr;
uint8_t os_space_active;
volatile uint8_t os_space_app_init_active;
volatile uint8_t os_space_explicit[configNUMBER_OF_CORES];
static unsigned refusals;
void os_space_refuse_create(void) { ++refusals; }
static uint64_t app_ttbr;
int main(void)
{
    struct tcb creator = {&app_ttbr}, kernel_creator = {0}, made = {0};
    /* A running application task creates directly: the new task takes its space. */
    os_space_inherit(&creator, &made);
    assert(made.pxTaskTag == &app_ttbr && !refusals);
    /* A running kernel task creates: no space. */
    made.pxTaskTag = 0; os_space_inherit(&kernel_creator, &made);
    assert(!made.pxTaskTag && !refusals);
    /* Before the scheduler, outside the init hook (kernel boot): no creator, no space, no refusal. */
    os_space_inherit((struct tcb *)0, &made);
    assert(!made.pxTaskTag && !refusals);
    /* In the init hook a direct creation is refused and gets no space... */
    os_space_app_init_active = 1;
    os_space_inherit((struct tcb *)0, &made);
    assert(!made.pxTaskTag && refusals == 1);
    /* ...but a creation call that names its space (the mark of that core) is not. */
    os_space_explicit[core] = 1;
    os_space_inherit((struct tcb *)0, &made);
    assert(refusals == 1);
    os_space_explicit[core] = 0;
    /* The mark of another core does not excuse this core. */
    core = 3; os_space_explicit[1] = 1;
    os_space_inherit((struct tcb *)0, &made);
    assert(refusals == 2);
    /* The switch hook reads the tag as an address, with no mark bit. */
    puts("os-space-create host case passed");
    return 0;
}
