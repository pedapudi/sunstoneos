# Task notifications: who gives what to whom

An application's tasks own their notification index 0. The OS never ends an application's wait on it. This page
lists every wait that can run in an application task, the index plan, and the check that enforces the rule.
The indices are defined in [include/sunstone_notify.h](../include/sunstone_notify.h).

## Index plan (`configTASK_NOTIFICATION_ARRAY_ENTRIES` is 2)

| Index | Name | Users |
| --- | --- | --- |
| 0 | `SS_NOTIFY_OWN` | The owning task: for an application task, the application (plain FreeRTOS calls, the deadline wait, `ss_large_wait`); for an OS service task, that service |
| 1 | `SS_NOTIFY_REQUEST_WAIT` | A caller sleeping for a request that an OS service serves for it: the SD owner's callers (`apps/os/sd_owner.c`) and the GPU compute fast tasks (`drivers/gpu/pvr_compute_fast.c`) |

## What the OS gives to an application task

| Source | Target | Before | Now |
| --- | --- | --- | --- |
| `os_application_usb_ready`, `os_application_usb_changed`, `os_application_host_observed` (`apps/os/application.c`) | main task, index 0 | Always. It ended any index 0 wait of the main task, including the application's deadline wait (status "notified" after microseconds) | Only while the main task sleeps in an OS wait that registered (`application_wait_until`: `main_entry`, `ss_usb_wait`, `ss_host_recv`); that wait removes what the OS gave |
| `os_host_offer` (mailbox) | the task in `ss_host_recv` | Always | Same gating |
| `os_large_finish` (`apps/os/large.c`) | a task registered in `ss_large_wait` | Without a lock; a notification after the wait returned stayed pending | Under the kernel lock the waiter also takes; the waiter removes what the loader gave |
| `read_blocking` / SD owner ready wait (`apps/os/sd_owner.c`) | the caller | Index 1, and the ready wait repeats until the owner has answered or its time bound has passed | Unchanged |

`ss_host_recv` also repeats its wait until a request arrives, the session ends or the timeout has passed, so a wake-up
for another reason does not end the call.

## Waits that run in an application task

| Wait | Index | Ended by | An OS wake-up ends it early? |
| --- | --- | --- | --- |
| `sunstone_deadline_wait_until` (`runtime/deadline.c`) | 0 | the timer interrupt, or a device hint the application registered for (status "notified") | No, since the OS gives index 0 only inside its own waits |
| `ss_large_wait` | 0 | the loader (`os_large_finish`), the timeout; the call repeats on any other wake | Not by an OS event; a stray one repeats the wait |
| `ss_usb_wait`, `ss_host_recv`, the pre-main wait in `main_entry` | 0 | USB or live-state events by design, the deadline | These are the OS waits that register |
| SD owner callers | 1 | the owner | No |
| NPU, DMA, GPU and Bluetooth waits (`drivers/npu`, `drivers/dma`, `drivers/gpu`, `apps/os/npu*.c`, `apps/os/headset.c`) | 0 in their own service tasks | their device interrupts and callers | They run in OS service tasks, not in an application task; the application reaches them through queues and flags |

## Check

`checks/qemu-os-boot.py`, case `os-notifications-do-not-end-waits`: a worker of the application gives the main task
the usb-ready and live-state notifications every millisecond while the main task sleeps in the deadline wait and in
`ss_host_recv`. The deadline wait must expire after its 30 ms and `ss_host_recv` must time out after 30 ms. With the OS
giving the notifications as before, the deadline wait ends after 5 microseconds and the check fails.
