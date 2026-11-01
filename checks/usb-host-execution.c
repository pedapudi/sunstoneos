/* SPDX-License-Identifier: Apache-2.0 */
/* Native execution gate, with only task identity and EL1 admission modeled. */
#include "a733_usb2_native.h"
#include <assert.h>
#include <stdio.h>
typedef void *TaskHandle_t;
#define configASSERT assert
static void *task;
static unsigned privileged=1,nested,notifications;
static void *xTaskGetCurrentTaskHandle(void){return task;}
static int a733_task_context_owner_el1(void *owner)
{return privileged&&!nested&&task&&owner==task;}
static void xTaskNotifyGive(void *owner){assert(owner==(void *)1);++notifications;}
#include "execution.inc"
int main(void)
{
    struct a733_usb2_native n={.self=&n,.initialized=1,.owner_task=(void *)1};
    task=(void *)1;assert(current(&n)&&a733_usb2_native_share(&n));
    assert(!current(&n)&&!a733_usb2_native_share(&n));
    assert(a733_usb2_native_enter(&n)==1&&current(&n));
    assert(a733_usb2_native_enter(&n)==0); /* No recursive acquisition. */
    assert(a733_usb2_native_client(&n,(void *)2));
    assert(!a733_usb2_native_client(&n,(void *)3));
    task=(void *)2;assert(!current(&n)&&a733_usb2_native_enter(&n)==0);
    task=(void *)3;assert(a733_usb2_native_enter(&n)==-1);
    task=(void *)1;a733_usb2_native_leave(&n);assert(!notifications&&!current(&n));
    task=(void *)2;privileged=0;assert(a733_usb2_native_enter(&n)==-1);
    privileged=1;nested=1;assert(a733_usb2_native_enter(&n)==-1);nested=0;
    assert(a733_usb2_native_enter(&n)==1&&current(&n));
    task=(void *)1;assert(!current(&n)&&a733_usb2_native_enter(&n)==0);
    task=(void *)2;a733_usb2_native_leave(&n);assert(notifications==1);
    task=NULL;assert(!current(&n)&&a733_usb2_native_enter(&n)==-1);
    puts("PASS native owner/client identity, contention, context denial, release notification");
}
