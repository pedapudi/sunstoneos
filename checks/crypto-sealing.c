#define _GNU_SOURCE
#include "snapshot.h"
#include "a733_storage_digest.h"
#include "boot.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
const unsigned char a733_smp_build_id[32]={1};
static unsigned publications,publish_ok=1;
int a733_cpu_coherence_publish(uint64_t address,uint64_t bytes,uint64_t deadline)
{assert(address&&bytes>=824&&deadline==24000024);++publications;return publish_ok;}
int main(int argc,char **argv)
{
 assert(argc==2);
 struct crypto_session j={.board={.magic=A733_BOARD_SESSION_MAGIC,.version=2,.bytes=592,
 .epoch=1,.generation=2,.lease=3,.topology=4,.cold_master=5,.memory_authority=6,.recovery_authority=7,
 .workload=2,.pmic_owner=10,.usb0_owner=11,.fabric_hz={24000000,24000000,24000000},
 .usb_supply_uv={3300000,3300000,1200000,900000},.usb0_cold_reference=1},
 .magic=A733_CRYPTO_SESSION_MAGIC,.version=1,.bytes=sizeof(j),.owner=12,.authority=13};
 memset(j.board.image,1,32);memset(j.board.qualification,2,32);memset(j.board.recovery,2,32);
 memset(j.board.wiring,3,32);memset(j.board.memory,4,32);memset((uint8_t *)&j+offsetof(struct crypto_session,initial),5,128);
 storage_digest_bytes(crypto_inputs,83,j.fixture);storage_digest_bytes(crypto_reference,160,j.reference);assert(crypto_session_valid(&j));
 void *mapped=mmap((void *)A733_BOARD_SESSION_ADDRESS,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);assert(mapped==(void *)A733_BOARD_SESSION_ADDRESS);
 struct a733_smp_contract c={.magic=A733_CRYPTO_CONTRACT_MAGIC,.version=2,.bytes=224,.epoch=1,.generation=2,
 .inverse_generation=~UINT64_C(2),.lease=3,.armed_at=24,.startup_deadline=24000024,
 .diagnostic_deadline=1440000024,.expires_at=1440000024,.reserved={A733_BOARD_SESSION_ADDRESS,sizeof(j)}};
 memcpy(c.build_id,a733_smp_build_id,32);storage_digest_bytes(&j,sizeof(j),(uint8_t *)&c.reserved[2]);
 for(unsigned test=0;test<8;++test){pid_t child=fork();assert(child>=0);if(!child){
  memcpy(mapped,&j,sizeof(j));struct a733_smp_contract bad=c;
  switch(test){case 0:((struct crypto_session *)mapped)->owner++;break;
  case 1:((struct crypto_session *)mapped)->board.pmic_owner++;break;
  case 2:((struct crypto_session *)mapped)->memory[0]^=1;break;
  case 3:bad.reserved[1]=592;break;case 4:bad.reserved[0]+=8;break;
  case 5:bad.magic^=1;break;case 6:publish_ok=0;break;case 7:bad.reserved[2]^=1;break;}
  assert(!crypto_session_prepare(&bad)&&!crypto_admitted_session());assert(publications==(test==6));
  memcpy(mapped,&j,sizeof(j));publish_ok=1;assert(!crypto_session_prepare(&c)&&!crypto_admitted_session());_exit(0);
 }int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);}
 memcpy(mapped,&j,sizeof(j));assert(crypto_session_prepare(&c)&&publications==1);
 const struct crypto_session *sealed=crypto_admitted_session();assert(sealed&&!memcmp(sealed,&j,sizeof(j)));
 ((struct crypto_session *)mapped)->owner++;((struct crypto_session *)mapped)->authority++;
 ((struct crypto_session *)mapped)->memory[0]^=1;((struct crypto_session *)mapped)->board.pmic_owner++;
 assert(!memcmp(sealed,&j,sizeof(j))&&!crypto_session_prepare(&c)&&publications==1);
 FILE *out=fopen(argv[1],"wb");assert(out&&fwrite(&j,1,sizeof(j),out)==sizeof(j)&&!fclose(out));assert(!munmap(mapped,4096));
 puts("Crypto copied-session digest, prefix/extension tamper, failed publication and once-only sealing passed");
}
