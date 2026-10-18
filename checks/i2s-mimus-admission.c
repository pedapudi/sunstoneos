#include "../apps/i2s/session.h"
#include "a733_storage_digest.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
const uint8_t i2s_recorded_pcm_digest[32]={1};
const uint8_t a733_smp_build_id[32]={1};
int main(int argc,char **argv)
{
 assert(argc==4);struct i2s_session s={.magic=I2S_SESSION_MAGIC,.version=2,.bytes=sizeof(s),
  .epoch=1,.generation=2,.lease=3,.image={1},.evidence={1},.memory={1},.recovery={1},.pcm={1},.capture_samples=80000};
 s.shared.identity=s.shared.pmic.identity=s.shared.usb.handoff=2;
 s.shared.pmic.owner=4;s.shared.usb.owner=5;s.dma.allowed.owner=6;s.dma.allowed.handoff=s.dma.evidence=3;
 s.dma.established=A733_DMA_HANDOFF_SUPERVISED;s.dma.owned_channels=0xffff;s.dma.service_channels=1;s.dma.controller=0x04601000;
 for(unsigned i=0;i<2;++i){
  FILE *f=fopen(argv[i+1],"rb");assert(f&&fseek(f,0,SEEK_END)==0);long bytes=ftell(f);assert(bytes>0&&fseek(f,0,SEEK_SET)==0);
  uint8_t *data=malloc((size_t)bytes);assert(data&&fread(data,1,(size_t)bytes,f)==(size_t)bytes);fclose(f);
  s.models.bytes[i]=(uint64_t)bytes;storage_digest_bytes(data,(size_t)bytes,s.models.sha256[i]);free(data);
 }
 assert(i2s_session_valid(&s));FILE *f=fopen(argv[3],"wb");assert(f&&fwrite(&s,1,sizeof(s),f)==sizeof(s));fclose(f);
 s.capture_samples=16000;assert(i2s_session_valid(&s));s.capture_samples=15999;assert(!i2s_session_valid(&s));
 s.capture_samples=80001;assert(!i2s_session_valid(&s));s.capture_samples=80000;
 s.direction=1;assert(!i2s_session_valid(&s));s.direction=0;s.reserved=1;assert(!i2s_session_valid(&s));s.reserved=0;
 s.version=1;assert(!i2s_session_valid(&s));s.version=2;
 --s.models.bytes[0];assert(!i2s_session_valid(&s));++s.models.bytes[0];s.models.sha256[1][0]^=1;assert(!i2s_session_valid(&s));
 puts("PASS I2S transcription session version, duration, direction and unchanged model admission");return 0;
}
