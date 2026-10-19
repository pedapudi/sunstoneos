#include "a733_h264_input.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static size_t read_file(const char *path,void *p,size_t limit)
{FILE *f=fopen(path,"rb");assert(f);size_t n=fread(p,1,limit,f);assert(fgetc(f)==EOF&&!fclose(f));return n;}
static int clock_at(void *v,uint64_t *now){*now=*(uint64_t *)v;return 1;}
static int feed_at(struct a733_h264_input *p,const struct a733_h264_session *s,
                   uint64_t now,uint64_t deadline,const uint8_t *bytes,size_t n)
{return a733_h264_input_feed(p,s,clock_at,&now,deadline,bytes,n);}
struct late_clock {unsigned calls;};
static int commit_late(void *v,uint64_t *now)
{struct late_clock *p=v;*now=++p->calls==1?10000:100000;return 1;}
static int feed(struct a733_h264_input *input,const struct a733_h264_session *session,
                const uint8_t *wire,size_t bytes,size_t chunk)
{
    size_t offset=0;int result=0;
    while(offset<bytes&&result>=0){
        size_t n=a733_h264_input_remaining(input);if(n>chunk)n=chunk;if(n>bytes-offset)n=bytes-offset;
        assert(n);result=feed_at(input,session,100+offset,100000,wire+offset,n);offset+=n;
        assert(a733_h264_input_ready(input)==(result==1));
    }
    return result;
}
int main(int argc,char **argv)
{
    assert(argc==3);struct a733_h264_session s;
    assert(read_file(argv[1],&s,sizeof(s))==sizeof(s));
    uint8_t wire[32+4096+2304];size_t bytes=read_file(argv[2],wire,sizeof(wire));assert(bytes>32);
    for(size_t chunk=1;chunk<=512;chunk=chunk==1?17:chunk==17?512:513){
        struct a733_h264_input input={0};
        assert(feed(&input,&s,wire,bytes,chunk)==1);
        assert(a733_h264_input_ready(&input));
        assert(input.stream_bytes+input.oracle_bytes+32==bytes);
        assert(!memcmp(input.stream,wire+32,input.stream_bytes));
        assert(!input.oracle_bytes||!memcmp(input.oracle,wire+32+input.stream_bytes,input.oracle_bytes));
        uint8_t saved[4096];memcpy(saved,input.stream,sizeof(saved));
        assert(feed_at(&input,&s,10000,100000,wire,1)==-1);
        assert(a733_h264_input_ready(&input)&&!memcmp(saved,input.stream,sizeof(saved)));
    }
    /* Actual producer header, then one changed fact per refusal. */
    for(unsigned variant=0;variant<12;++variant){
        struct a733_h264_input input={0};uint8_t damaged[sizeof(wire)];memcpy(damaged,wire,bytes);
        struct a733_h264_session changed=s;
        if(variant==0)damaged[0]^=1;
        if(variant==1)damaged[8]=damaged[9]=damaged[10]=damaged[11]=0;
        if(variant==2)damaged[9]=0x20; /* >4096 */
        if(variant==3)damaged[12]=1; /* Oracle length must be0or2304. */
        if(variant==4)damaged[16]^=1;
        if(variant==5)damaged[24]^=1;
        if(variant==6)damaged[32]^=1; /* Sealed stream digest. */
        if(variant==7){memset(changed.reference,0,32);if(wire[12]==0&&wire[13]==0&&wire[14]==0&&wire[15]==0)memset(changed.reference,1,32);}
        if(variant<=7){assert(feed(&input,&changed,damaged,bytes,512)==-1);assert(!a733_h264_input_ready(&input));continue;}
        assert(feed_at(&input,&s,100,100000,wire,32)==0);
        if(variant==8)changed.board.generation++;
        if(variant==9)changed.authority++;
        if(variant==10)changed.fixture[0]^=1;
        assert(feed_at(&input,&changed,variant==11?100000:200,100000,wire+32,1)==-1);
        assert(!a733_h264_input_ready(&input));
    }
    struct a733_h264_input input={0};
    assert(feed_at(&input,&s,100,100000,wire,32)==0);
    assert(feed_at(&input,&s,99,100000,wire+32,1)==-1);
    struct a733_h264_input late={0};
    assert(feed(&late,&s,wire,bytes-1,512)==0);
    struct late_clock clock={0};
    assert(a733_h264_input_feed(&late,&s,commit_late,&clock,100000,wire+bytes-1,1)==-1);
    assert(!a733_h264_input_ready(&late));
    puts("PASS bounded fragmented upload, immutable publication, session binding, late/hash/mode refusal");return 0;
}
