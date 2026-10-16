#include "result.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void put(uint8_t *p,uint64_t x,unsigned n)
{for(unsigned i=0;i<n;++i)p[i]=(uint8_t)(x>>(8*i));}
int main(int argc,char **argv)
{
    assert(argc==2);FILE *out=fopen(argv[1],"wb");assert(out);
    uint8_t p[512],digest[32],challenge[32],build[32],request[64]={0};
    for(unsigned i=0;i<512;++i)p[i]=(uint8_t)i;
    const unsigned sizes[]={0,1,55,56,63,64,65,480};
    for(unsigned i=0;i<8;++i){a733_observe_digest(p,sizes[i],digest);assert(fwrite(digest,1,32,out)==32);}
    struct a733_observe_identity id={5,7,9,24,72000000};
    struct a733_observe_snapshot s={.start=100,.end=200};
    for(unsigned c=0;c<8;++c){s.core[c][0]=32;s.core[c][1]=c+1;s.core[c][2]=1;s.core[c][4]=c?0:32;}
    for(unsigned i=0;i<32;++i){challenge[i]=(uint8_t)(i+1);build[i]=(uint8_t)(i+32);}
    assert(a733_observe_frame(p,&id,&s,challenge,build));assert(fwrite(p,1,512,out)==512);
    assert(a733_observe_frame(p,&id,NULL,challenge,build));assert(fwrite(p,1,512,out)==512);
    for(unsigned c=0;c<8;++c){s.core[c][3]=1;assert(!a733_observe_frame(p,&id,&s,challenge,build));s.core[c][3]=0;}
    s.end=id.deadline;assert(!a733_observe_frame(p,&id,&s,challenge,build));s.end=200;
    s.core[0][4]=31;assert(!a733_observe_frame(p,&id,&s,challenge,build));s.core[0][4]=32;
    put(request,A733_OBSERVE_MESSAGE_MAGIC,4);put(request+4,1,4);put(request+8,A733_OBSERVE_COMMAND,4);
    put(request+12,48,4);put(request+16,5,8);put(request+24,7,8);memcpy(request+32,challenge,32);
    uint32_t seq=0;uint8_t got[32];assert(a733_observe_request(request,&id,&seq,got)&&seq==1&&!memcmp(got,challenge,32));
    request[24]++;assert(!a733_observe_request(request,&id,&seq,got));request[24]--;
    memset(request+32,0,32);assert(!a733_observe_request(request,&id,&seq,got));
    assert(!fclose(out));puts("PASS result digest vectors, immutable success/pending encoding and invalid identity/result refusal");
}
