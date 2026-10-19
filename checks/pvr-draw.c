#include "pvr_render_draw.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv)
{
    assert(argc==2);
    unsigned char *payload=malloc(PVR_RENDER_PROGRAM_BYTES),*reference=malloc(PVR_RENDER_PROGRAM_BYTES);
    assert(payload && reference);
    struct pvr_render_program_image image;
    assert(pvr_render_programs_build(payload,PVR_RENDER_PROGRAM_BYTES,&image));
    assert(pvr_render_draw_programs_build(payload,PVR_RENDER_PROGRAM_BYTES));
    FILE *f=fopen(argv[1],"rb");assert(f);
    assert(fread(reference,1,PVR_RENDER_PROGRAM_BYTES,f)==PVR_RENDER_PROGRAM_BYTES && fgetc(f)==EOF);assert(!fclose(f));
    for(unsigned i=0;i<PVR_RENDER_PROGRAM_BYTES;++i) {
        unsigned out=8192+PVR_RENDER_OUTPUT_OFFSET;
        if(i>=out && i<out+PVR_RENDER_OUTPUT_BYTES)continue;
        if(payload[i]!=reference[i]){fprintf(stderr,"draw producer mismatch at %u: %02x != %02x\n",i,payload[i],reference[i]);abort();}
    }
    uint32_t vertices[PVR_RENDER_DRAW_VERTICES*8]={0};
    for(unsigned i=0;i<PVR_RENDER_DRAW_VERTICES;++i){vertices[8*i+3]=0x3f800000;vertices[8*i+7]=0x3f800000;}
    assert(pvr_render_draw_vertices_valid(vertices,sizeof(vertices),126));
    assert(pvr_render_draw_vertices_valid(vertices,96,3));
    assert(!pvr_render_draw_vertices_valid(vertices,sizeof(vertices)-1,126));
    assert(!pvr_render_draw_vertices_valid(vertices,sizeof(vertices),127));
    for(unsigned i=0;i<8;++i) {
        uint32_t saved=vertices[i];vertices[i]=0x7f800000;
        assert(!pvr_render_draw_vertices_valid(vertices,sizeof(vertices),126));vertices[i]=saved;
    }
    vertices[0]=0xbf800000;vertices[1]=0x3f800000;vertices[2]=0x80000000;vertices[4]=0x80000000;
    assert(pvr_render_draw_vertices_valid(vertices,sizeof(vertices),126));
    vertices[4]=0xbf000000;assert(!pvr_render_draw_vertices_valid(vertices,sizeof(vertices),126));
    free(reference);free(payload);
    puts("PASS draw: complete pinned compiler/PDS/PPP/VDM payload, immutable spans and numeric admission");
    return 0;
}
