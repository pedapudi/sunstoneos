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
static unsigned publications,ok=1;
static const struct thermal_session *expected;
int a733_cpu_coherence_publish(uint64_t a,uint64_t n,uint64_t d)
{assert(a&&n==816&&d==24000100);assert(!memcmp((void *)(uintptr_t)a,expected,808));
 const unsigned *receipt=(void *)(uintptr_t)(a+808);assert(receipt[0]==1&&receipt[1]==1);++publications;return ok;}
int main(int argc,char **argv)
{
 assert(argc==2);struct thermal_session s;FILE *f=fopen(argv[1],"rb");assert(f&&fread(&s,1,sizeof(s),f)==sizeof(s)&&fgetc(f)==EOF&&!fclose(f));
 expected=&s;assert(thermal_session_valid(&s));
 void *mapped=mmap((void *)A733_BOARD_SESSION_ADDRESS,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);assert(mapped==(void *)A733_BOARD_SESSION_ADDRESS);memcpy(mapped,&s,sizeof(s));
 struct a733_smp_contract c={.magic=A733_THERMAL_CONTRACT_MAGIC,.version=2,.bytes=224,.epoch=s.board.epoch,.generation=s.board.generation,.inverse_generation=~s.board.generation,.lease=s.board.lease,.armed_at=100,.startup_deadline=24000100,.diagnostic_deadline=1440000100,.expires_at=1440000100,.reserved={A733_BOARD_SESSION_ADDRESS,sizeof(s)}};
 memcpy(c.build_id,s.board.image,32);storage_digest_bytes(&s,sizeof(s),(void *)&c.reserved[2]);assert(a733_smp_contract_valid(&c,101));
 for(unsigned i=592;i<sizeof(s);++i){((uint8_t *)mapped)[i]^=1;assert(!a733_smp_contract_valid(&c,101));((uint8_t *)mapped)[i]^=1;}
 for(unsigned test=0;test<4;++test){pid_t child=fork();assert(child>=0);if(!child){
  memcpy(mapped,&s,sizeof(s));
  if(test==0)((struct thermal_session *)mapped)->owner^=16;
  if(test==1)((struct thermal_session *)mapped)->initial[0]^=1;
  if(test==2)ok=0;
  if(test==3)c.reserved[1]=592;
  assert(!thermal_session_prepare(&c)&&!thermal_admitted_session());assert(publications==(test==2));
  memcpy(mapped,&s,sizeof(s));ok=1;assert(!thermal_session_prepare(&c));_exit(0);
 }int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));}
 assert(thermal_session_prepare(&c)&&publications==1);memset(mapped,0,808);
 assert(!memcmp(thermal_admitted_session(),&s,sizeof(s))&&!thermal_session_prepare(&c));
 puts("PASS actual complete session/contract validators, full digest refusals, retained copy and receipt publication");
}
