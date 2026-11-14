# Task notifications: who gives what to whom

An application's tasks own their notification index 0. The OS never ends an application's wait on it. This page
lists every wait that can run in an application task, the index plan, and the check that enforces the rule.
The indices are defined in [include/sunstone_notify.h](../include/sunstone_notify.h).

## Index plan (`configTASK_NOTIFICATION_ARRAY_ENTRIES` is 3)

| Index | Name | Users |
| --- | --- | --- |
| 0 | `SS_NOTIFY_OWN` | The owning task. For an application task, the application alone: no OS, SDK or runtime code gives or takes it. For an OS service task, that service |
| 1 | `SS_NOTIFY_REQUEST_WAIT` | A caller sleeping for a request that an OS service serves for it: the SD owner's callers (`apps/os/sd_owner.c`); legacy GPU diagnostic tasks also use this index |
| 2 | `SS_NOTIFY_DEADLINE` | The waits of an application task inside the runtime and the OS: the deadline wait (timer interrupt and device hints), `ss_large_wait` and the OS waits that register with `os_wake`. Native GPU fence and lifecycle waits also use it. A wait takes only this index on its way out |

The deadline wait obtains its index from `sunstone_deadline_notify_index()`. The default is index 0 (an OS service
task keeps its own); `apps/os/application.c` returns index 2 for a task that carries the application tag, which the
main task receives when the application starts and every task it creates inherits. The SD owner, an OS task that an
application call creates, drops the tag.
The trusted GPU peer is created by the OS without that address-space tag.
Its explicit task identity selects index 2 for startup and fence waits; the
startup-ready publication also gives index 2. OS service tasks keep index 0.

## What the OS gives to an application task

| Source | Target | Before | Now |
| --- | --- | --- | --- |
| `os_application_usb_ready`, `os_application_usb_changed`, `os_application_host_observed` (`apps/os/application.c`) | main task, index 2 | Always. It ended any index 0 wait of the main task, including the application's deadline wait (status "notified" after microseconds) | Only while the main task sleeps in an OS wait that registered (`application_wait_until`: `main_entry`, `ss_usb_wait`, `ss_host_recv`); that wait removes what the OS gave |
| `os_host_offer` (mailbox) | the task in `ss_host_recv` | Always | Same gating |
| `os_large_finish` (`apps/os/large.c`) | a task registered in `ss_large_wait` | Without a lock; a notification after the wait returned stayed pending | Under the kernel lock the waiter also takes; the waiter removes what the loader gave |
| `read_blocking` / SD owner ready wait (`apps/os/sd_owner.c`) | the caller | Index 1, and the ready wait repeats until the owner has answered or its time bound has passed | Unchanged |

`ss_host_recv` also repeats its wait until a request arrives, the session ends or the timeout has passed, so a wake-up
for another reason does not end the call.

## Waits that run in an application task

| Wait | Index | Ended by | An OS wake-up ends it early? |
| --- | --- | --- | --- |
| `sunstone_deadline_wait_until` (`runtime/deadline.c`) | 2 for an application task | the timer interrupt, or a device hint at index 2 (status "notified") | No: the OS gives index 2 only inside its own waits, and the application's index 0 does not end it |
| `ss_large_wait` | 2 | the loader (`os_large_finish`), the timeout; the call repeats on any other wake | Not by an OS event; a stray one repeats the wait |
| `ss_usb_wait`, `ss_host_recv`, the pre-main wait in `main_entry` | 2 | USB or live-state events by design, the deadline | These are the OS waits that register |
| SD owner callers | 1 | the owner | No |
| Native GPU fence waits | 2 | the completion interrupt, transport-lock release or precise deadline | The wait rechecks its fence after every hint |
| Native GPU lifecycle waits | 2 for an application task | a registered service publication or precise deadline | The wait rechecks its phase or queue predicate; it removes late service hints |
| NPU, DMA, GPU service and Bluetooth waits (`drivers/npu`, `drivers/dma`, `drivers/gpu`, `apps/os/npu*.c`, `apps/os/headset.c`) | 0 in their own service tasks | their device interrupts and callers | They run in OS service tasks, not in an application task; the application reaches them through queues and flags |

The [GPU owner wait design](designs/gpu-owner-events.md) defines native GPU registration,
sequence rechecks and late-hint removal. GPU lifecycle and direct fence waits run
sequentially in each caller; they never arm both uses of index 2 together.

## Checks

`checks/event-gpu.py` executes native GPU wait registration and verifies that a
service publication uses runtime index 2 and leaves no late service hint.
`checks/event-gpu-peer.py` executes the real selector, peer startup and fence
deadline wait for an untagged peer. It checks readiness before and during
wait registration. Its modeled delivery uses indices captured from the linked
GPU IRQ, transport-unlock and startup publishers, and rejects an independently
changed signaling index. All three producers and the peer selector derive their
native index from `SS_NOTIFY_DEADLINE`.


`checks/qemu-os-boot.py`:
- `os-notifications-do-not-end-waits`: a worker of the application gives the main task the usb-ready and live-state notifications every millisecond while it sleeps in the deadline wait and in `ss_host_recv`; each ends at its own condition.
- `application-give-survives-os-wait`: a task of the application gives the main task index 0 during `ss_host_recv`; the give is still pending when the call returns (it was cleared before the OS used its own index).
- `application-give-does-not-end-deadline-wait`: the same give during the deadline wait; the wait expires and the give is pending afterwards.
- `deadline-hint-leaves-index-0-alone`: a hint at index 2 ends the deadline wait early, and the application's next wait on index 0 times out.
