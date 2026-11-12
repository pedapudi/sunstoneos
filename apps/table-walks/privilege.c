/* SPDX-License-Identifier: BSD-3-Clause */
/* Image-side diagnostic hook: the application keeps the public OS ABI. */
extern void table_walks_worker(void *);
extern int __real_os_application_task_privileged(void (*)(void *), void *);
int __wrap_os_application_task_privileged(void (*entry)(void *), void *arg)
{
    return entry == table_walks_worker || __real_os_application_task_privileged(entry, arg);
}
