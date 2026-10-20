/* Host-only LP64 records from the pinned public encoder API. No codec calls. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "vencoder.h"
static int save(const char *directory,const char *name,const void *data,size_t bytes)
{char path[4096];if(snprintf(path,sizeof(path),"%s/%s",directory,name)>=(int)sizeof(path))return 0;FILE *f=fopen(path,"wb");return f&&fwrite(data,1,bytes,f)==bytes&&!fclose(f);}
int main(int argc,char **argv)
{
 if(argc!=2&&argc!=3)return 2;
 VencBaseConfig c={0};c.nInputWidth=c.nDstWidth=c.nStride=640;c.nInputHeight=c.nDstHeight=480;c.eInputFormat=VENC_PIXEL_YUV420SP;
 c.memops=(void *)(uintptr_t)0x10001000;c.veOpsS=(void *)(uintptr_t)0x10002000;c.pVeOpsSelf=(void *)(uintptr_t)0x10003000;
 VencInputBuffer b={0};b.nID=17;b.nPts=0;b.pAddrPhyY=b.pAddrVirY=(void *)(uintptr_t)0x12000000;b.pAddrPhyC=b.pAddrVirC=(void *)(uintptr_t)(0x12000000+640*480);b.nWidth=640;b.nHeight=480;b.nAlign=16;
 VencH264Param p={0};p.sProfileLevel.nProfile=VENC_H264ProfileBaseline;p.sProfileLevel.nLevel=VENC_H264Level31;p.bEntropyCodingCABAC=0;
 p.sQPRange=(VencQPRange){.nMaxqp=26,.nMinqp=26,.nMaxPqp=26,.nMinPqp=26,.nQpInit=26};p.nFramerate=p.nSrcFramerate=30;p.nBitrate=2000000;p.nMaxKeyInterval=30;p.nCodingMode=VENC_FRAME_CODING;
 p.sGopParam.bUseGopCtrlEn=1;p.sGopParam.eGopMode=AW_NORMALP;
 p.sRcParam.eRcMode=AW_FIXQP;p.sRcParam.sFixQp=(VencFixQP){1,26,26};
 VencFixQP q={1,26,26};int vbv=4194304;
 _Static_assert(sizeof(void *)==8&&sizeof(VencBaseConfig)==80&&sizeof(VencInputBuffer)==376,"Pinned LP64 ABI");
 if(!save(argv[1],"config.bin",&c,sizeof(c))||!save(argv[1],"frame.bin",&b,sizeof(b))||!save(argv[1],"parameters.bin",&p,sizeof(p))||!save(argv[1],"fixqp.bin",&q,sizeof(q))||!save(argv[1],"vbv.bin",&vbv,sizeof(vbv)))return 3;
 if(argc==3){b.nID=18;b.nPts=33333;b.pAddrPhyY=b.pAddrVirY=(void *)(uintptr_t)0x12100000;b.pAddrPhyC=b.pAddrVirC=(void *)(uintptr_t)(0x12100000+640*480);FILE *f=fopen(argv[2],"wb");if(!f||fwrite(&b,1,sizeof(b),f)!=sizeof(b)||fclose(f))return 4;}
 printf("{\"config_bytes\":%zu,\"frame_bytes\":%zu,\"parameter_bytes\":%zu,\"param_index\":%u,\"fixqp_index\":%u,\"vbv_index\":%u,\"format\":%u,\"rc_mode\":%u,\"gop\":%u}\n",sizeof(c),sizeof(b),sizeof(p),VENC_IndexParamH264Param,VENC_IndexParamH264FixQP,VENC_IndexParamSetVbvSize,c.eInputFormat,p.sRcParam.eRcMode,p.sGopParam.eGopMode);return 0;
}
