#ifndef SUNSTONE_DSI_APPLICATION_H
#define SUNSTONE_DSI_APPLICATION_H
#include "../board/application.h"
#include "dsi-session.h"
#include "input.h"
#define DSI_OBSERVATION_BYTES 256u
struct dsi_application {
 struct dsi_input input;uint8_t replay_result[256];
 struct dsi_session session;TaskHandle_t handle;StaticTask_t task;StackType_t stack[8192];
 atomic_uint result,failed;unsigned admitted,stage,supply_index,panel_index,packet_pending;
 uint64_t deadline,panel_wait,observed_start,observed_end,next_observe,pck_token;
 struct a733_rail_token rail;struct a733_pmic_request supply_request;struct a733_pmic_snapshot supplies[8];
 struct a733_pin_lease_banked pins;struct a733_gpu_resource_lease pck;
 struct a733_media_clock media;struct a733_display_pll pll;struct a733_iommu iommu;
 struct a733_display_native native;struct a733_dsi command;
 struct device_memory_pool *pool;struct device_memory_lease frame,tables;
 struct device_memory_extent frame_extent,table_extent;
 uint8_t profile_hash[32],frame_hash[32],observation[256];
};
extern struct dsi_application dsi;
int dsi_live(void);
int dsi_input_now(void *,uint64_t *);
void dsi_task(void *);
#endif
