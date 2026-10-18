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
_Static_assert(offsetof(struct board_radio_profile,response_bytes)==sizeof(struct board_radio_profile)-520,
    "observer response extent");
_Static_assert(sizeof(struct board_radio_profile)==1856 &&
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
};
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
