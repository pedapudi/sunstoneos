/* Use the descriptor fixture with only the changed hash-method cases. */
#define main prior_modes_main
#include "a733-ce-modes.c"
#undef main
int main(int argc,char **argv)
{
    assert(argc==2);FILE *f=fopen(argv[1],"r");assert(f);tables();
    static struct model m;static char line[20000];unsigned cases=0;
    setup(&m);
    while (fgets(line,sizeof(line),f)) {
        char *at=line;
        if (!strncmp(at,"k512 ",5)) {
            at+=5;for (unsigned i=0;i<80;++i) k512[i]=strtoull(at,&at,16);
        } else if (!strncmp(at,"initial ",8)) {
            at+=8;unsigned method=(unsigned)strtoul(at,&at,10);assert(method<7);
            for (unsigned i=0;i<8;++i) initial[method][i]=strtoull(at,&at,16);
        } else {
            unsigned method;char key[2200];size_t length;uint8_t bytes[1100]={0};
            assert(sscanf(line,"hash %u %2199s %zu",&method,key,&length)==3);
            size_t n=strcmp(key,"*") && strcmp(key,"-")?unhex(key,bytes,sizeof(bytes)):0;
            hash_case(&m,method,strcmp(key,"*")?bytes:0,n,length);++cases;
        }
    }
    fclose(f);
    for (unsigned method=0;method<=6;method+=6) {
        setup(&m);uint64_t generation=0;uint8_t output[64];memset(output,0xa5,sizeof(output));
        assert(a733_ce_hash_start(&m.hash,method,NULL,0,10000,&generation)==A733_CE_HASH_READY);
        assert(a733_ce_hash_poll_digest(&m.hash,generation,output,17)==A733_CE_HASH_INVALID);
        assert(a733_ce_hash_final(&m.hash,generation)==A733_CE_HASH_PENDING);
        m.fail_status=1;enum a733_ce_hash_result result=A733_CE_HASH_PENDING;
        for (unsigned i=0;i<10 && result==A733_CE_HASH_PENDING;++i) {
            if (m.pending) complete(&m);
            result=a733_ce_hash_poll_digest(&m.hash,generation,output,a733_ce_digest_size(method));
        }
        assert(result==A733_CE_HASH_RETAINED);unchanged(output,sizeof(output));
        zeroed(m.hash.buffer,sizeof(m.hash.buffer));zeroed(m.hash.digest,sizeof(m.hash.digest));
        struct device_memory_extent e;enum device_memory_state state;
        assert(device_memory_describe(&m.pool,m.lease,&e,&state) && state==DEVICE_MEMORY_RETAINED);
    }
    setup(&m);uint64_t generation=77;uint8_t data[64]={0};
    for (unsigned i=0;i<2;++i) {
        enum a733_ce_hash_method bad=(enum a733_ce_hash_method)(i?UINT32_MAX:7);
        assert(a733_ce_hash_start(&m.hash,bad,NULL,0,10000,&generation)==A733_CE_HASH_INVALID);
        assert(a733_ce_digest_request(&m.session.controller,bad,data,64,NULL,0,512,10000)==A733_CE_INVALID);
    }
    assert(generation==77 && !m.starts && !m.session.client);
    printf("PASS %u changed-method streams and retained failure/refusal checks\n",cases);
}
