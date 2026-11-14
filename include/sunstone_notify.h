#ifndef SUNSTONE_NOTIFY_H
#define SUNSTONE_NOTIFY_H
/* Task notification indices (configTASK_NOTIFICATION_ARRAY_ENTRIES is 3).
 *
 * Index 0 belongs to the task that owns it. For a task of an application, the main task and every task the
 * application creates, it is the application's alone: the FreeRTOS calls without an index. No OS, SDK or runtime
 * code gives or takes it for such a task. An OS service task owns its own index 0 for its own waits.
 *
 * Index 1 is the completion wait of a caller on a request that an OS service serves for it: the SD owner's
 * callers (apps/os/sd_owner.c). Legacy GPU diagnostic tasks also use index 1.
 * A task sleeps on index 1 for one such request at a time.
 *
 * Index 2 is the wait of an application task inside the runtime and the OS: the deadline wait
 * (runtime/deadline.c, whose timer interrupt and device hints give it), ss_large_wait and the OS waits that
 * register with os_wake (apps/os/application.c). Native GPU fence and lifecycle waits share this index with
 * their precise timeout. SS_NOTIFY_DEADLINE is the shared definition for the native GPU completion producers,
 * the trusted peer's deadline selector and its startup-ready producer. The peer has no application-space tag.
 * Each wait takes only this index on its way out. */
#define SS_NOTIFY_OWN 0u
#define SS_NOTIFY_REQUEST_WAIT 1u
#define SS_NOTIFY_DEADLINE 2u
#endif
