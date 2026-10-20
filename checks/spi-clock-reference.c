/* Reuse the real clock/pin/platform fixture; run only changed parent cases. */
#define main spi_previous_platform_cases
#include "a733-spi-platform.c"
#undef main
static void reference_case(enum a733_spi_route route,uint32_t osc,uint32_t ref,int accepted)
{
 struct fixture f;init(&f,route);
 *cell(&f,0x07090160)=osc;*cell(&f,0x02002000)=ref;
 enum a733_spi_platform_result result=advance(&f,accepted?A733_SPI_PLATFORM_READY:A733_SPI_PLATFORM_RETAINED);
 assert(result==(accepted?A733_SPI_PLATFORM_READY:A733_SPI_PLATFORM_RETAINED));
 if(accepted)assert(f.platform.spi.plan.actual_hz==500000&&!f.platform.fault);
 else assert(f.platform.clock.id&&f.platform.fault&&!f.transactions);
}
int main(void)
{
 for(unsigned route=A733_SPI0_PC;route<=A733_SPI3_PE;++route){
  reference_case(route,0x10008002,0xf8675f00,1); /* 26*96/104 = 24MHz. */
  reference_case(route,0x10004002,0xf84f6300,1); /* 19.2*100/80 = 24MHz. */
  reference_case(route,0x10008002,0xf8636300,0);
 }
 reference_case(A733_S_SPI_PL,0x10008002,0xf8675f00,0);
 reference_case(A733_S_SPI_PM,0x10004002,0xf84f6300,0);
 puts("Main SPI accepts normalized24; S_SPI source0 rejects raw19.2/26; invalid reference refused");
}
