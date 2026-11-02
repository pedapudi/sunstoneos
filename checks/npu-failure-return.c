/* Execute the actual finite NPU return writer (apps/npu-finite/return.c) with the
 * failure return linked. The application state, the clock and the platform log are
 * modeled; the receipt, the refusal records and the failure record are produced by
 * the production code. */
#include "../apps/npu-finite/return.c"
struct a733_observe_identity a733_observe_identity;
struct a733_npu_application_result a733_npu_application_result;
unsigned fixture_quiet,fixture_ready,fixture_stride=64,fixture_status,fixture_smc_calls,fixture_crumbs;
uint64_t fixture_now=1000;
uint64_t a733_smp_counter(void){return fixture_now;}
unsigned a733_npu_application_quiescent(void){return fixture_quiet;}
int npu_startup_return_ready(void){return fixture_ready;}
unsigned aarch64_device_cache_stride(void){return fixture_stride;}
void platform_puts(const char *p)
{
    volatile uint64_t *length=(volatile uint64_t *)(uintptr_t)0x42000018;
    volatile char *log=(volatile char *)(uintptr_t)0x42000100;
    while(*p){log[(*length)++]=*p++;}
}
void platform_hex(uint64_t value)
{
    char text[19]="0x";static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<16;++i)text[2+i]=hex[(value>>(60-4*i))&15];text[18]=0;platform_puts(text);
}
__attribute__((noinline)) _Noreturn void npu_finite_smc(unsigned status){fixture_status=status;++fixture_smc_calls;for(;;){}}
_Noreturn void fixture_run(unsigned status)
{
    a733_observe_identity=(struct a733_observe_identity){7,8,9,0,480000000};
    atomic_store(&a733_npu_application_result.complete,fixture_ready?NPU_FINITE_COMPLETE(status>1?1:status):0);
    platform_exit(status);
}
