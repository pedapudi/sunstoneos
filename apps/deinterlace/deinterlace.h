#ifndef SUNSTONE_DEINTERLACE_PRIVATE_H
#define SUNSTONE_DEINTERLACE_PRIVATE_H
#include "a733_deinterlace.h"
#include "input.h"
#include "a733_iommu.h"
#include "device_memory_pool.h"
#include "../board/application.h"
struct deinterlace_application {
    struct a733_deinterlace_session session;
    struct deinterlace_input supplied;
    struct a733_iommu iommu;
    struct a733_iommu_tables tables;
    struct a733_clock_token clock;
    struct a733_rail_token rail;
    struct a733_pmic_request request;
    struct a733_pmic_snapshot supply;
    struct a733_gpu_power_resources power_ready;
    uint64_t power_token;
    struct device_memory_pool *pool,*wb_pool;
    struct device_memory_lease leases[6],wb_leases[2];
    StaticTask_t task;StackType_t stack[4096];TaskHandle_t handle;
    uint8_t *input,*output,*payload,*current,*flag_input,*flag_output;
    struct a733_deinterlace_plan plan;
    uint32_t *table;
    size_t bytes,rounded,copied;
    uint32_t status,guards_ok,complete;
    unsigned stage,command_step,clock_step,armed;
    uint64_t last_now;
    atomic_uint result;
    uint8_t digest[32],session_digest[32];
};
_Static_assert(sizeof(struct deinterlace_application)<=131072,"DEINTERLACE metadata budget");
extern struct deinterlace_application deinterlace;
int deinterlace_upload_allocate(void);
int deinterlace_allocate(void);
int deinterlace_guards(void);
void deinterlace_task(void *);
void board_usb0_task(void *);
#endif
