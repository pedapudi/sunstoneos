#ifndef SUNSTONE_PWM_SESSION_H
#define SUNSTONE_PWM_SESSION_H
#include "a733_board_application.h"
#include "a733_pins.h"
#define A733_PWM_CONTRACT_MAGIC UINT64_C(0x4153373350574331)
#define A733_PWM_SESSION_MAGIC UINT64_C(0x4153373350574131)
#define PWM_RECORD_BYTES 512u
struct pwm_session {
 struct a733_board_session board;
 uint64_t magic,version,bytes,owner,authority;
 uint8_t initial[32],memory[32],power[32],supervision[32];
 struct a733_pin_config expected,desired;
 uint8_t reserved[2];
 uint32_t apb0,apb0_hz;
 uint64_t operation_us;
 uint64_t led_enabled,latch_identity,latch_owner;
 uint32_t latch_known_mask,latch_value;
 struct a733_pin_config led_expected;
 uint8_t led_reserved[5];
 uint32_t apbs0,apbs0_hz;
 uint8_t led_authority[32];
 uint32_t max_commands,reserved_tail;

};
_Static_assert(sizeof(struct pwm_session)==872,"PWM full session wire layout");
_Static_assert(offsetof(struct pwm_session,apb0)==768,"PWM APB wire offset");
struct pwm_request {
 uint32_t magic,version;
 uint64_t epoch,generation,sequence;
 uint32_t kind,argument,budget_us,reserved;
 uint8_t session_digest[32];
};
_Static_assert(sizeof(struct pwm_request)==80,"PWM request wire layout");
int pwm_session_valid(const struct pwm_session *);
#endif
