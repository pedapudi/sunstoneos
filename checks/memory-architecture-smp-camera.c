/* The camera driver's memory check, built into the device-memory ledger
 * fixture: authority() calls memory_owned(), which checks the capture lease
 * with device_memory_describe() on every call. */
#include "../drivers/camera/a733_camera_native.c"

void fixture_camera_setup(struct device_memory_pool *, struct device_memory_lease, struct device_memory_extent);
int fixture_camera_memory_owned(void);

static struct a733_camera_native camera;

void fixture_camera_setup(struct device_memory_pool *pool, struct device_memory_lease lease,
                          struct device_memory_extent extent)
{
    camera.pool = pool;
    camera.memory = lease;
    camera.extent = extent;
}
int fixture_camera_memory_owned(void) { return memory_owned(&camera); }
