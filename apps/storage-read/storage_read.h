#ifndef SUNSTONE_STORAGE_READ_PRIVATE_H
#define SUNSTONE_STORAGE_READ_PRIVATE_H
#include "storage_read_session.h"
#include "a733_mmc_platform_native.h"
#include "../board/application.h"
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
 unsigned initialized,stage,reason,card_valid;
 uint64_t deadline;
 struct sunxi_mmc_card card;
 struct sunxi_mmc_result completion;
 /* Borrowed PIO memory is never reused after fault. USB only uses copied. */
 uint8_t borrowed[512],copied[512],digest[32],header[256];
};
extern struct storage_read_application storage_read;
void storage_read_task(void *);
int storage_read_output_ready(void);
void storage_read_revoke(unsigned);
int storage_read_copy(uint8_t *,unsigned *);
#endif
