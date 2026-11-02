/* SPDX-License-Identifier: GPL-2.0-only */
/* Host stand-in for the FreeRTOS calls of apps/dram-bandwidth/main.c (protocol check only). */
#ifndef SHIM_FREERTOS_H
#define SHIM_FREERTOS_H
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define configNUMBER_OF_CORES 8
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) ((unsigned)(ms))
#define configASSERT(x) do { if (!(x)) { fprintf(stderr, "assert %s\n", #x); exit(2); } } while (0)
typedef uint32_t StackType_t;
typedef unsigned TickType_t;
typedef struct StaticTask {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    unsigned notifications, core;
    void (*function)(void *);
    void *argument;
    pthread_t thread;
} StaticTask_t;
typedef StaticTask_t *TaskHandle_t;
unsigned shim_core(void);
#define portGET_CORE_ID() shim_core()
#endif
