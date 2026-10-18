#include "a733_video_jpeg.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv)
{
 assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);uint8_t data[4096];size_t n=fread(data,1,sizeof(data),f);fclose(f);assert(n==520);
 struct a733_video_jpeg j={0};struct a733_video_jpeg_tables t={0};
 assert(a733_video_jpeg_parse(data,n,&j)&&a733_video_jpeg_prepare(&j,&t));
 assert(j.width==48&&j.height==32&&j.file_bytes==520&&j.scan_bytes>0&&t.format==3&&t.size==0x10002);
 assert(j.quant_select[0]==0&&j.quant_select[1]==1&&j.quant_select[2]==1);
 assert(j.dc_select[0]==0&&j.ac_select[0]==0&&j.dc_select[1]==1&&j.ac_select[1]==1);
 const unsigned positions[]={0,14,10,39};const uint8_t expected[2][4]={{8,4,8,2},{4,8,2,4}};
 for(unsigned table=0;table<2;++table)for(unsigned i=0;i<4;++i){
  unsigned pos=positions[i];assert(j.quant[table][pos]==expected[table][i]);
  assert(t.quant[64*table+pos]==((table?0u:0x4000u)|(pos<<8)|expected[table][i]));
 }
 assert(j.huffman[0][0].counts[3]==12&&j.huffman[0][1].counts[4]==12);
 assert(j.huffman[0][0].symbols[0]==0&&j.huffman[0][1].symbols[0]==11);
 for(unsigned table=0;table<2;++table){
  const struct a733_video_huffman *h=&j.huffman[1][table];unsigned ac=0,eob=0,zrl=0;
  assert(h->count>2&&h->counts[5+table]==h->count);
  for(unsigned i=0;i<h->count;++i){ac+=!!(h->symbols[i]&15);eob+=h->symbols[i]==0;zrl+=h->symbols[i]==0xf0;}
  assert(ac&&eob==1&&zrl==1);
 }
 assert(memcmp(t.huffman,t.huffman+128,128*sizeof(uint32_t)));
 puts("PASS nonconstant baseline420 parser, distinct quantization/Huffman tables and native preparation");return 0;
}
