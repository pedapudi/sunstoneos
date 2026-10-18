#include "pwm_session.h"
#include "a733_pwm.h"
#define a733_smp_contract_valid board_contract_unused
#include "../board/admission.c"
#undef a733_smp_contract_valid
int pwm_session_valid(const struct pwm_session *s)
{
 if(!s||!a733_board_session_valid(&s->board,0)||s->board.workload!=2||
 s->magic!=A733_PWM_SESSION_MAGIC||s->version!=1||s->bytes!=sizeof(*s)||
 !s->owner||s->owner==s->board.pmic_owner||s->owner==s->board.usb0_owner||!s->authority||
 !present(s->initial,32)||!present(s->memory,32)||!present(s->power,32)||!present(s->supervision,32)||
 s->reserved[0]||s->reserved[1]||s->operation_us<200000||s->operation_us>5000000)return 0;
 struct a733_pwm_config c={.owner=s->owner,.handoff=s->authority,.deadline_us=1,
 .apb0=s->apb0,.apb0_hz=s->apb0_hz,.period_ns=25000,.duty_ns=12500,.pulses=4000,.active_high=1,
 .expected=s->expected,.desired=s->desired};
 struct a733_pwm_plan plan;a733_clock_mask_t shared;return a733_pwm_plan(&c,&plan,&shared);
}

int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
    const struct pwm_session *s=(const void *)A733_BOARD_SESSION_ADDRESS;
    if(!c||!pwm_session_valid(s)||c->magic!=A733_PWM_CONTRACT_MAGIC||
       c->reserved[1]!=sizeof(*s))return 0;
    uint8_t hash[32];storage_digest_bytes(s,sizeof(*s),hash);
    if(memcmp(hash,&c->reserved[2],32))return 0;
    struct a733_smp_contract base=*c;
    base.magic=A733_BOARD_CONTRACT_MAGIC;base.reserved[1]=sizeof(s->board);
    storage_digest_bytes(&s->board,sizeof(s->board),(void *)&base.reserved[2]);
    return a733_board_contract_valid(&base,&s->board,a733_smp_build_id,now);
}
