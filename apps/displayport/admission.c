#include "a733_displayport_application.h"
#include "boot-record.h"
#include "a733_device_memory_layout.h"
#define a733_smp_contract_valid board_contract_unused
#include "../board/admission.c"
#undef a733_smp_contract_valid
_Static_assert(A733_BOARD_SUPERVISED_SECONDS==A733_DISPLAYPORT_SESSION_SECONDS,
    "DP and board providers must use the same session lifetime");
/* Read from the linked ELF by host preparation. */
__attribute__((used,retain)) const uint64_t displayport_geometry[13]={
    A733_DISPLAYPORT_SESSION_MAGIC,1,sizeof(struct a733_displayport_session),
    sizeof(struct displayport_boot_contract),A733_BOARD_SESSION_ADDRESS,
    A733_DISPLAYPORT_SESSION_SECONDS,A733_DISPLAYPORT_RESULT_BYTES,
    A733_DISPLAYPORT_WIDTH,A733_DISPLAYPORT_HEIGHT,A733_DISPLAYPORT_STRIDE,
    A733_DISPLAYPORT_ALLOCATION_BYTES,A733_DISPLAYPORT_PIXEL_OFFSET,
    A733_DISPLAYPORT_PIXEL_IOVA};
int a733_displayport_session_valid(const struct a733_displayport_session *s)
{
    return s&&a733_board_session_valid(&s->board,0)&&s->board.workload==2&&
        s->magic==A733_DISPLAYPORT_SESSION_MAGIC&&s->version==1&&s->bytes==sizeof(*s)&&
        a733_displayport_fields_valid(s)&&a733_displayport_parameters_valid(s)&&
        present(s->isolation,32)&&present(s->memory,32)&&present(s->analog,32)&&
        present(s->power,32)&&present(s->serdes,32)&&present(s->supervision,32);
}
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
    const struct a733_displayport_session *s=(const void *)A733_BOARD_SESSION_ADDRESS;
    if(!c||!a733_displayport_session_valid(s)||c->magic!=A733_DISPLAYPORT_CONTRACT_MAGIC)return 0;
    const struct displayport_boot_contract *record=displayport_boot_record(c);
    const struct a733_device_memory_layout layout={A733_DEVICE_MEMORY_LAYOUT,
        A733_DEVICE_WB_START,A733_DEVICE_WB_END,A733_DEVICE_NC_START,A733_DEVICE_NC_END};
    uint8_t hash[32];storage_digest_bytes(s,sizeof(*s),hash);
    if(!record||c->reserved[1]!=s->board.memory_authority||c->bytes!=sizeof(*record)||
       !a733_device_memory_handoff_valid(&layout,c->reserved)||
       memcmp(hash,record->session_digest,32)||
       memcmp(record->entry_digest,s->board.qualification,32))return 0;
    struct a733_smp_contract base=*c;
    base.magic=A733_BOARD_CONTRACT_MAGIC;base.bytes=sizeof(base);
    base.reserved[0]=A733_BOARD_SESSION_ADDRESS;base.reserved[1]=sizeof(s->board);
    storage_digest_bytes(&s->board,sizeof(s->board),(void *)&base.reserved[2]);
    return a733_board_contract_valid(&base,&s->board,a733_smp_build_id,now);
}
