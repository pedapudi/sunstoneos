/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "a733_video_h264.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv)
{
    assert(argc==2);uint8_t input[A733_H264_INPUT_BYTES];
    FILE *f=fopen(argv[1],"rb");assert(f);
    size_t n=fread(input,1,sizeof(input),f);assert(n&&fgetc(f)==EOF&&!fclose(f));
    struct a733_h264_picture p;
    assert(a733_h264_parse(input,n,&p));
    printf("%u %u %u %u %u %u %u %u %u %u\n",p.width,p.height,
        p.slice_offset,p.slice_bytes,p.header_bits,p.sps,p.pps,p.shs,p.shs2,p.qp);
    /* Invalid public bounds and every truncation through the IDR header must
     * fail without publishing a partial plan. */
    struct a733_h264_picture sentinel;memset(&sentinel,0xa5,sizeof(sentinel));
    struct a733_h264_picture result=sentinel;
    assert(!a733_h264_parse(input,4097,&result)&&!memcmp(&result,&sentinel,sizeof(result)));
    assert(!a733_h264_parse(NULL,n,&result)&&!a733_h264_parse(input,n,NULL));
    for(size_t k=0;k<=p.slice_offset;++k){
        result=sentinel;
        assert(!a733_h264_parse(input,k,&result)&&!memcmp(&result,&sentinel,sizeof(result)));
    }
    /* Feed damaged syntax with ASan/UBSan, keeping the externally supplied
     * length equal to the actual allocated byte count. Acceptance is permitted
     * for damage confined to hardware-decoded slice data. */
    uint32_t seed=1;
    for(unsigned iteration=0;iteration<10000;++iteration){
        uint8_t damaged[A733_H264_INPUT_BYTES];memcpy(damaged,input,n);
        seed=seed*1664525u+1013904223u;size_t k=seed%n;
        seed=seed*1664525u+1013904223u;damaged[k]^=(uint8_t)(seed>>24);
        seed=seed*1664525u+1013904223u;
        (void)a733_h264_parse(damaged,seed%(n+1),&result);
    }
    return 0;
}
