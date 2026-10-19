#include "nbg_checksum.h"
#include "nbg_model.h"
#include "nbg_assembly.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t word(const unsigned char *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void put(unsigned char *p,uint32_t v)
{ for(unsigned i=0;i<4;++i)p[i]=(unsigned char)(v>>(8*i)); }
static uint32_t primary(const unsigned char *p)
{
    uint32_t value=UINT32_MAX;
    for(unsigned n=0;n<word(p+56);++n) {
        const unsigned char *entry=p+60+n*44;
        for(size_t at=word(entry+4);at<(size_t)word(entry+4)+word(entry+12);++at) {
            unsigned char b=(at>=856 && at<880 && ((at-856)&7)>=4) ? 0 : p[at];
            value^=b;
            for(unsigned bit=0;bit<8;++bit)value=(value>>1)^((value&1)?0xedb88320u:0);
        }
    }
    return ~value;
}
static void invalid(unsigned char *p,size_t n)
{
    struct nbg_checksum out={1,2},saved=out;
    assert(nbg_checksum_verify(p,n,&out)==NBG_CHECKSUM_INVALID);
    assert(!memcmp(&saved,&out,sizeof(out)));
}
int main(int argc,char **argv)
{
    assert(argc==5); /* Actual padded model, old model, repaired gap, valid zero CRC. */
    unsigned cases=0;
    for(unsigned fixture=0;fixture<4;++fixture) {
        FILE *f=fopen(argv[fixture+1],"rb");assert(f);
        assert(!fseek(f,0,SEEK_END));long len=ftell(f);rewind(f);
        assert(len>880 && len<8192);size_t n=(size_t)len;
        unsigned char *p=malloc(n+1),*saved=malloc(n);assert(p && saved);
        assert(fread(p,1,n,f)==n);fclose(f);memcpy(saved,p,n);
        struct nbg_checksum out;
        assert(nbg_checksum_verify(p,n,&out)==NBG_CHECKSUM_MATCH);
        assert(out.computed==primary(p));++cases;
        if(fixture==3)assert(out.computed==0);
        if(!fixture) {
            assert(n==2720 && word(p+12)==0xe864f719);
            for(size_t at=2084;at<2112;++at)assert(!p[at]);
            /* Excluded bytes cannot hide arbitrary data; both ends are checked. */
            for(unsigned at=2084;at<=2111;at+=27) {
                p[at]=0xa5;invalid(p,n);p[at]=0;++cases;
            }
            p[n]=0;put(p+16,(uint32_t)n+1);invalid(p,n+1);++cases;memcpy(p,saved,n);
            /* Payload order is a separate admission rule outside CRC coverage. */
            unsigned char entry[44];memcpy(entry,p+60,44);memcpy(p+60,p+104,44);memcpy(p+104,entry,44);
            invalid(p,n);++cases;memcpy(p,saved,n);
            /* Header/directory edits may not make payloads alias or escape. */
            put(p+64+7*44,2084);invalid(p,n);++cases;memcpy(p,saved,n);
            put(p+64+11*44,2084);invalid(p,n);++cases;memcpy(p,saved,n);
            /* Coefficient table offsets remain relative to the coefficient
             * payload: interpreting an excluded gap as data must be refused. */
            put(p+2044,UINT32_MAX);invalid(p,n);++cases;memcpy(p,saved,n);
            /* Repairing the accumulated CRC cannot replace any listed CRC. */
            const unsigned payloads[]={1352,2112,2624};
            for(unsigned i=0;i<3;++i) {
                p[payloads[i]]^=1;put(p+12,primary(p));
                assert(nbg_checksum_verify(p,n,&out)==NBG_CHECKSUM_MISMATCH);
                ++cases;memcpy(p,saved,n);
            }
            struct nbg_model m;struct nbg_assembly_plan plan;
            assert(nbg_model_open(p,n,&m)==NBG_DECODE_OK && nbg_assembly_requirements(&m,&plan));
            assert(plan.nn_operations==2 && plan.descriptor_patches==8 && plan.state_patches==2);
            assert(plan.command_body_bytes==104 && !plan.buffers[NBG_ALLOC_POOL].allocation_bytes);
            ++cases;
        }
        assert(!memcmp(p,saved,n));free(saved);free(p);
    }
    printf("PASS padded directory and malformed boundaries: %u focused cases\n",cases);
}
