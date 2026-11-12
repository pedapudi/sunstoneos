/* Services and hardware that the virtual OS board does not have, so the
 * image links without them.
 *
 * The session's workload (2, which has no USB host and no radio) selects the
 * tasks that exist: the board starts no Type-C, power, USB host, radio or
 * network task, so none of these functions runs. An entry that does run
 * panics with its name, which turns a workload mismatch into a named stop.
 * The GPU service is not linked: its images select it with a build option
 * that the virtual board does not set. The NPU and I2S capture services are
 * linked, as in every image, and their session grants are absent. */
#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "../../apps/board/application.h"
#include "a733_usb_platform_native.h"
#include "a733_memory_uncached.h"

#define ABSENT(name) platform_panic("absent service: " name, 0)
int a733_typec_begin(struct a733_typec *t, uint64_t deadline) { (void)t; (void)deadline; ABSENT("typec begin"); }
int a733_typec_poll(struct a733_typec *t) { (void)t; ABSENT("typec poll"); }
void a733_typec_revoke(struct a733_typec *t) { (void)t; }
int a733_typec_snapshot(struct a733_typec *t, struct a733_typec_token *k, uint64_t deadline)
{ (void)t; (void)k; (void)deadline; ABSENT("typec snapshot"); }
int a733_typec_native_init(struct a733_typec_native *n, const struct a733_typec_native_config *c)
{ (void)n; (void)c; ABSENT("typec init"); }
int a733_typec_native_power_poll(struct a733_typec_native *n) { (void)n; ABSENT("typec power poll"); }
int a733_usb2_native_init(struct a733_usb2_native *n, const struct a733_usb2_platform_config *c,
    struct a733_platform_locks *l, void *t, struct device_memory_pool *p,
    struct a733_clock_leases *k, struct a733_rail_leases *r, struct a733_pmic *m,
    struct a733_gpu_resource_manager *g, struct a733_typec *y,
    int (*admit)(void *, const struct a733_usb2_platform_config *), void *context)
{ (void)n; (void)c; (void)l; (void)t; (void)p; (void)k; (void)r; (void)m; (void)g; (void)y; (void)admit; (void)context;
  ABSENT("usb2 init"); }
enum a733_usb2_device_kind a733_usb2_device_kind(struct a733_usb2_native *n) { (void)n; ABSENT("usb2 device kind"); }
void board_host_task(void *unused) { (void)unused; ABSENT("usb host task"); }
struct device_memory_pool *a733_memory_uncached_init(void) { return 0; }
/* The device memory reservations back DMA0's pages; the virtual board has no DMA0, and no session grants I2S capture. */
struct device_memory_pool *a733_memory_reservations_init(void) { ABSENT("device memory reservations"); }
int a733_usb_platform_native_read(void *c, uintptr_t a, uint32_t *v) { (void)c; (void)a; (void)v; return 0; }
int a733_usb_platform_native_write(void *c, uintptr_t a, uint32_t v) { (void)c; (void)a; (void)v; return 0; }

/* No radio: workload 2 starts neither the radio owner nor the network client. */
const struct board_radio_chip_id *board_radio_chip_id(void) { return 0; }
int board_radio_startup(struct board_radio *r, struct a733_native_usb *u, struct board_radio_ce_provider *c,
    const void *p, size_t size, const struct board_radio_profile *f, const uint8_t d[32],
    const struct board_radio_chip_id *i, TaskHandle_t owner, TaskHandle_t client, atomic_uint *w)
{ (void)r; (void)u; (void)c; (void)p; (void)size; (void)f; (void)d; (void)i; (void)owner; (void)client; (void)w;
  ABSENT("radio startup"); }
enum board_radio_result board_radio_poll(struct board_radio *r) { (void)r; ABSENT("radio poll"); }
enum board_radio_result board_radio_external_failure(struct board_radio *r) { (void)r; ABSENT("radio failure"); }
int board_radio_client_poll(struct board_radio *r) { (void)r; ABSENT("radio client"); }
int board_radio_client_result(const struct board_radio *r, unsigned *bytes) { (void)r; (void)bytes; ABSENT("radio result"); }
int board_radio_watchdog(struct board_radio *r, uint64_t *progress, uint64_t *deadline)
{ (void)r; (void)progress; (void)deadline; ABSENT("radio watchdog"); }
int board_radio_profile_runtime(struct board_radio_profile *o, const struct board_radio_profile *i,
    const struct a733_board_session *s, uint8_t d[32])
{ (void)o; (void)i; (void)s; (void)d; ABSENT("radio profile"); }

/* The returning lifetime's units (apps/os/cpu-return*.c) are not linked: the virtual board's scenarios run the supervised
 * and resident lifetimes, whose paths test os_returning() or os_cpu_return_requested() before any other entry. */
#include "../../apps/os/cpu-return.h"
uint32_t os_cpu_return_entry_refusal;
int os_cpu_return_requested(void) { return 0; }
int os_cpu_return_failing(void) { return 0; }
int os_cpu_return_pmic_fault(uint32_t site,uint32_t value,const void *caller)
{ (void)site;(void)value;(void)caller;return 0; }
uint64_t os_cpu_return_cutoff(void) { return UINT64_MAX; }
int os_cpu_return_start(void) { ABSENT("cpu return start"); }
void os_cpu_return_begin(const uint8_t *data, size_t bytes) { (void)data; (void)bytes; ABSENT("cpu return begin"); }
void os_cpu_return_thermal_done(void) { ABSENT("cpu return thermal"); }
int os_cpu_return_devices_ready(void) { ABSENT("cpu return devices"); }
int os_cpu_return_entry_guard(const struct a733_cpu_clock *c, const struct a733_thermal_policy *p, uint64_t now,
    int (*read)(void *, uintptr_t, uint32_t *), void *context)
{ (void)c; (void)p; (void)now; (void)read; (void)context; ABSENT("cpu return entry guard"); }

/* The virtual board does not grant the physical radio. */
struct board_radio;
int board_radio_shutdown(struct board_radio *radio,uint64_t deadline)
{ (void)radio;(void)deadline;return 0; }
