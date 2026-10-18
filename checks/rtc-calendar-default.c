#include "a733_board_application.h"
#include "a733_storage_digest.h"
#include "boot.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
const unsigned char a733_smp_build_id[32]={1};
#ifndef EXPECT_STARTUP
#define EXPECT_STARTUP 1
#endif
_Static_assert(A733_BOARD_STARTUP_SECONDS==EXPECT_STARTUP,"Actual selected startup interval");
int main(int argc,char **argv)
{
 assert(argc==2);struct a733_board_session s;FILE *f=fopen(argv[1],"rb");assert(f&&fread(&s,1,sizeof(s),f)==sizeof(s)&&!fclose(f));
 memcpy(s.image,a733_smp_build_id,32);
 struct a733_smp_contract c={.magic=A733_BOARD_CONTRACT_MAGIC,.version=2,.bytes=224,
  .epoch=s.epoch,.generation=s.generation,.inverse_generation=~s.generation,.lease=s.lease,
  .armed_at=24,.startup_deadline=24+EXPECT_STARTUP*A733_SMP_COUNTER_HZ,
  .diagnostic_deadline=24+60*A733_SMP_COUNTER_HZ,.expires_at=24+60*A733_SMP_COUNTER_HZ,
  .reserved={A733_BOARD_SESSION_ADDRESS,592}};
 memcpy(c.build_id,a733_smp_build_id,32);storage_digest_bytes(&s,sizeof(s),(void *)&c.reserved[2]);
 assert(a733_board_contract_valid(&c,&s,a733_smp_build_id,100));
 c.startup_deadline=24+(EXPECT_STARTUP==1?6:1)*A733_SMP_COUNTER_HZ;
 assert(!a733_board_contract_valid(&c,&s,a733_smp_build_id,100));
 puts("PASS actual board startup validator preserves selected interval and60s lifetime");
}
