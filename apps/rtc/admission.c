#include "a733_rtc_application.h"
#include "a733_rail_leases.h"
#define a733_smp_contract_valid board_contract_unused
#include "../board/admission.c"
#undef a733_smp_contract_valid
int a733_rtc_session_valid(const struct a733_rtc_session *s)
{
    return s&&a733_board_session_valid(&s->board,0)&&s->board.workload==2&&
        s->magic==A733_RTC_SESSION_MAGIC&&
        ((s->version==1&&s->bytes==sizeof(*s))||
         (s->version==2&&s->bytes==sizeof(struct a733_rtc_session_set)&&
          a733_rtc_setting_valid(&((const struct a733_rtc_session_set *)s)->setting)))&&
        s->owner&&s->owner!=s->board.pmic_owner&&s->owner!=s->board.usb0_owner&&
        s->authority&&s->alarm_seconds>=1&&s->alarm_seconds<=10&&!s->reserved&&
        present(s->initial,32)&&present(s->clock,32)&&present(s->electrical,32)&&
        present(s->supervision,32);
}
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
    if(!c||c->magic!=A733_RTC_CONTRACT_MAGIC)return 0;
    const uint64_t bytes=c->reserved[1];
    if(bytes!=sizeof(struct a733_rtc_session)&&bytes!=sizeof(struct a733_rtc_session_set))return 0;
    struct a733_rtc_session_set snapshot={0};
    const volatile uint8_t *source=(const void *)A733_BOARD_SESSION_ADDRESS;
    for(size_t i=0;i<(size_t)bytes;++i)((uint8_t *)&snapshot)[i]=source[i];
    const struct a733_rtc_session *s=&snapshot.base;
    if(s->bytes!=bytes||!a733_rtc_session_valid(s))return 0;
    uint8_t hash[32];storage_digest_bytes(&snapshot,(size_t)bytes,hash);
    if(memcmp(hash,&c->reserved[2],32))return 0;
    struct a733_smp_contract base=*c;
    base.magic=A733_BOARD_CONTRACT_MAGIC;base.reserved[1]=sizeof(s->board);
    storage_digest_bytes(&s->board,sizeof(s->board),(void *)&base.reserved[2]);
    return a733_board_contract_valid(&base,&s->board,a733_smp_build_id,now);
}
