/* Memory-bus priority hook against a counting register file, and the decode
 * check. Built twice by checks/dram-controller.py, with SUNSTONE_DRAM_PRIORITY
 * 2 (table) and 1 (reads only), together with drivers/dram/a733_dram.c and
 * drivers/dram/a733_dram_decode.c. */
#include "a733_dram.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define TABLE (SUNSTONE_DRAM_PRIORITY==2)
#define CCU_ON (UINT32_C(0xc0000000))
#define CFG_ON (UINT32_C(0x00010001))
static const unsigned ports[3]={2,14,15},pri[3]={2,3,3};
struct model {
    uint32_t clock,config,word[3][3]; /* Port index, field index (+0x10, +0x14, +0x18). */
    unsigned reads,writes,ccu,nsi,last;     /* last: highest port*3+field touched, plus 1. */
    int stuck,fail_at,access,outside;       /* stuck: field index+1 whose writes are dropped. */
    uintptr_t log[64];
};
static uint32_t *reg(struct model *m,uintptr_t a,unsigned *index)
{
    assert(a733_dram_address(a));
    if(a==A733_NSI_CLOCK){++m->ccu;return &m->clock;}
    if(a==A733_NSI_CONFIG){++m->ccu;return &m->config;}
    for(unsigned p=0;p<3;++p)for(unsigned f=0;f<3;++f)
        if(a==A733_NSI_PORT(ports[p])+0x10+4*f){
            ++m->nsi;*index=3*p+f;if(3*p+f+1>m->last)m->last=3*p+f+1;return &m->word[p][f];
        }
    assert(!"unexpected address");return NULL;
}
static int rd(void *v,uintptr_t a,uint32_t *out)
{
    struct model *m=v;unsigned i=0;
    assert(m->access<64);m->log[m->access]=a;
    if(++m->access==m->fail_at)return 0;
    ++m->reads;*out=*reg(m,a,&i);
    /* Port-word bits outside every field change between reads. */
    if(m->outside&&a!=A733_NSI_CLOCK&&a!=A733_NSI_CONFIG)*out^=UINT32_C(0x80000000);
    return 1;
}
static int wr(void *v,uintptr_t a,uint32_t value)
{
    struct model *m=v;unsigned i=0;
    assert(m->access<64);m->log[m->access]=a;
    if(++m->access==m->fail_at)return 0;
    ++m->writes;uint32_t *r=reg(m,a,&i);
    if(m->stuck!=(int)i+1)*r=value;
    return 1;
}
static void setup(struct model *m,uint32_t other)
{
    memset(m,0,sizeof(*m));m->clock=CCU_ON|0x03000002;m->config=CFG_ON;
    for(unsigned p=0;p<3;++p)for(unsigned f=0;f<3;++f)m->word[p][f]=other;
}
static int run(struct model *m,struct a733_dram_block *e)
{
    const struct a733_dram_io io={m,rd,wr};
    return a733_dram_priority(&io,e);
}
static uint16_t packed(uint32_t mode,uint32_t priority,uint32_t select)
{return (uint16_t)((mode&3)|(priority&15)<<2|(select&1)<<6);}
int main(void)
{
    struct model m;struct a733_dram_block e;
    /* No register source for the decode exists, so the check refuses and
     * stage 2 stays compiled out of every admitted layout. */
    assert(a733_dram_decode_check()==0);
    /* Address set: the two CCU words and nine port words, nothing adjacent. */
    unsigned admitted=0;
    for(uintptr_t a=0x02002000;a<0x02004000;a+=4)admitted+=(unsigned)a733_dram_address(a);
    for(uintptr_t a=A733_NSI_BASE;a<A733_NSI_BASE+0x10000;a+=4)admitted+=(unsigned)a733_dram_address(a);
    assert(admitted==11 && !a733_dram_address(A733_NSI_PORT(2)+0x11) && !a733_dram_address(A733_NSI_PORT(2)+0xc0));

    /* NSI not running: gate 31 or reset 30 of +0x580, or either bit of +0x584. */
    static const uint32_t clocks[]={CCU_ON&~(1u<<31),CCU_ON&~(1u<<30),CCU_ON,CCU_ON};
    static const uint32_t configs[]={CFG_ON,CFG_ON,CFG_ON&~1u,CFG_ON&~(1u<<16)};
    for(unsigned i=0;i<4;++i){
        setup(&m,0);m.clock=clocks[i];m.config=configs[i];
        assert(!run(&m,&e) && m.ccu==2 && !m.nsi && !e.failed && !(e.result&A733_DRAM_HOOK_PASSED) && e.nsi_clock==clocks[i] &&
            e.nsi_config==configs[i]);
    }
    /* Reset values (all fields zero) with unrelated bits set in every word. */
    setup(&m,UINT32_C(0x5a5a5a50)&~UINT32_C(0xf));
    for(unsigned p=0;p<3;++p){m.word[p][0]&=~3u;m.word[p][2]&=~1u;}
    uint32_t before[3][3];memcpy(before,m.word,sizeof(before));
    assert(run(&m,&e) && !e.failed && m.ccu==2);
    for(unsigned p=0;p<3;++p){
        assert(e.inherited[p]==packed(0,0,0));
        if(TABLE){
            assert(e.final[p]==packed(0,pri[p]<<2|pri[p],1));
            assert(m.word[p][0]==before[p][0]); /* Mode already 0: read only. */
            assert(m.word[p][1]==(before[p][1]|(pri[p]<<2|pri[p])) && m.word[p][2]==(before[p][2]|1));
        }else assert(e.final[p]==e.inherited[p] && !memcmp(m.word,before,sizeof(before)));
    }
    /* Per field: 1 access at the target, else read, write, read back. */
    assert(TABLE ? m.nsi==3+6*3 && m.writes==6 : m.nsi==9 && !m.writes);
    /* Every field at the table: nine reads, no writes. The addresses, in
     * order, go to the source comparison. */
    setup(&m,0);for(unsigned p=0;p<3;++p){m.word[p][1]=pri[p]<<2|pri[p];m.word[p][2]=1;}
    assert(run(&m,&e) && m.nsi==9 && !m.writes && m.access==11);
    uintptr_t order[11];memcpy(order,m.log,sizeof(order));
    /* Worst case: every field differs, 27 NSI accesses. */
    setup(&m,0);for(unsigned p=0;p<3;++p){m.word[p][0]=3;m.word[p][1]=0;m.word[p][2]=0;}
    assert(run(&m,&e) && m.nsi==(TABLE?27u:9u) && m.ccu==2);
    if(TABLE)for(unsigned p=0;p<3;++p)assert(!(m.word[p][0]&3) && e.inherited[p]==packed(3,0,0));
    /* Unequal priority subfields 3 and 1 with p = 3 become 0xf. */
    setup(&m,0);m.word[1][1]=UINT32_C(0xfff0)|(3u<<2)|1u;
    assert(run(&m,&e) && e.inherited[1]==packed(0,0xd,0));
    if(TABLE)assert(m.word[1][1]==UINT32_C(0xffff) && e.final[1]==packed(0,0xf,1));
    else assert(m.word[1][1]==(UINT32_C(0xfff0)|0xd));
    /* A difference outside the field mask is not a mismatch. */
    setup(&m,0);m.outside=1;assert(run(&m,&e) && !e.failed);
    /* Reads-only images write nothing; the table image stops at the first
     * mismatch and leaves every later field untouched. */
    for(unsigned i=0;i<9;++i){
        setup(&m,0);m.stuck=(int)i+1;
        /* Make the stuck field differ from the table. */
        if(i%3==0)m.word[i/3][0]=1;
        int ok=run(&m,&e);
        if(!TABLE){assert(ok && !m.writes);continue;}
        assert(!ok && e.failed==i+1 && m.last==i+1);
        assert(m.log[m.access-1]==A733_NSI_PORT(ports[i/3])+0x10+4*(i%3));
    }
    /* A failed access at any position stops the hook with no further access.
     * A failed CCU read has its own code, so it differs from the NSI being
     * off (failed 0). */
    setup(&m,0);assert(run(&m,&e));unsigned total=(unsigned)m.access;
    unsigned stopped=0;
    for(int k=1;k<=(int)total;++k){
        setup(&m,0);m.fail_at=k;
        assert(!run(&m,&e) && m.access==k && !(e.result&A733_DRAM_HOOK_PASSED));
        assert(k<=2 ? e.failed==A733_DRAM_CCU_FAILED && !m.nsi : e.failed>=stopped && e.failed>=1 && e.failed<=9);
        if(k>2)stopped=e.failed;
    }
    /* Evidence block outside the load images: the hook zeroes it and fills
     * the hook fields and the result bits from the build and its result. */
    struct a733_dram_block value;memset(&value,0xee,sizeof(value));
    setup(&m,0);int ok=run(&m,&value);assert(ok);
    const uint8_t *block=(const uint8_t *)&value; /* The host and the board are little-endian. */
    for(unsigned i=0;i<16;++i)assert(!block[i]);
    assert(block[16]==(TABLE?7:5) && !block[17] && !block[18] && !block[19]);
    for(unsigned p=0;p<3;++p){
        assert((block[20+2*p]|block[21+2*p]<<8)==packed(0,0,0));
        assert((block[26+2*p]|block[27+2*p]<<8)==(TABLE?packed(0,pri[p]<<2|pri[p],1):packed(0,0,0)));
    }
    uint32_t w0=(uint32_t)block[40]|(uint32_t)block[41]<<8|(uint32_t)block[42]<<16|(uint32_t)block[43]<<24;
    uint32_t w1=(uint32_t)block[44]|(uint32_t)block[45]<<8|(uint32_t)block[46]<<16|(uint32_t)block[47]<<24;
    assert(w0==m.clock && w1==m.config);
    for(unsigned i=32;i<40;++i)assert(!block[i]);
    for(unsigned i=48;i<sizeof(value);++i)assert(!block[i]);
    /* A failed hook clears the result bit and carries its stopping field. */
    /* The read of port 14's +0x14 word: after 2 CCU reads, port 2's 7 table
     * accesses (3 reads only) and port 14's mode read. */
    setup(&m,0);m.fail_at=TABLE?11:7;
    ok=run(&m,&value);assert(!ok && value.failed==5);
    assert(block[16]==(TABLE?3:1) && block[18]==5 && !block[19]);
    setup(&m,0);m.fail_at=2;
    ok=run(&m,&value);assert(!ok && block[16]==(TABLE?3:1) && block[18]==A733_DRAM_CCU_FAILED && !block[19]);

    /* Observed facts for the source comparison in checks/dram-controller.py.
     * CCU: the bits whose clearing alone refuses the hook. Ports: in the table
     * build, each field starts as the complement of the check's target, so
     * the bits the hook flips are the field mask and the bits it leaves set
     * in the mask are the driver's target. Every other field starts at the
     * table, so the hook writes once. Each field's entry also carries
     * the address of the one write, 0 in the reads-only build. */
    uint32_t need[2]={0,0};
    for(unsigned word=0;word<2;++word)for(unsigned bit=0;bit<32;++bit){
        setup(&m,0);m.clock=m.config=UINT32_MAX;
        if(word)m.config&=~(1u<<bit);else m.clock&=~(1u<<bit);
        if(!run(&m,&e))need[word]|=1u<<bit;
    }
    printf("{\"mode\":%d,\"accesses_reset\":%u,\"accesses_at_table\":9,\"worst\":%u,"
        "\"ccu\":[[%u,%u],[%u,%u]],\"order\":[",SUNSTONE_DRAM_PRIORITY,TABLE?21u:9u,TABLE?27u:9u,
        (unsigned)A733_NSI_CLOCK,need[0],(unsigned)A733_NSI_CONFIG,need[1]);
    for(unsigned i=0;i<11;++i)printf("%s%u",i?",":"",(unsigned)order[i]);
    printf("],\"fields\":[");
    for(unsigned p=0;p<3;++p)for(unsigned f=0;f<3;++f){
        uint32_t want=f==0?0:f==1?pri[p]<<2|pri[p]:1,start=~want;
        setup(&m,0);
        for(unsigned q=0;q<3;++q){m.word[q][1]=pri[q]<<2|pri[q];m.word[q][2]=1;}
        m.word[p][f]=start;assert(run(&m,&e) && m.writes==(TABLE?1u:0u));
        uint32_t mask=TABLE?m.word[p][f]^start:0,value=m.word[p][f]&mask;
        uintptr_t wrote=0;
        if(TABLE){assert(m.access==13);wrote=m.log[3+3*p+f];}
        printf("%s[%u,%u,%u,%u,%u]",p||f?",":"",ports[p],0x10+4*f,mask,value,(unsigned)wrote);
    }
    printf("]}\n");
    printf("PASS memory-bus priority hook, mode %d: NSI clock gating, per-field access counts, "
        "kept bits, read-back mismatch, failed accesses, the evidence block, observed register facts and the decode refusal\n",SUNSTONE_DRAM_PRIORITY);
    return 0;
}
