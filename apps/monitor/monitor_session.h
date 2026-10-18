#ifndef SUNSTONE_MONITOR_SESSION_H
#define SUNSTONE_MONITOR_SESSION_H
#include "a733_board_application.h"
#define A733_MONITOR_SESSION_MAGIC UINT64_C(0x415337334d4f4131)
#define A733_MONITOR_CONTRACT_MAGIC UINT64_C(0x415337334d4f4331)
struct monitor_session {
 struct a733_board_session board;
 uint64_t magic,version,bytes,owner,authority;
 uint8_t initial[32],electrical[32],clocks[32],supervision[32];
 uint32_t apb1,apb1_hz;
 uint8_t configure_adc_clock,cldo3_selector,reserved[6];
 uint32_t cldo3_uv,source_impedance_ohms;
 uint64_t thermal_owner,clock_owner;
 uint16_t limits[5][3];
 uint8_t alarm_mask,critical_mask;
};
_Static_assert(sizeof(struct monitor_session)==832,"monitor session geometry");
int monitor_session_valid(const struct monitor_session *);
#endif
