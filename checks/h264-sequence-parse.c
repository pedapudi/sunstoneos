/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "a733_video_h264.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv)
{
    assert(argc==3);uint8_t input[A733_H264_INPUT_BYTES];FILE *f=fopen(argv[1],"rb");assert(f);
    size_t bytes=fread(input,1,sizeof(input),f);assert(fgetc(f)==EOF&&!fclose(f));
    struct a733_h264_sequence sequence,before;memset(&before,0xa5,sizeof(before));sequence=before;
    int result=a733_h264_parse_sequence(input,bytes,&sequence);
    if(!strcmp(argv[2],"invalid")){assert(!result&&!memcmp(&sequence,&before,sizeof(before)));return 0;}
    assert(result);
    if(sequence.count==1){struct a733_h264_picture picture;assert(a733_h264_parse(input,bytes,&picture)&&!memcmp(&picture,&sequence.pictures[0],sizeof(picture)));}
    printf("%u\n",sequence.count);
    for(unsigned i=0;i<sequence.count;++i){
        const struct a733_h264_picture *p=&sequence.pictures[i];
        printf("%u %u %u %u %u %u %u %u %u %u %u %u %u\n",p->width,p->height,p->slice_offset,p->slice_bytes,p->header_bits,p->sps,p->pps,p->shs,p->shs2,p->qp,p->frame_num,p->poc,p->slice_type);
    }
}
