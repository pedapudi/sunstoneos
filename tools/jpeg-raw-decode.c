/* Decode a baseline 4:2:0 JPEG with libjpeg's raw-data path: no upsampling, no colour conversion.
 * Writes the active Y, Cb, Cr samples, row by row, in that order. */
#include <stdio.h>
#include <stdlib.h>
#include <jpeglib.h>
int main(int argc,char **argv)
{
    if(argc!=4)return 2;
    FILE *in=fopen(argv[1],"rb"),*out=fopen(argv[2],"wb");if(!in||!out)return 2;
    struct jpeg_decompress_struct c;struct jpeg_error_mgr e;
    c.err=jpeg_std_error(&e);jpeg_create_decompress(&c);jpeg_stdio_src(&c,in);jpeg_read_header(&c,TRUE);
    c.raw_data_out=TRUE;c.out_color_space=JCS_YCbCr;c.do_fancy_upsampling=FALSE;
    c.dct_method=atoi(argv[3]);
    jpeg_start_decompress(&c);
    unsigned w=c.image_width,h=c.image_height,cw=(w+1)/2,ch=(h+1)/2;
    unsigned char *plane[3];unsigned pw[3]={(w+15)&~15u,((w+15)&~15u)/2,((w+15)&~15u)/2},ph[3]={(h+15)&~15u,((h+15)&~15u)/2,((h+15)&~15u)/2};
    for(int i=0;i<3;i++)plane[i]=calloc(pw[i],ph[i]);
    while(c.output_scanline<c.output_height){
        JSAMPROW rows[3][16];JSAMPARRAY arr[3]={rows[0],rows[1],rows[2]};
        unsigned base=c.output_scanline;
        for(int r=0;r<16;r++)rows[0][r]=plane[0]+(base+r)*pw[0];
        for(int r=0;r<8;r++){rows[1][r]=plane[1]+(base/2+r)*pw[1];rows[2][r]=plane[2]+(base/2+r)*pw[2];}
        jpeg_read_raw_data(&c,arr,16);
    }
    jpeg_finish_decompress(&c);
    for(unsigned y=0;y<h;y++)fwrite(plane[0]+y*pw[0],1,w,out);
    for(int i=1;i<3;i++)for(unsigned y=0;y<ch;y++)fwrite(plane[i]+y*pw[i],1,cw,out);
    fclose(out);return 0;
}
