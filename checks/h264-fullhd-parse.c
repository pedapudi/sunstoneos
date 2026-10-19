#include "a733_video_h264.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv)
{
    assert(argc==3||argc==4);unsigned frame=(unsigned)strtoul(argv[2],NULL,10);
    FILE *f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));long bytes=ftell(f);
    assert(bytes>0&&bytes<=4194305&&!fseek(f,0,SEEK_SET));
    uint8_t *p=malloc((size_t)bytes);assert(p&&fread(p,1,(size_t)bytes,f)==(size_t)bytes&&!fclose(f));
    struct a733_h264_picture picture,before;memset(&before,0xa5,sizeof(before));picture=before;size_t config=123;
    int valid=a733_h264_parse_stream(A733_H264_FULLHD_STREAM,p,(size_t)bytes,frame,&picture,&config);
    if(argc==4){assert(!strcmp(argv[3],"invalid")&&!valid&&!memcmp(&picture,&before,sizeof(picture))&&config==123);free(p);return 0;}
    assert(valid&&picture.width==1920&&picture.height==1088&&picture.visible_width==1920&&picture.visible_height==1080&&
        !picture.crop_left&&!picture.crop_top&&!picture.crop_right&&picture.crop_bottom==8);
    const uint32_t *values=(const uint32_t *)&picture;
    for(unsigned i=0;i<sizeof(picture)/sizeof(uint32_t);++i)printf("%u%c",values[i],i+1==sizeof(picture)/sizeof(uint32_t)?'\n':' ');
    size_t header_end=picture.slice_offset+picture.header_bits/8;
    for(size_t n=0;n<header_end;++n){picture=before;size_t old=123;
        assert(!a733_h264_parse_stream(A733_H264_FULLHD_STREAM,p,n,frame,&picture,&old)&&!memcmp(&picture,&before,sizeof(picture))&&old==123);}
    free(p);return 0;
}
