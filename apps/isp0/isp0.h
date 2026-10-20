#ifndef SUNSTONE_ISP0_PRIVATE_H
#define SUNSTONE_ISP0_PRIVATE_H
#include "a733_isp0.h"
#include "input.h"
#include "a733_iommu.h"
#include "a733_camera_dma.h"
#include "a733_media_clock.h"
#include "a733_media_clock_native.h"
#include "device_memory_pool.h"
#include "../board/application.h"
enum isp0_extent {ISP0_TABLE,ISP0_RAW,ISP0_Y,ISP0_UV,ISP0_LOAD,ISP0_SAVE,ISP0_SAVE_LOAD,ISP0_VIPP,ISP0_EXTENTS};
struct isp0_application {
 struct a733_isp0_session session;struct isp0_input supplied;
 struct a733_isp0 engine;struct a733_camera_dma dma;
 struct a733_iommu iommu;struct a733_iommu_tables tables;
 struct a733_media_clock media;
 struct a733_rail_token rail;struct a733_pmic_request request;struct a733_pmic_snapshot supply;
 struct a733_gpu_power_resources power_ready;uint64_t power_token;
 struct a733_gpu_resource_lease power;
 struct device_memory_pool *pool,*wb_pool;
 struct device_memory_lease leases[ISP0_EXTENTS],wb_leases[2];
 uint8_t *memory[ISP0_EXTENTS],*payload;size_t sizes[ISP0_EXTENTS];
 StaticTask_t task;StackType_t stack[4096];TaskHandle_t handle;
 uint64_t last_now;size_t bytes,copied;
 unsigned stage,complete,guards_ok;atomic_uint result;
 uint8_t digest[32],session_digest[32];
};
extern struct isp0_application isp0;
int isp0_upload_allocate(void);
int isp0_allocate(void);
int isp0_guards(void);
void isp0_task(void *);
void board_usb0_task(void *);
#endif
