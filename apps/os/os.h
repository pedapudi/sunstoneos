#ifndef SUNSTONE_OS_H
#define SUNSTONE_OS_H
/* Integrated OS image (docs/os-image.md): the mimus board composition plus a
 * continuous thermal service, one fixed interrupt table and one fixed start
 * list. */
#include "a733_board_application.h"
/* Immutable little-endian session: the board session (workload 0 or 4)
 * followed by the thermal owner's facts. The evidence hashes identify
 * externally reviewed records; they do not establish those facts. */
struct os_session {
    struct a733_board_session board;
    uint64_t magic,version,bytes;
    uint64_t thermal_owner,thermal_authority;
    uint8_t thermal_initial[32],thermal_electrical[32],thermal_clocks[32],thermal_supervision[32];
    uint32_t apb1,apb1_hz;
    uint8_t configure_adc_clock,alarm_mask,critical_mask,reserved0;
    uint16_t limits[5][3]; /* hot, hysteresis, critical hardware codes */
    uint8_t reserved[6];
};
_Static_assert(sizeof(struct os_session)==A733_OS_SESSION_BYTES,"OS session geometry");
#define OS_SESSION_VERSION 1u
int os_session_valid(const struct os_session *);
/* The admitted complete session after EL3 sealing; NULL before. */
const struct os_session *os_session(void);

/* Fixed start list entries; each runs at the board hook of the same name. */
struct a733_pin_handoff;
void os_thermal_task(void *);
int os_thermal_task_privileged(void (*)(void *),void *);
int os_thermal_pins(struct a733_pin_handoff *);
int os_thermal_start(void);
/* IRQ context, from the fixed interrupt table on CPU0. */
void os_thermal_irq(void);

/* Resident services after the board's own (apps/board/application.h). The
 * mimus board application reports as the first of them. */
enum { OS_SERVICE_MIMUS=5, OS_SERVICE_THERMAL, OS_SERVICES };
struct os_service { const char *name; uint32_t window_us; };
extern const struct os_service os_services[OS_SERVICES];
/* Interval between watchdog renewals while every service is in its window. */
#define OS_RENEW_US 1000000u
/* 1 when every service reported progress no more than its window before
 * now_us (counter microseconds); 0 when any record is older than its window;
 * -1 when no record is stale but some service has not yet reported. */
int os_services_healthy(uint64_t now_us);
/* Resident WDT0 renewal from the supervising task (watchdog.c); 1 renewed. */
int os_watchdog_reload(void *supervisor);
#endif
