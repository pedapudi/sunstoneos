#ifndef SUNSTONE_G2D_PRIVATE_H
#define SUNSTONE_G2D_PRIVATE_H
#include "a733_g2d.h"
#include "input.h"
#include "a733_iommu.h"
#include "device_memory_pool.h"
#include "a733_power_domains.h"
#include "../board/application.h"
struct g2d_application {
    struct a733_g2d_session session;
    struct g2d_input supplied;
    struct a733_iommu iommu;
    struct a733_iommu_tables tables;
    struct a733_clock_token clock;
    struct a733_rail_token rail;
    struct a733_pmic_request request;
    struct a733_pmic_snapshot supply;
    struct a733_gpu_power_resources power_ready;
    uint64_t power_token;
    struct a733_power_domain_row row; /* PCK domain 1, registered at startup */
    struct a733_clock_token domain_clock;
    struct a733_rail_token domain_rail;
    struct a733_gpu_resource_lease domain_pck;
    struct device_memory_pool *pool,*wb_pool;
    struct device_memory_lease leases[4],wb_leases[2];
    StaticTask_t task;StackType_t stack[4096];TaskHandle_t handle;
    uint8_t *input,*output,*commands,*payload;
    uint32_t *table;
    size_t bytes,rounded,copied;
    uint32_t rcq_status,baseline,guards_ok,complete,reset_word;
    unsigned finish_ready;
    unsigned stage,copy_step,clock_step,domain_requested;
    uint64_t last_now;
    atomic_uint result;
    uint8_t digest[32],session_digest[32];
};
_Static_assert(sizeof(struct g2d_application)<=65536,"G2D metadata budget");
extern struct g2d_application g2d;
int g2d_upload_allocate(void);
int g2d_allocate(void);
int g2d_guards(void);
void g2d_task(void *);
void board_usb0_task(void *);
#endif
