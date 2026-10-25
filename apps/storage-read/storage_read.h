#ifndef SUNSTONE_STORAGE_READ_PRIVATE_H
#define SUNSTONE_STORAGE_READ_PRIVATE_H
#include "storage_read_session.h"
#include "storage_read_service.h"
#include "a733_mmc_platform_native.h"
#include "../board/application.h"
/* These two client tasks are fixed by image startup, never registered at runtime. */
#define STORAGE_READ_CLIENTS 2u
#define STORAGE_READ_ACTIVE 5u
struct storage_read_slot {
 TaskHandle_t client;
 struct storage_read_request request;
 struct sunxi_mmc_result completion;
 uint64_t ticket,last_request_id,deadline,epoch,generation;
 uint64_t started; /* Counter at the first poll of the active read. */
 uint32_t elapsed_us; /* First poll to completion; nonzero profiles only. */
 atomic_uint state;
 uint8_t copied[STORAGE_READ_OUTPUT],digest[32];
};
struct storage_read_application {
 struct storage_read_session session;
 struct a733_mmc_platform platform;
 struct a733_mmc_platform_native native;
 struct a733_mmc_platform_config config;
 StaticSemaphore_t mutex_storage;
 SemaphoreHandle_t mutex;
 StaticTask_t task;
 StackType_t stack[2048];
 TaskHandle_t handle;
 atomic_uint result,cancel;
 unsigned initialized,stage,reason,card_valid,active;
 uint64_t deadline;
 struct sunxi_mmc_card card;
 struct storage_read_slot slots[STORAGE_READ_CLIENTS];
 uint64_t next_ticket;
 /* Borrowed PIO memory is never reused after fault. USB only uses copied. */
 uint8_t borrowed[STORAGE_READ_OUTPUT];
};
struct storage_read_consumer {
 StaticTask_t task;
 StackType_t stack[2048];
 TaskHandle_t handle;
 atomic_uint result,completed;
 unsigned stage;
 uint64_t ticket;
 uint8_t response[STORAGE_READ_HEADER+STORAGE_READ_OUTPUT],digest[32];
};
extern struct storage_read_application storage_read;
extern struct storage_read_consumer storage_read_consumer;
void storage_read_client_task(void *);
void storage_read_task(void *);
int storage_read_output_ready(void);
void storage_read_revoke(unsigned);
#endif
