#include "input.h"
#include "a733_storage_digest.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t tick=1000;
static unsigned clock_calls,expire_at;
static int clock_now(void *unused,uint64_t *out)
{(void)unused;++clock_calls;*out=clock_calls==expire_at?1000000:tick;return 1;}
static size_t read_file(const char *path,void *out,size_t capacity)
{FILE *f=fopen(path,"rb");assert(f);size_t n=fread(out,1,capacity,f);assert(!ferror(f)&&fgetc(f)==EOF&&!fclose(f));return n;}
/* Capacity-sized buffers are static; together they exceed a safe stack. */
static uint8_t packet[32+JPEG_INPUT_CAPACITY+JPEG_OUTPUT_CAPACITY],data[sizeof(packet)],frozen[JPEG_INPUT_CAPACITY];
static struct jpeg_input p;
int main(int argc,char **argv)
{
 assert(argc==5||argc==6);struct a733_jpeg_session original;
 assert(read_file(argv[1],&original,sizeof(original)+0)==sizeof(original));
 size_t bytes=read_file(argv[2],packet,sizeof(packet));
 unsigned width=(unsigned)strtoul(argv[3],NULL,10),height=(unsigned)strtoul(argv[4],NULL,10);
 for(unsigned variant=argc==6?12:0;variant<15;++variant){
  memset(&p,0,sizeof(p));struct a733_jpeg_session s=original;memcpy(data,packet,bytes);
  clock_calls=expire_at=0;tick=1000;size_t offset=0;int result=0;
  if(variant==3)data[16]^=1;
  if(variant==4)data[8]=0;
  if(variant==5)data[bytes-1]^=1;
  if(variant==6)s.reference[0]^=1;
  if(variant==12){unsigned has_oracle=0;for(unsigned i=0;i<32;++i)has_oracle|=s.reference[i];
   memset(data+12,0,4);if(!has_oracle)data[12]=1;}
  if(variant==13){unsigned oracle=(unsigned)data[12]|((unsigned)data[13]<<8);if(!oracle)continue;
   --oracle;data[12]=(uint8_t)oracle;data[13]=(uint8_t)(oracle>>8);}
  if(variant==14){unsigned input=(unsigned)data[8]|((unsigned)data[9]<<8);data[32]=0;
   storage_digest_bytes(data+32,input,s.fixture);}
  while(offset<bytes&&result>=0){
   size_t n=jpeg_input_remaining(&p);size_t chunk=variant%3==0?1:variant%3==1?17:512;
   if(n>chunk)n=chunk;if(n>bytes-offset)n=bytes-offset;
   if(variant==7&&offset>=32)s.board.generation++;
   if(variant==8&&offset>=32)s.authority++;
   if(variant==9&&offset>=32)tick=999;
   if(variant==10&&p.header_used==32&&p.received+n==p.input_bytes+p.oracle_bytes)expire_at=clock_calls+2;
   result=jpeg_input_feed(&p,&s,clock_now,NULL,1000000,data+offset,n);offset+=n;
  }
  if(variant<3||variant==11){
   assert(result==1&&jpeg_input_ready(&p)&&offset==bytes&&p.width==width&&p.height==height);
   assert(p.luma_bytes==p.stride*p.rows&&p.luma_bytes<=JPEG_PLANE_CAPACITY&&p.chroma_bytes==p.luma_bytes/2);
   for(unsigned b=0;b<3;++b){size_t used=b==0?p.input_bytes:b==1?p.luma_bytes:p.chroma_bytes;
    assert(jpeg_input_mapped(&p,b)>=used&&jpeg_input_mapped(&p,b)<used+4096&&!(jpeg_input_mapped(&p,b)&4095));}
   assert(p.output_bytes==width*height+2*((width+1)/2)*((height+1)/2));
   memcpy(frozen,p.bytes,sizeof(frozen));
   assert(jpeg_input_feed(&p,&s,clock_now,NULL,1000000,data,1)==-1);
   assert(!memcmp(frozen,p.bytes,sizeof(frozen))&&jpeg_input_ready(&p));
  }else assert(result==-1&&p.failed&&!jpeg_input_ready(&p));
 }
 /* A padded luma plane of at most 307,200 bytes: 640x480 and 1920x160 fit;
  * 641x480, 640x481 and 1920x1080 exceed it, as do shapes past the decoder's
  * 1920x1080 limit. */
 static struct jpeg_input geometry;struct a733_video_jpeg pic={.width=640,.height=480};
 assert(jpeg_input_geometry(&geometry,&pic)&&geometry.output_bytes==JPEG_OUTPUT_CAPACITY&&geometry.luma_bytes==JPEG_PLANE_CAPACITY);
 assert(jpeg_input_mapped(&geometry,1)==307200&&jpeg_input_mapped(&geometry,2)==155648);
 pic.width=1920;pic.height=160;assert(jpeg_input_geometry(&geometry,&pic)&&geometry.output_bytes==460800);
 pic.width=639;pic.height=479;assert(jpeg_input_geometry(&geometry,&pic)&&geometry.output_bytes==639*479+2*320*240);
 const unsigned refused[][2]={{641,480},{640,481},{1920,1080},{1921,1},{1,1081},{0,1},{1,0}};
 for(unsigned i=0;i<sizeof(refused)/sizeof(refused[0]);++i){
  pic.width=(uint16_t)refused[i][0];pic.height=(uint16_t)refused[i][1];assert(!jpeg_input_geometry(&geometry,&pic));}
 puts("JPEG runtime fragmented input, hashes, mode, generation, authority, time, publication and bounded geometry passed");
}
