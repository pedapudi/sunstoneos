#include "a733_h264_application.h"
#include "stream-profile.h"
#include "a733_rail_leases.h"
#ifdef SUNSTONE_H264_FULLHD
#include "fullhd-boot.h"
#include "a733_device_memory_layout.h"
#endif
#define a733_smp_contract_valid board_contract_unused
#include "../board/admission.c"
#undef a733_smp_contract_valid
int a733_h264_session_valid(const struct a733_h264_session *s)
{
    return s&&a733_board_session_valid(&s->board,0)&&s->board.workload==2&&
        s->magic==A733_H264_SESSION_MAGIC&&
#ifdef SUNSTONE_H264_FULLHD
        s->version==H264_STREAM_SESSION_VERSION&&
#else
        (s->version==1||s->version==2)&&
#endif
        s->bytes==sizeof(*s)&&(s->version==1||!present(s->reference,32))&&
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
    if(!c||!a733_h264_session_valid(s)||c->magic!=A733_H264_CONTRACT_MAGIC)return 0;
    uint8_t hash[32];storage_digest_bytes(s,sizeof(*s),hash);
    struct a733_smp_contract base=*c;
#ifdef SUNSTONE_H264_FULLHD
    const struct h264_fullhd_boot_contract *record=h264_fullhd_boot_record(c);
    const struct a733_device_memory_layout layout={A733_DEVICE_MEMORY_LAYOUT,
        A733_DEVICE_WB_START,A733_DEVICE_WB_END,A733_DEVICE_NC_START,A733_DEVICE_NC_END};
    if(!record||c->bytes!=sizeof(*record)||!a733_device_memory_handoff_valid(&layout,c->reserved)||
       memcmp(hash,record->session_digest,32)||memcmp(record->entry_digest,s->board.qualification,32))return 0;
    /* This local board view is consumed synchronously by the existing pure
     * validator; memory admission always receives the original protected c. */
    base.bytes=sizeof(base);base.reserved[0]=A733_BOARD_SESSION_ADDRESS;
#else
    if(c->reserved[1]!=sizeof(*s)||memcmp(hash,&c->reserved[2],32))return 0;
#endif
    base.magic=A733_BOARD_CONTRACT_MAGIC;base.reserved[1]=sizeof(s->board);
    storage_digest_bytes(&s->board,sizeof(s->board),(void *)&base.reserved[2]);
    return a733_board_contract_valid(&base,&s->board,a733_smp_build_id,now);
}
