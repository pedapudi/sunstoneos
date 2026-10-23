/* The xHCI driver's register access path, built into the device-memory
 * ledger fixture: rd() checks the controller's lease with
 * device_memory_describe() before and after each register read. */
#include "../drivers/usb/a733_xhci.c"

void fixture_xhci_setup(struct device_memory_pool *, struct device_memory_lease, struct device_memory_extent);
int fixture_xhci_rd(uint32_t, uint32_t *);

static struct a733_xhci controller;
static uint64_t ticks;
static int fixture_now(void *context, uint64_t *out) { (void)context; *out = ++ticks; return 1; }
static int fixture_live(void *context, uint64_t generation, uint64_t deadline)
{ (void)context; (void)generation; (void)deadline; return 1; }
static int fixture_read32(void *context, uint32_t offset, uint32_t *value)
{ (void)context; *value = offset; return 1; }

void fixture_xhci_setup(struct device_memory_pool *pool, struct device_memory_lease lease,
                        struct device_memory_extent extent)
{
    memset(&controller, 0, sizeof(controller));
    controller.ops.now = fixture_now;
    controller.ops.live = fixture_live;
    controller.ops.read32 = fixture_read32;
    controller.pool = pool;
    controller.memory = lease;
    controller.extent = extent;
    controller.deadline = controller.lifetime_deadline = UINT64_MAX;
}
int fixture_xhci_rd(uint32_t offset, uint32_t *value) { return rd(&controller, offset, value); }
