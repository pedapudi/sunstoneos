/* Included in the actual kernel translation unit; no duplicated TCB layout. */
#include <stddef.h>
const uint64_t a733_fault_task_name_offset = offsetof(TCB_t, pcTaskName);
_Static_assert(offsetof(TCB_t, pcTaskName)%8==0 && configMAX_TASK_NAME_LEN>=16,"fault task name");
