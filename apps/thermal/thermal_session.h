#ifndef SUNSTONE_THERMAL_SESSION_H
#define SUNSTONE_THERMAL_SESSION_H
#include "a733_board_application.h"
#define A733_THERMAL_CONTRACT_MAGIC UINT64_C(0x4153373354484331)
#define A733_THERMAL_SESSION_MAGIC UINT64_C(0x4153373354484131)
#define THERMAL_OUTPUT_BYTES 224u
struct thermal_session {
 struct a733_board_session board;
 uint64_t magic,version,bytes,owner,authority;
 uint8_t initial[32],electrical[32],clocks[32],supervision[32];
 uint32_t apb1,apb1_hz;
 uint8_t configure_adc_clock,alarm_mask,critical_mask,reserved0;
 uint16_t limits[5][3]; /* hot, hysteresis, critical hardware codes */
 uint8_t reserved[6];
};
_Static_assert(sizeof(struct thermal_session)==808,"thermal session geometry");
int thermal_session_valid(const struct thermal_session *);
#endif
