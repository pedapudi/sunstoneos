#include "a733_deinterlace_application.h"
#include "a733_rail_leases.h"
#include "boot-record.h"
#include "a733_device_memory_layout.h"
#define a733_smp_contract_valid board_contract_unused
#include "../board/admission.c"
#undef a733_smp_contract_valid
_Static_assert(A733_BOARD_SUPERVISED_SECONDS==A733_DEINTERLACE_SESSION_SECONDS,
    "DEINTERLACE and board providers must use the same session lifetime");
/* Host preparation reads these words from the actual linked image. */
__attribute__((used,retain)) const uint64_t deinterlace_geometry[11]={
    A733_DEINTERLACE_SESSION_MAGIC,1,sizeof(struct a733_deinterlace_session),
    sizeof(struct deinterlace_boot_contract),A733_BOARD_SESSION_ADDRESS,
    A733_DEINTERLACE_SESSION_SECONDS,A733_DEINTERLACE_INPUT_CAPACITY,A733_DEINTERLACE_OUTPUT_CAPACITY,
    A733_DEINTERLACE_RESULT_BYTES,A733_DEINTERLACE_MAX_WIDTH,A733_DEINTERLACE_MAX_HEIGHT};
int a733_deinterlace_session_valid(const struct a733_deinterlace_session *s)
{
    return s&&a733_board_session_valid(&s->board,0)&&s->board.workload==2&&
        s->magic==A733_DEINTERLACE_SESSION_MAGIC&&s->version==1&&s->bytes==sizeof(*s)&&
        s->owner&&s->owner!=s->board.pmic_owner&&s->owner!=s->board.usb0_owner&&
        s->authority&&s->pck_client<16&&
        s->module==UINT32_C(0x00000001)&&s->module_hz==300000000&&
        s->supply_uv==800000&&s->supply_rail==A733_RAIL_DCDC2&&
        s->iommu_gates[0]==0x10007&&s->iommu_gates[1]==0x10007&&
        present(s->initial,32)&&present(s->memory,32)&&present(s->electrical,32)&&
        present(s->supervision,32)&&present(s->fixture,32)&&
        !present(s->reference,32)&&a733_deinterlace_geometry_valid(s);
}
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
    const struct a733_deinterlace_session *s=(const void *)A733_BOARD_SESSION_ADDRESS;
    if(!c||!a733_deinterlace_session_valid(s)||c->magic!=A733_DEINTERLACE_CONTRACT_MAGIC)return 0;
    const struct deinterlace_boot_contract *record=deinterlace_boot_record(c);
    const struct a733_device_memory_layout layout={A733_DEVICE_MEMORY_LAYOUT,
        A733_DEVICE_WB_START,A733_DEVICE_WB_END,A733_DEVICE_NC_START,A733_DEVICE_NC_END};
    uint8_t hash[32];storage_digest_bytes(s,sizeof(*s),hash);
    if(!record||c->reserved[1]!=s->board.memory_authority||c->bytes!=sizeof(*record)||
       !a733_device_memory_handoff_valid(&layout,c->reserved)||
       memcmp(hash,record->session_digest,32)||
       memcmp(record->entry_digest,s->board.qualification,32))return 0;
    /* The existing validator receives an ordinary board-prefix view. Extended
     * memory authority remains in the retained original record. */
    struct a733_smp_contract base=*c;
    base.magic=A733_BOARD_CONTRACT_MAGIC;base.bytes=sizeof(base);
    base.reserved[0]=A733_BOARD_SESSION_ADDRESS;base.reserved[1]=sizeof(s->board);
    storage_digest_bytes(&s->board,sizeof(s->board),(void *)&base.reserved[2]);
    return a733_board_contract_valid(&base,&s->board,a733_smp_build_id,now);
}
