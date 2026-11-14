/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SS_FREERTOS_OWNER_ADAPTER_H
#define SS_FREERTOS_OWNER_ADAPTER_H
/* Included after FreeRTOS.h. The public critical sections (vTaskEnterCritical and its FromISR form) take the two
 * global locks of the port; the queue, event group, stream buffer and timer sources use the per-object entries. */
void ssKernelObjectExit(void);
void ssKernelObjectSuspend(void);
BaseType_t ssKernelObjectResume(void);
UBaseType_t ssKernelObjectEnterFromISR(void);
void ssKernelObjectExitFromISR(UBaseType_t);
void ssKernelObjectEnterFor(const void *object, uintptr_t site);
/* Object sections: the lock is chosen by the object (see kernel/scheduler/freertos.c). */
void ssKernelObjectEnterOn(const void *object);
void ssKernelObjectLeave(void);
UBaseType_t ssKernelObjectEnterFromISROn(const void *object);
void ssKernelObjectLeaveFromISR(UBaseType_t);
UBaseType_t ssKernelObjectEnterWaitSlot(void);
void ssKernelObjectSuspendOn(const void *object);
BaseType_t ssKernelObjectResumeOn(void);
BaseType_t ssKernelOwnerIRQExit(void);
void ssKernelOwnerYield(void);
void ssKernelOwnerYieldTail(void);
#ifdef SS_FREERTOS_OBJECT_TRANSLATION
#undef taskENTER_CRITICAL
#undef taskEXIT_CRITICAL
#undef taskENTER_CRITICAL_FROM_ISR
#undef taskEXIT_CRITICAL_FROM_ISR
/* A section opened by name, where the source cannot use the translated macro. */
#define ssKernelObjectSectionEnter(object) ssKernelObjectEnterOn((const void *)(object))
/* Object sources lock the object they work on and nothing else, so operations on different objects do not wait
 * for each other. The public critical-section API takes the global locks and does not exclude them. */
#define taskENTER_CRITICAL() ssKernelObjectEnterOn((const void *)(SS_FREERTOS_OBJECT_EXPR))
#define taskEXIT_CRITICAL() ssKernelObjectLeave()
#define taskENTER_CRITICAL_FROM_ISR() ssKernelObjectEnterFromISROn((const void *)(SS_FREERTOS_OBJECT_EXPR))
#define taskEXIT_CRITICAL_FROM_ISR(mask) ssKernelObjectLeaveFromISR(mask)
/* The timer service suspends the scheduler across timer callbacks, which may call any object's API, so its suspension
 * is not tied to one object: it takes the global task lock. */
#define ssTimerServiceSuspend() ssKernelObjectSuspend()
#define ssTimerServiceResume() ssKernelObjectResume()
/* The timer source's one-time initialisation creates the timer queue, and the queue's own section nests inside it, so
 * it takes the public critical section (the global locks), which orders before every object lock. */
#define ssTimerInitEnter() vTaskEnterCritical()
#define ssTimerInitExit() vTaskExitCritical()
#define vTaskSuspendAll() ssKernelObjectSuspendOn((const void *)(SS_FREERTOS_OBJECT_EXPR))
#define xTaskResumeAll() ssKernelObjectResumeOn()
/* A walk of an object's waiter list. The walker holds the task-side lock throughout, so no waiter joins the list and no
 * other walk runs. The owner of a waiter times it out, aborts it or detaches it under the slot's interrupt-side lock
 * alone, so the walker takes that lock for one waiter at a time: it reads the waiter after its cursor, moves the cursor
 * past it and decides on it, then releases the lock. The cursor is an item linked into the list and counted in no
 * length, so an owner that unlinks a neighbour while the lock is free relinks the cursor, and the next waiter is read
 * after the lock is taken again. Interrupts on the walker's core and owners on other cores wait for one waiter, not
 * for the list. */
static inline void ssKernelObjectWalkLink(ListItem_t *cursor, ListItem_t *after)
{
    cursor->pxPrevious = after;
    cursor->pxNext = after->pxNext;
    after->pxNext->pxPrevious = cursor;
    after->pxNext = cursor;
}
static inline void ssKernelObjectWalkUnlink(ListItem_t *cursor)
{
    cursor->pxNext->pxPrevious = cursor->pxPrevious;
    cursor->pxPrevious->pxNext = cursor->pxNext;
}
/* Takes the interrupt-side lock and parks the cursor before the first waiter. */
#define ssKernelObjectWalkBegin(cursor, list) \
    ssKernelObjectWalkBeginOn((const void *)(SS_FREERTOS_OBJECT_EXPR), (cursor), (list))
static inline UBaseType_t ssKernelObjectWalkBeginOn(const void *object, ListItem_t *cursor, List_t const *list)
{
    UBaseType_t mask = ssKernelObjectEnterFromISROn(object);
    vListInitialiseItem(cursor);
    ssKernelObjectWalkLink(cursor, (ListItem_t *)listGET_END_MARKER(list));
    return mask;
}
/* Under the interrupt-side lock: the waiter after the cursor, with the cursor moved past it, or the end marker. */
static inline ListItem_t *ssKernelObjectWalkNext(ListItem_t *cursor, ListItem_t const *end)
{
    ListItem_t *item = cursor->pxNext;
    if (item == end) return item;
    ssKernelObjectWalkUnlink(cursor);
    ssKernelObjectWalkLink(cursor, item);
    return item;
}
/* Unlinks the cursor and releases the interrupt-side lock. */
static inline void ssKernelObjectWalkEnd(ListItem_t *cursor, UBaseType_t mask)
{
    ssKernelObjectWalkUnlink(cursor);
    ssKernelObjectLeaveFromISR(mask);
}
/* A walk that removes the first waiter until none is left takes the lock once per waiter; no cursor is needed. */
#define ssKernelObjectWalkEnter() ssKernelObjectEnterFromISROn((const void *)(SS_FREERTOS_OBJECT_EXPR))
#define ssKernelObjectWalkLeave(mask) ssKernelObjectLeaveFromISR(mask)
/* Between two waiters: releases the interrupt-side lock and takes it again. Host models delay here, with the lock free
 * and the cursor parked (perturb.h). */
#include "perturb.h"
#define ssKernelObjectWalkStep(mask) \
    (ssKernelObjectLeaveFromISR(mask), SS_KERNEL_PERTURB(SS_PERTURB_OBJECT_WALK), \
     ssKernelObjectEnterFromISROn((const void *)(SS_FREERTOS_OBJECT_EXPR)))
/* A member queue would wake a queue set's waiters under the member's slot, while the set's waiters are timed out under
 * the set's slot. */
#if ( configUSE_QUEUE_SETS == 1 )
#error "queue sets need the set's slot lock held where a member queue wakes the set's waiters"
#endif
#endif
#endif
