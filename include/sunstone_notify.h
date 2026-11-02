#ifndef SUNSTONE_NOTIFY_H
#define SUNSTONE_NOTIFY_H
/* Task notification indices (configTASK_NOTIFICATION_ARRAY_ENTRIES is 2).
 *
 * Index 0 belongs to the task that owns it. For a task of an application, the main task and every task the
 * application creates, it is the application's: the FreeRTOS calls without an index, the deadline wait
 * (runtime/deadline.c, whose timer interrupt and device hints use it) and the SDK calls that sleep on it. The OS
 * gives a notification at index 0 to an application task only while that task sleeps in an OS wait that registered
 * for it (apps/os/application.c, os_wake), and that wait consumes what the OS gave. An OS service task owns its own
 * index 0.
 *
 * Index 1 is the completion wait of a caller on a request that an OS service serves for it: the SD owner's
 * callers (apps/os/sd_owner.c) and the GPU compute fast tasks (drivers/gpu/pvr_compute_fast.c). A task sleeps on
 * index 1 for one such request at a time. */
#define SS_NOTIFY_OWN 0u
#define SS_NOTIFY_REQUEST_WAIT 1u
#endif
