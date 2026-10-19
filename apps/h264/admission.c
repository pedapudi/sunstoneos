#include "a733_h264_application.h"
#include "a733_rail_leases.h"
#define a733_smp_contract_valid board_contract_unused
#include "../board/admission.c"
#undef a733_smp_contract_valid
int a733_h264_session_valid(const struct a733_h264_session *s)
{
    return s&&a733_board_session_valid(&s->board,0)&&s->board.workload==2&&
        s->magic==A733_H264_SESSION_MAGIC&&s->version==1&&s->bytes==sizeof(*s)&&
        s->owner&&s->owner!=s->board.pmic_owner&&s->owner!=s->board.usb0_owner&&
        s->authority&&s->pck_client<16&&s->module_hz&&s->module_hz<=720000000&&
        !(s->module&~UINT32_C(0x8700001f))&&((s->module>>24)&7)<=1&&
        !(s->module&UINT32_C(0x80000000))&&s->supply_uv==800000&&s->supply_rail==A733_RAIL_DCDC2&&
        s->iommu_gates[0]==0x10007&&s->iommu_gates[1]==0x10007&&
        present(s->initial,32)&&present(s->memory,32)&&present(s->electrical,32)&&
        present(s->supervision,32)&&present(s->fixture,32);
}
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
    const struct a733_h264_session *s=(const void *)A733_BOARD_SESSION_ADDRESS;
    if(!c||!a733_h264_session_valid(s)||c->magic!=A733_H264_CONTRACT_MAGIC||
       c->reserved[1]!=sizeof(*s))return 0;
    uint8_t hash[32];storage_digest_bytes(s,sizeof(*s),hash);
    if(memcmp(hash,&c->reserved[2],32))return 0;
    struct a733_smp_contract base=*c;
    base.magic=A733_BOARD_CONTRACT_MAGIC;base.reserved[1]=sizeof(s->board);
    storage_digest_bytes(&s->board,sizeof(s->board),(void *)&base.reserved[2]);
    return a733_board_contract_valid(&base,&s->board,a733_smp_build_id,now);
}
