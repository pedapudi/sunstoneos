#ifndef SUNSTONE_CRYPTO_PRIVATE_H
#define SUNSTONE_CRYPTO_PRIVATE_H
#include "crypto_session.h"
#include "run.h"
#include "../board/application.h"
struct crypto_application {
 struct crypto_session session;
 struct a733_ce_session engine;
 struct a733_ce_config config;
 struct a733_clock_token token;
 struct crypto_run run;
 StaticSemaphore_t mutex_storage;
 SemaphoreHandle_t mutex;
 StaticTask_t task;
 StackType_t stack[2048];
 TaskHandle_t handle;
 atomic_uint result;
 unsigned retaining;
 uint8_t payload[160],digest[32];
};
extern struct crypto_application crypto;
void crypto_task(void *);
int crypto_output_ready(void);
#endif
