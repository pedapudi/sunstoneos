#include "npu-board-session.h"
#include "boot-record.h"
#include "a733_npu_platform.h"
#include "a733_device_memory_layout.h"
#define a733_smp_contract_valid board_contract_unused
#include "../board/admission.c"
#undef a733_smp_contract_valid
_Static_assert(A733_BOARD_SUPERVISED_SECONDS==NPU_BOARD_SESSION_SECONDS,
    "NPU and board providers must use the same session lifetime");
/* Host preparation reads these words from the actual linked image. */
__attribute__((used,retain)) const uint64_t npu_board_geometry[6]={
    NPU_BOARD_SESSION_MAGIC,NPU_BOARD_SESSION_VERSION,sizeof(struct npu_board_session),
    sizeof(struct npu_board_boot_contract),A733_BOARD_SESSION_ADDRESS,NPU_BOARD_SESSION_SECONDS};
int npu_board_session_valid(const struct npu_board_session *s)
{
    a733_clock_mask_t shared;
    if(!s||!a733_board_session_valid(&s->board,0)||s->board.workload!=NPU_BOARD_WORKLOAD||
       s->magic!=NPU_BOARD_SESSION_MAGIC||s->version!=NPU_BOARD_SESSION_VERSION||s->bytes!=sizeof(*s)||
       !s->owner||!s->command_admission||!s->supervisor||s->reserved||
       s->owner==s->board.pmic_owner||s->owner==s->board.usb0_owner||
       s->owner==s->board.typec_owner||s->owner==s->board.usb2_owner||
       s->pck_client>=16||s->pck_client==s->board.pck_client||
       memcmp(s->catalogue,npu_board_catalogue_digest,32)||
       !present(s->admission,32)||!present(s->clock,32)||!present(s->power,32)||!present(s->supervision,32))return 0;
    const struct a733_npu_platform_config fabric={{s->board.fabric[0],s->board.fabric[1],s->board.fabric[2]},
        {s->board.fabric_hz[0],s->board.fabric_hz[1],s->board.fabric_hz[2]}};
    return a733_npu_platform_scopes(&fabric,&shared);
}
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
    const struct npu_board_session *s=(const void *)A733_BOARD_SESSION_ADDRESS;
    if(!c||!npu_board_session_valid(s)||c->magic!=NPU_BOARD_CONTRACT_MAGIC)return 0;
    const struct npu_board_boot_contract *record=npu_board_boot_record(c);
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
