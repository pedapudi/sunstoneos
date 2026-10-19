#include "a733_h264_application.h"
#include "a733_storage_digest.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv)
{
    assert(argc==3);
    struct a733_h264_session s={.board={.magic=A733_BOARD_SESSION_MAGIC,.version=2,.bytes=592,
        .epoch=1,.generation=2,.lease=3,.topology=4,.cold_master=5,.memory_authority=6,.recovery_authority=7,
        .workload=2,.pmic_owner=10,.usb0_owner=11,.fabric={0,0x80000000,0x80000000},
        .fabric_hz={24000000,24000000,24000000},.usb_supply_uv={3300000,3300000,1200000,900000},.usb0_cold_reference=1},
        .magic=A733_H264_SESSION_MAGIC,.version=3,.bytes=856,.owner=12,.authority=13,
        .module=1,.module_hz=234000000,.iommu_gates={0x10007,0x10007},.supply_uv=800000,.supply_rail=1};
    memset(s.board.image,1,32);memset(s.board.qualification,2,32);memset(s.board.recovery,2,32);
    memset(s.board.wiring,3,32);memset(s.board.memory,4,32);memset(s.initial,5,128);
    uint8_t manifest[256*96];FILE *f=fopen(argv[1],"rb");assert(f);
    size_t n=fread(manifest,1,sizeof(manifest),f);assert(n&&n%96==0&&fgetc(f)==EOF&&!fclose(f));
    storage_digest_bytes(manifest,n,s.fixture);assert(a733_h264_session_valid(&s));
    s.version=2;assert(!a733_h264_session_valid(&s));s.version=3;
    s.reference[0]=1;assert(!a733_h264_session_valid(&s));s.reference[0]=0;
    f=fopen(argv[2],"wb");assert(f&&fwrite(&s,1,sizeof(s),f)==sizeof(s)&&!fclose(f));
    puts("Full-HD producer session/profile/oracle-mode admission passed");return 0;
}
