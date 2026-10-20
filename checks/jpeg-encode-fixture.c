/* Build a host fixture from the pinned LP64 encoder API, without calling it. */
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include "vencoder.h"
int main(int argc,char **argv) {
 if(argc!=3)return 2;
 VencBaseConfig c={0};c.nInputWidth=c.nDstWidth=c.nStride=640;c.nInputHeight=c.nDstHeight=480;c.eInputFormat=VENC_PIXEL_YUV420SP;
 c.memops=(void *)(uintptr_t)0x10001000;c.veOpsS=(void *)(uintptr_t)0x10002000;c.pVeOpsSelf=(void *)(uintptr_t)0x10003000;
 VencInputBuffer b={0};b.nID=17;b.nPts=73;b.pAddrPhyY=b.pAddrVirY=(void *)(uintptr_t)0x10800000;b.pAddrPhyC=b.pAddrVirC=(void *)(uintptr_t)(0x10800000+640*480);b.nWidth=640;b.nHeight=480;b.nAlign=16;
 _Static_assert(sizeof(void *)==8&&offsetof(VencBaseConfig,veOpsS)==40&&offsetof(VencInputBuffer,pAddrPhyY)==24,"LP64 fixture ABI");
 FILE *f=fopen(argv[1],"wb");if(!f)return 3;fwrite(&c,1,sizeof(c),f);fclose(f);f=fopen(argv[2],"wb");if(!f)return 3;fwrite(&b,1,sizeof(b),f);fclose(f);
 printf("{\"config_bytes\":%zu,\"input_bytes\":%zu,\"width_offset\":%zu,\"format\":%u}\n",sizeof(c),sizeof(b),offsetof(VencBaseConfig,nInputWidth),c.eInputFormat);return 0;
}
