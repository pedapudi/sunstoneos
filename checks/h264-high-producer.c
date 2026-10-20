/* Host syntax producer: actual bounded parser/DPB, no pixel decoding/device. */
#include "a733_h264_high.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv)
{
    void *storage=calloc(1,H264_HIGH_METADATA_BYTES);struct h264_high *h=h264_high_init(storage,H264_HIGH_METADATA_BYTES);
    uint8_t *data=malloc(4194305);if(!h||!data||argc<2||argc>257)return 2;
    puts("[");
    for(int i=1;i<argc;++i){FILE *f=fopen(argv[i],"rb");if(!f)return 3;size_t n=fread(data,1,4194305,f);if(ferror(f)||!feof(f)||fclose(f)||n>4194304)return 3;
        struct h264_high_plan p;size_t config;
        if(!h264_high_prepare(h,data,n,&p,&config)||!h264_high_commit(h,&p))return 4;
        const struct a733_h264_picture *q=&p.picture;
        printf("%s{\"frame_num\":%u,\"poc\":%d,\"kind\":%u,\"reference\":%u,\"idr\":%u,\"gop\":%u,\"width\":%u,\"height\":%u,\"visible_width\":%u,\"visible_height\":%u,\"crop\":[%u,%u,%u,%u],\"refs\":%u,\"reorder\":%u,\"dpb\":%u,\"config_bytes\":%zu,\"slot\":%u,\"list0\":[",i>1?",\n":"",q->frame_num,(int32_t)q->poc,q->slice_type,p.reference,p.idr,p.gop,q->width,q->height,q->visible_width,q->visible_height,q->crop_left,q->crop_top,q->crop_right,q->crop_bottom,p.slots-1,p.reorder,p.dpb_limit,config,p.slot);
        for(unsigned k=0;k<p.ref_count[0];++k)printf("%s%u",k?",":"",p.lists[0][k]);printf("],\"list1\":[");for(unsigned k=0;k<p.ref_count[1];++k)printf("%s%u",k?",":"",p.lists[1][k]);printf("]}");
    }
    puts("\n]");free(data);free(storage);return 0;
}
