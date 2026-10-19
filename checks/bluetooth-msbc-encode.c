/* Produce H2 fixture bytes with the same pinned upstream encoder as native TX.
 * This host utility establishes inputs, not controller or radio behavior. */
#include <sbc/sbc.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
int main(int argc,char **argv)
{
    assert(argc==3);FILE *in=fopen(argv[1],"rb"),*out=fopen(argv[2],"wb");assert(in&&out);
    sbc_t encoder;assert(!sbc_init_msbc(&encoder,0));encoder.endian=SBC_LE;
    const uint8_t sequence[]={8,0x38,0xc8,0xf8};uint8_t pcm[240];unsigned n=0;size_t count;
    while((count=fread(pcm,1,sizeof(pcm),in))){
        assert(count==sizeof(pcm));uint8_t frame[60]={1};ssize_t written=0;frame[1]=sequence[n++&3];
        assert(sbc_encode(&encoder,pcm,sizeof(pcm),frame+2,57,&written)==240&&written==57);
        assert(fwrite(frame,1,sizeof(frame),out)==sizeof(frame));
    }
    assert(n&&!ferror(in)&&!fclose(in)&&!fclose(out));sbc_finish(&encoder);return 0;
}
