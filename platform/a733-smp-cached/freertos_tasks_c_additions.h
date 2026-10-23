#include "../qemu-smp/freertos_tasks_c_additions.h"
#if defined(SUNSTONE_A733_FAULT_PAGE)
/* The EL3 fault capture reads 16 name bytes at this offset of the current
 * TCB with one aligned 16-byte load (platform/a733/fault.S). */
_Static_assert(offsetof(TCB_t, pcTaskName) % 8 == 0 && configMAX_TASK_NAME_LEN >= 16,
               "task name layout for the fault capture");
const uint64_t a733_fault_task_name_offset = offsetof(TCB_t, pcTaskName);
#endif
