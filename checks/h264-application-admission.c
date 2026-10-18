#define _GNU_SOURCE
#include "a733_h264_application.h"
#include "a733_storage_digest.h"
#include "boot.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
const unsigned char a733_smp_build_id[32]={1};
static unsigned publications;
int a733_cpu_coherence_publish(uint64_t address,uint64_t bytes,uint64_t deadline)
{assert(address&&bytes>=sizeof(struct a733_h264_session)&&deadline==24000024);++publications;return 1;}
int main(int argc,char **argv)
{
    struct a733_h264_session j={.board={.magic=A733_BOARD_SESSION_MAGIC,.version=2,.bytes=592,
        .epoch=1,.generation=2,.lease=3,.topology=4,.cold_master=5,.memory_authority=6,.recovery_authority=7,
        .workload=2,.pmic_owner=10,.usb0_owner=11,.fabric_hz={24000000,24000000,24000000},
        .usb_supply_uv={3300000,3300000,1200000,900000},.usb0_cold_reference=1},
        .magic=A733_H264_SESSION_MAGIC,.version=1,.bytes=sizeof(j),.owner=12,.authority=13,
        .module=1,.module_hz=234000000,.iommu_gates={0x10007,0x10007},.supply_uv=800000,.supply_rail=1};
    memset(j.board.image,1,32);memset(j.board.qualification,2,32);memset(j.board.recovery,2,32);
    memset(j.board.wiring,3,32);memset(j.board.memory,4,32);memset(j.initial,5,192);
    assert(a733_h264_session_valid(&j));
    void *mapped=mmap((void *)A733_BOARD_SESSION_ADDRESS,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    assert(mapped==(void *)A733_BOARD_SESSION_ADDRESS);memcpy(mapped,&j,sizeof(j));
    struct a733_smp_contract c={.magic=A733_H264_CONTRACT_MAGIC,.version=2,.bytes=224,.epoch=1,.generation=2,
        .inverse_generation=~UINT64_C(2),.lease=3,.armed_at=24,.startup_deadline=24000024,
        .diagnostic_deadline=1440000024,.expires_at=1440000024,.reserved={A733_BOARD_SESSION_ADDRESS,sizeof(j)}};
    memcpy(c.build_id,a733_smp_build_id,32);storage_digest_bytes(&j,sizeof(j),(uint8_t *)&c.reserved[2]);
    assert(a733_smp_contract_valid(&c,100));
    ((struct a733_h264_session *)mapped)->module_hz++;assert(!a733_smp_contract_valid(&c,100));memcpy(mapped,&j,sizeof(j));
    ((struct a733_h264_session *)mapped)->authority++;
    assert(!h264_session_prepare(&c)&&!h264_admitted_session()&&!publications);
    memcpy(mapped,&j,sizeof(j));assert(h264_session_prepare(&c)&&publications==1);
    const struct a733_h264_session *sealed=h264_admitted_session();
    assert(sealed&&!memcmp(sealed,&j,sizeof(j)));
    ((struct a733_h264_session *)mapped)->owner++;
    ((struct a733_h264_session *)mapped)->module_hz++;
    ((struct a733_h264_session *)mapped)->memory[0]^=1;
    assert(!memcmp(sealed,&j,sizeof(j))&&!h264_session_prepare(&c)&&publications==1);
    memcpy(mapped,&j,sizeof(j));
    c.reserved[1]=592;assert(!a733_smp_contract_valid(&c,100));c.reserved[1]=sizeof(j);
    j.supply_rail=2;assert(!a733_h264_session_valid(&j));j.supply_rail=1;
    j.board.workload=3;assert(!a733_h264_session_valid(&j));j.board.workload=2;
    j.board.deadline_us=1;assert(!a733_h264_session_valid(&j));j.board.deadline_us=0;
    assert(argc==2);FILE*f=fopen(argv[1],"wb");assert(f&&fwrite(&j,1,sizeof(j),f)==sizeof(j)&&!fclose(f));
    assert(!munmap(mapped,4096));puts("H264 full-session extension/digest/rail/workload admission passed");return 0;
}
