#include "a733_isp0_application.h"
#include "a733_rail_leases.h"
#define a733_smp_contract_valid board_contract_unused
#include "../board/admission.c"
#undef a733_smp_contract_valid
_Static_assert(A733_BOARD_SUPERVISED_SECONDS==A733_ISP0_SESSION_SECONDS,
    "ISP0 and board providers must use the same session lifetime");
__attribute__((used,retain)) const uint64_t isp0_geometry[11]={
    A733_ISP0_SESSION_MAGIC,1,sizeof(struct a733_isp0_session),
    sizeof(struct a733_smp_contract),A733_BOARD_SESSION_ADDRESS,
    A733_ISP0_SESSION_SECONDS,A733_ISP0_INPUT_CAPACITY,A733_ISP0_OUTPUT_CAPACITY,
    A733_ISP0_RESULT_BYTES,A733_ISP0_MAX_WIDTH,A733_ISP0_MAX_HEIGHT};
int a733_isp0_session_valid(const struct a733_isp0_session *s)
{
    return s&&a733_board_session_valid(&s->board,0)&&s->board.workload==2&&
        s->magic==A733_ISP0_SESSION_MAGIC&&s->version==1&&s->bytes==sizeof(*s)&&
        s->owner&&s->owner!=s->board.pmic_owner&&s->owner!=s->board.usb0_owner&&
        s->authority&&s->pck_client<16&&s->supply_uv==800000&&s->supply_rail==A733_RAIL_DCDC2&&
        s->iommu_gates[0]==0x10007&&s->iommu_gates[1]==0x10007&&
        present(s->initial,32)&&present(s->memory,32)&&present(s->electrical,32)&&
        present(s->supervision,32)&&present(s->fixture,32)&&!present(s->reference,32)&&a733_isp0_geometry_valid(s);
}
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
    const struct a733_isp0_session *s=(const void *)A733_BOARD_SESSION_ADDRESS;
    if(!c||!a733_isp0_session_valid(s)||c->magic!=A733_ISP0_CONTRACT_MAGIC||
       c->reserved[1]!=sizeof(*s))return 0;
    uint8_t hash[32];storage_digest_bytes(s,sizeof(*s),hash);
    if(memcmp(hash,&c->reserved[2],32))return 0;
    struct a733_smp_contract base=*c;
    base.magic=A733_BOARD_CONTRACT_MAGIC;base.reserved[1]=sizeof(s->board);
    storage_digest_bytes(&s->board,sizeof(s->board),(void *)&base.reserved[2]);
    return a733_board_contract_valid(&base,&s->board,a733_smp_build_id,now);
}
