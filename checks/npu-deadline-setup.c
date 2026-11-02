/* Execute the finite image's real shared-timer setup (apps/npu-finite/deadline.c) with a
 * modeled redistributor. configASSERT ends at platform_panic, which records its line. */
#include "../apps/npu-finite/deadline.c"
unsigned fixture_panic_line;
const char *fixture_panic_file;
uint64_t fixture_now=1000;
uint64_t a733_smp_counter(void){fixture_now+=100000;return fixture_now;}
__attribute__((noinline)) _Noreturn void platform_panic(const char *file,unsigned line)
{fixture_panic_file=file;fixture_panic_line=line;for(;;){}}
__attribute__((noinline)) void fixture_done(void){}
void fixture_setup(void){npu_finite_deadline_setup();fixture_done();}
