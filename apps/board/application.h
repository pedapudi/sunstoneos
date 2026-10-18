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
struct board_application {
    struct a733_board_session session;
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
extern struct board_application board;
extern struct board_provider providers;
int board_live(void *);
void board_fail(void *);
void board_usb0_task(void *);
void board_host_task(void *);
#endif
