/* Card selection uses the admitted fixed record; no parameter edit during scan. */
#include <stdint.h>
#define CONFIG ((const uint32_t *)0x47ac8)
extern uint32_t card_admit(const uint8_t *, uint32_t);
extern uint32_t restore_write(void), restore_check(uint32_t);
extern void card_read(uint32_t, uint32_t, uint8_t *);
extern void card_stop(uint32_t) __attribute__((noreturn));
#define MAIL ((volatile uint32_t *)0x86000)
static uint32_t word(const uint8_t *p) { return *(const uint32_t *)p; }
static uint32_t crc(const uint8_t *p, uint32_t n)
{
    uint32_t c=~0u;
    while(n--) {
        c ^= *p++;
        for(uint32_t b=0;b<8;++b) c=(c>>1)^(0xedb88320u&(0u-(c&1)));
    }
    return ~c;
}
static uint32_t load(uint32_t sector, uint8_t *r)
{
    card_read(sector,1,r);
    if(word(r)!=0x33375341 || word(r+4)!=0x4e525444 || word(r+8)!=1 || word(r+12)!=128
       || crc(r,124)!=word(r+124) || !(word(r+24)|word(r+28))) return 0;
    uint32_t n=word(r+16);
    if(!n || n>0x6000-128) return 0;
    for(uint32_t i=0;i<(n+511)/512;++i) card_read(sector+1+i,1,r+128+512*i);
    return card_admit(r,0)==0;
}
uint32_t card_select(void)
{
    uint8_t *a=(uint8_t *)0xa0000,*b=(uint8_t *)0xa7000;
    uint32_t va=load(CONFIG[3],a),vb=load(CONFIG[4],b);
    MAIL[17]=va; MAIL[18]=vb;
    if(!va && !vb) return 0;
    if(vb && (!va || word(b+28)>word(a+28) ||
               (word(b+28)==word(a+28) && word(b+24)>word(a+24)))) {
        for(uint32_t i=0;i<128+word(b+16);++i) a[i]=b[i];
        MAIL[19]=2;
    } else MAIL[19]=1;
    if(card_admit(a,1)) card_stop(14);
    MAIL[14]=1;
    return 1;
}
uint32_t card_finish(uint32_t size)
{
    uint32_t error=restore_check(size);
    if(error) card_stop(error);
    MAIL[14]=0;
    return size;
}
