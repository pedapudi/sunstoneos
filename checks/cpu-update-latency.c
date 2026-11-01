/* SPDX-License-Identifier: Apache-2.0 */
/* Reuse the real CPU/PMIC/rail/thermal transition fixture, excluding its
 * unrelated cases. The driver is compiled separately by register-latency.py. */
#define main cpu_clock_full_fixture_main
#include "cpu-clock-model.c"
#undef main

int main(void)
{
    static const unsigned delays[]={103,257,1003};
    for(unsigned i=0;i<sizeof(delays)/sizeof(*delays);++i) {
        setup(0x00000100,1,30,30,30,&both);
        cm.update_us=delays[i];cm.lock_us=delays[i]+30;
        assert(heat(50000)==A733_THERMAL_NORMAL);
        for(unsigned pass=0;pass<2;++pass) {
            static const unsigned rates[]={1196,416};
            for(unsigned j=0;j<2;++j) {
                assert(a733_cpu_clock_request(&cpu,A733_CPU_LITTLE,rates[j],pm.now+1000000)
                    ==A733_CPU_CLOCK_PENDING);
                enum a733_cpu_clock_status status=run(1000);
#ifdef EXPECT_PREMATURE_VERIFY
                /* The mutant faults while UPDATE still permits the old N.
                 * Record that originating observation, before later refusal. */
                uint32_t observed=*word(PLL(cpu.cluster));
                assert(status==A733_CPU_CLOCK_RETAINED && cpu.failed);
                assert(cm.update[cpu.cluster].pending && (observed&(1u<<26)));
                assert(((observed>>8)&255)!=cpu.n);
                printf("origin=multiplier-before-update domain=%u observed=%08x expected_n=%u "
                    "now_us=%llu update_start=%llu update_delay=%llu\n",cpu.cluster,observed,cpu.n,
                    (unsigned long long)pm.now,(unsigned long long)cm.update[cpu.cluster].start,
                    (unsigned long long)cm.update[cpu.cluster].delay);
                break;
#else
                assert(status==A733_CPU_CLOCK_COMPLETE && !cpu.failed);
                assert(cpu.applied_mhz[A733_CPU_LITTLE]==rates[j] && released());
#endif
            }
#ifdef EXPECT_PREMATURE_VERIFY
            break;
#endif
        }
    }
#ifdef EXPECT_PREMATURE_VERIFY
    puts("CPU UPDATE: premature verification reproduced at three latencies");
#else
    puts("CPU UPDATE: three latencies, repeated raise/lower transitions");
#endif
    return 0;
}
