#ifndef SUNSTONE_DISPLAYPORT_PRIVATE_APPLICATION_H
#define SUNSTONE_DISPLAYPORT_PRIVATE_APPLICATION_H
#include "../board/application.h"
#include "snapshot.h"
#include "a733_dp.h"
#include "a733_display_engine.h"
#include "a733_iommu.h"
#include "a733_media_clock.h"
#include "a733_typec_pd.h"
#include "device_memory_pool.h"
struct dp_memory {
 struct dp_memory *self;
 struct device_memory_pool *pool;
 struct device_memory_lease lease;
 struct device_memory_extent extent;
 struct a733_clock_token exclusion;
 struct a733_media_clock media;
 struct a733_media_clock_config media_config;
 struct a733_iommu iommu;
 struct a733_iommu_tables tables;
 struct a733_de de;
 struct a733_de_config de_config;
 uint64_t last_now,until;
 unsigned stage,index,held,failed,domain_on,boot_ready;
 uint8_t pixel_digest[32];
};
struct dp_native {
 struct a733_clock_token clock;
 struct a733_rail_token rail;
 struct a733_pmic_lifetime pmic_lifetime;
 struct a733_pmic_request requests[2];
 struct a733_pmic_snapshot supplies[2];
 struct a733_gpu_resource_lease pck_lease;
 struct a733_gpu_power_resources pck_ready;
 struct a733_dp controller;
 struct a733_typec_dp_token token;
 uint64_t last_now,until;
 unsigned stage,index,held,failed,domain_on,clock_ready,tv_prepared,tv_started;
};
struct dp_application {
 struct a733_displayport_session session;
 struct a733_typec_native typec;
 struct dp_native native;
 struct dp_memory memory;
 StaticTask_t tasks[3];StackType_t stacks[3][4096];TaskHandle_t handles[3];
 struct a733_displayport_reply reply;
 uint64_t deadline;
 atomic_uint terminal,pd_enabled,typec_off;
 unsigned fresh_frame,guards,padding_valid;
 unsigned initialized;
};
extern struct dp_application dp_app;
int dp_current(void);
int dp_live(void);
int dp_now(void *,uint64_t *);
void dp_retain(void);
int dp_native_reserve(void);
int dp_native_prepare(void);
int dp_native_start(void);
int dp_native_stop(void);
int dp_native_source(void *,unsigned);
int dp_memory_boot(void);
int dp_memory_prepare(void);
int dp_memory_stop(void);
int dp_memory_service(void);
int dp_memory_source(void *,unsigned);
void dp_memory_retain(void);
int dp_memory_frame(unsigned);
int dp_memory_guards(void);
void dp_publish(unsigned);
#endif
