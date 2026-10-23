#ifndef SUNSTONE_BOARD_APPLICATION_PRIVATE_H
#define SUNSTONE_BOARD_APPLICATION_PRIVATE_H
#include "a733_board_application.h"
#include "a733_native_usb.h"
#include "a733_typec_native.h"
#include "a733_usb2_native.h"
#include "a733_maintenance_watchdog_internal.h"
#include "task.h"
#include "radio.h"
_Static_assert(offsetof(struct a733_board_session,radio_profile_digest)==368,"observer session digest");
_Static_assert(offsetof(struct a733_board_session,workload)==400,"observer workload");
_Static_assert(offsetof(struct a733_board_session,source_ma)==552,"SS source admission");
_Static_assert(offsetof(struct board_radio_profile,response_bytes)==BOARD_RADIO_PROFILE_RESPONSE_BYTES_OFFSET,
    "observer response extent");
_Static_assert(offsetof(struct board_radio_profile,response)==1596 &&
    offsetof(struct board_radio_profile,network_dhcp)==BOARD_RADIO_PROFILE_NETWORK_OFFSET &&
    offsetof(struct board_radio_profile,network_dns)==1857 &&
    offsetof(struct board_radio_profile,network_reserved)==1861 &&
    offsetof(struct board_radio_profile,address_timeout_ticks)==1864 &&
    offsetof(struct board_radio_profile,lease_cap_ticks)==1868 && sizeof(TickType_t)==8,"network wire suffix");
_Static_assert(sizeof(struct board_radio_profile)==BOARD_RADIO_PROFILE_BYTES &&
    offsetof(struct board_radio_profile,firmware_address)==432 &&
    offsetof(struct board_radio_profile,firmware_bytes)==464,"offline profile wire geometry");
_Static_assert(offsetof(struct board_radio_profile,platform)==184 &&
    offsetof(struct a733_radio_platform_config,handoff)==8 &&
    offsetof(struct a733_radio_platform_config,deadline_us)==16 &&
    offsetof(struct a733_radio_platform_config,lifetime_deadline_us)==24 &&
    offsetof(struct board_radio_profile,ce)==336 && offsetof(struct a733_ce_config,handoff)==8 &&
    offsetof(struct a733_ce_config,deadline_us)==24 &&
    offsetof(struct board_radio_profile,bootstrap_deadline_us)==1232 &&
    offsetof(struct board_radio_profile,startup_deadline_us)==1288 &&
    offsetof(struct board_radio_profile,client_deadline_us)==1320,"offline relative-profile fields");
struct board_application {
    struct a733_board_session session;
#ifdef SUNSTONE_BOARD_SUPERVISED
    struct a733_board_session admitted_session;
#endif
    struct a733_typec_native typec;
    struct a733_usb2_native host;
    struct a733_usb2_platform_config host_config;
    struct a733_watchdog_window window;
    StaticTask_t tasks[8];
    StackType_t stacks[8][2048];
    TaskHandle_t handles[8];
    atomic_uint fatal,result,watchdog_state,radio_result,radio_bytes;
    atomic_uint_fast64_t live_until;
    uint8_t sector[512];
    uint8_t dual_result[1088];
    uint8_t ss_result[544];
};
_Static_assert(64+512==576 && 576+512==sizeof(((struct board_application *)0)->dual_result),
    "combined header and disjoint copied output extents");
struct board_provider {
    struct a733_native_usb shared;
    struct board_radio_ce_provider ce;
};
#ifdef SUNSTONE_BOARD_SUPERVISED
/* Outputs are private, mutually disjoint and separate from immutable inputs. */
int board_radio_profile_runtime(struct board_radio_profile *,const struct board_radio_profile *,
    const struct a733_board_session *,uint8_t[32]);
#endif
extern struct board_application board;
extern struct board_provider providers;
int board_live(void *);
void board_fail(void *);
/* Resident lifetime. Each listed service calls board_service_progress after
 * every bounded iteration of its loop; recording is one atomic store, safe at
 * EL0. The supervising task calls board_resident_renew every 20 ms with the
 * loader's first watchdog expiry in counter ticks; it renews the armed
 * watchdog only while every listed service has made progress within its
 * window. Before every service has reported once it returns 1 without
 * renewing until that expiry, and 0 afterwards. It returns 0 once a reported
 * service is outside its window or a renewal fails. The image supplies both
 * functions and numbers its own services after BOARD_SERVICES. */
enum { BOARD_SERVICE_PMIC, BOARD_SERVICE_USB0, BOARD_SERVICE_TYPEC, BOARD_SERVICE_POWER,
       BOARD_SERVICE_USB2, BOARD_SERVICES };
#ifdef SUNSTONE_BOARD_RESIDENT
void board_service_progress(unsigned);
int board_resident_renew(void *supervisor,uint64_t first_expiry);
/* After a refusal: one plus the index of the first service that was stale,
 * or still silent at the first expiry; 0 for a pending fault or a failed
 * renewal. */
uint64_t board_resident_stop_detail(void);
#define BOARD_PROGRESS(service) board_service_progress(service)
#else
#define BOARD_PROGRESS(service) ((void)0)
#endif
void board_usb0_task(void *);
void board_host_task(void *);
#ifdef SUNSTONE_BOARD_APPLICATION
#ifndef SUNSTONE_BOARD_SUPERVISED
#error Board application hooks require supervised startup
#endif
/* Fixed image-owned callbacks, not dynamically registered extensions.
 * Match exact task entry/context pairs. Validate the complete immutable image
 * session before extending pin authority or constructing application tasks.
 * pins runs before provider construction; start runs after shared task binding.
 * Both run before the scheduler and return exactly 1 on success. */
int board_application_task_privileged(void (*)(void *),void *);
int board_application_pins(struct a733_pin_handoff *);
int board_application_start(void);
#endif
#endif
