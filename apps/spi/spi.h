#ifndef SUNSTONE_SPI_PRIVATE_H
#define SUNSTONE_SPI_PRIVATE_H
#include "spi_session.h"
#include "../board/application.h"
#include "a733_spi_platform_native.h"
struct spi_application {
 struct spi_session session;
 struct a733_spi_platform platform;
 struct a733_spi_platform_native native;
 struct a733_spi_platform_config config;
 struct a733_spi_platform_ops operations;
 struct a733_spi_transfer transfer;
 struct a733_rail_token rail;
 struct a733_pmic_lifetime lifetime;
 struct a733_pmic_request request;
 struct a733_pmic_snapshot supply;
 StaticTask_t task;StackType_t stack[2048];TaskHandle_t handle;
 atomic_uint result,failed;
 unsigned stage,rail_retained,clock_retained;
 uint64_t deadline;
 uint8_t tx[32],rx[32],payload[SPI_OUTPUT_BYTES],digest[32],session_digest[32];
};
extern struct spi_application spi;
int spi_construct(void);
int spi_step(void);
void spi_task(void *);
void spi_cancel(void);
int spi_output_ready(void);
int spi_live(void);
int spi_publish(void);
#endif
