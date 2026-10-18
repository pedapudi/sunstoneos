#define _GNU_SOURCE
#include "crypto_session.h"
#include "a733_storage_digest.h"
#include "boot.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
const unsigned char a733_smp_build_id[32]={1};
int main(int argc,char **argv)
{
 struct crypto_session s={.board={.magic=A733_BOARD_SESSION_MAGIC,.version=2,.bytes=592,
 .epoch=1,.generation=2,.lease=3,.topology=4,.cold_master=5,.memory_authority=6,.recovery_authority=7,
 .workload=2,.pmic_owner=10,.usb0_owner=11,.fabric_hz={24000000,24000000,24000000},
 .usb_supply_uv={3300000,3300000,1200000,900000},.usb0_cold_reference=1},
 .magic=A733_CRYPTO_SESSION_MAGIC,.version=1,.bytes=sizeof(s),.owner=12,.authority=13};
 memset(s.board.image,1,32);memset(s.board.qualification,2,32);memset(s.board.recovery,2,32);
 memset(s.board.wiring,3,32);memset(s.board.memory,4,32);memset((uint8_t *)&s+offsetof(struct crypto_session,initial),5,128);
 storage_digest_bytes(crypto_inputs,83,s.fixture);storage_digest_bytes(crypto_reference,160,s.reference);
 assert(crypto_session_valid(&s));
 void *mapped=mmap((void *)A733_BOARD_SESSION_ADDRESS,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
 assert(mapped==(void *)A733_BOARD_SESSION_ADDRESS);memcpy(mapped,&s,sizeof(s));
 struct a733_smp_contract c={.magic=A733_CRYPTO_CONTRACT_MAGIC,.version=2,.bytes=224,.epoch=1,.generation=2,
 .inverse_generation=~UINT64_C(2),.lease=3,.armed_at=24,.startup_deadline=24000024,
 .diagnostic_deadline=1440000024,.expires_at=1440000024,.reserved={A733_BOARD_SESSION_ADDRESS,sizeof(s)}};
 memcpy(c.build_id,a733_smp_build_id,32);storage_digest_bytes(&s,sizeof(s),(uint8_t *)&c.reserved[2]);
 assert(a733_smp_contract_valid(&c,100));
 for(unsigned i=592;i<sizeof(s);++i){((uint8_t *)mapped)[i]^=1;assert(!a733_smp_contract_valid(&c,100));((uint8_t *)mapped)[i]^=1;}
 c.reserved[1]=592;assert(!a733_smp_contract_valid(&c,100));
 struct crypto_session bad=s;bad.owner=10;assert(!crypto_session_valid(&bad));
 bad=s;bad.board.workload=3;assert(!crypto_session_valid(&bad));
 for(unsigned i=0;i<4;++i){bad=s;memset((uint8_t *)&bad+offsetof(struct crypto_session,initial)+32*i,0,32);assert(!crypto_session_valid(&bad));}
 assert(argc==2);FILE *f=fopen(argv[1],"wb");assert(f&&fwrite(&s,1,sizeof(s),f)==sizeof(s)&&!fclose(f));
 assert(!munmap(mapped,4096));puts("Crypto complete session binding and missing-authority refusals passed");
}
