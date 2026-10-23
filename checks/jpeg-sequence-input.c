/* Sanitized sequence-session ingestion through the actual JPEG input code.
 * argv: session, steps file, chunk size, then the three expected mapped
 * capacities. Each step record is LE32 length, signed expected result of the
 * last fragment, deliver-before flag, width and height, then the bytes. A
 * deliver-before flag marks every published result delivered first. */
#include "input.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t tick=1000;
static int clock_now(void *unused,uint64_t *out){(void)unused;*out=++tick;return 1;}
static uint32_t le32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint8_t steps[4<<20];
static struct jpeg_input p,early;
int main(int argc,char **argv)
{
    assert(argc==7);struct a733_jpeg_session s;
    FILE *f=fopen(argv[1],"rb");assert(f&&fread(&s,1,sizeof(s),f)==sizeof(s)&&fgetc(f)==EOF&&!fclose(f));
    f=fopen(argv[2],"rb");assert(f);size_t total=fread(steps,1,sizeof(steps),f);assert(!ferror(f)&&feof(f)&&!fclose(f));
    size_t chunk=strtoul(argv[3],NULL,10);uint32_t mapped[3];
    for(unsigned i=0;i<3;++i)mapped[i]=(uint32_t)strtoul(argv[4+i],NULL,10);
    unsigned commits=0,rejected=0;
    for(size_t at=0;at<total;){
        assert(!rejected&&total-at>=20);
        uint32_t n=le32(steps+at),deliver=le32(steps+at+8),width=le32(steps+at+12),height=le32(steps+at+16);
        int expect=(int)le32(steps+at+4);const uint8_t *bytes=steps+at+20;at+=20+n;assert(at<=total);
        if(deliver)p.delivered=jpeg_input_ready(&p);
        int result=0;
        for(size_t used=0;used<n&&result>=0;){
            size_t k=n-used<chunk?n-used:chunk;
            /* A fragment never spans two frames or a header and its body;
             * the USB task reads at most the bytes still owed. */
            size_t left=jpeg_input_remaining(&p);if(left&&k>left)k=left;
            result=jpeg_input_feed(&p,&s,clock_now,NULL,1000000,bytes+used,k);used+=k;
        }
        assert(result==expect);
        if(result<0){assert(p.failed&&jpeg_input_ready(&p)==commits);rejected=1;continue;}
        if(result==1){
            ++commits;assert(jpeg_input_ready(&p)==commits&&p.sequence&&!jpeg_input_partial(&p));
            assert(p.width==width&&p.height==height&&p.input_bytes<=mapped[0]&&p.luma_bytes<=mapped[1]&&p.chroma_bytes<=mapped[2]);
            for(unsigned b=0;b<3;++b)assert(jpeg_input_mapped(&p,b)==mapped[b]);
            assert(!memcmp(p.bytes,bytes+32,p.input_bytes));
            /* On a copy of the state, the next frame is refused while this
             * image's result is undelivered. */
            if(commits<p.count){
                const uint8_t magic[4]={0x4a,0x50,0x47,0x49};early=p;
                assert(jpeg_input_feed(&early,&s,clock_now,NULL,1000000,magic,4)==-1&&early.failed);
            }
        }
    }
    printf("commits=%u rejected=%u\n",commits,rejected);
    return 0;
}
