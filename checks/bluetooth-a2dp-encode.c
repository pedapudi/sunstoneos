/* Generate real SBC fixtures with the pinned upstream encoder. */
#include <sbc/sbc.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
int main(int argc,char **argv)
{
    assert(argc==3);uint8_t config[4]={0x11,0x15,2,53};unsigned frames=3;
    if(argv[1][0]=='s'){config[0]=0x18;config[1]=0x89;frames=15;}
    sbc_t encoder;assert(!sbc_init_a2dp(&encoder,0,config,4));encoder.bitpool=20;encoder.endian=SBC_LE;
    FILE *file=fopen(argv[2],"wb");assert(file);size_t code=sbc_get_codesize(&encoder);
    for(unsigned f=0;f<frames;++f){int16_t pcm[256];uint8_t encoded[512];
        for(unsigned i=0;i<code/2;++i)pcm[i]=(int16_t)(8000*sin(2*3.141592653589793*440*(f*code/2+i)/48000.0));
        ssize_t written;assert(sbc_encode(&encoder,pcm,code,encoded,sizeof(encoded),&written)==(ssize_t)code);
        assert(fwrite(encoded,1,(size_t)written,file)==(size_t)written);}
    fclose(file);sbc_finish(&encoder);return 0;
}
