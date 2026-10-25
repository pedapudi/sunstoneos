/* SPI 65 (INTID 97, level high) on core 1, where the NPU worker runs. The
 * EL3 boot programs this table entry (boot.patch) and the shared handler
 * dispatches through it; the source stays disabled until the service rearms
 * it. The mask, rearm and stop callbacks and the bind-time readbacks are the
 * single-core images' (apps/npu-interrupt/irq.c), compiled here for core 1;
 * that file also defines npu_board_irq_bind. */
#include "irq_table.h"
#include "npu-board.h"
#define NPU_IRQ_CORE NPU_BOARD_CORE
#define NPU_IRQ_PRIORITY 0xd8u
#define NPU_IRQ_HANDLER npu_board_irq
#define NPU_IRQ_EXTERNAL_BIND npu_board_irq_bind
void npu_board_irq(void);
#include "../npu-interrupt/irq.c"

const struct a733_irq_route a733_irq_routes[]={
    {97,NPU_IRQ_CORE,NPU_IRQ_PRIORITY,npu_board_irq},{71,0,0xd8u,npu_board_thermal_irq}};
const unsigned a733_irq_route_count=sizeof(a733_irq_routes)/sizeof(a733_irq_routes[0]);
_Static_assert(NPU_IRQ_PRIORITY>=A733_IRQ_TABLE_MIN_PRIORITY,"NPU priority must permit FreeRTOS FromISR calls");
