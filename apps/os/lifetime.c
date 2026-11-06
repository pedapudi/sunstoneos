/* The admitted lifetime (enum a733_lifetime) for code that runs at EL3 or EL1 (docs/designs/one-os-image.md,
 * "Lifetime word and sealed session"); 0 before admission. At EL3 it is the monitor word that admission writes from the
 * verified contract magic, and while admission decodes a contract's session, the lifetime that contract's magic names.
 * At EL1 it is the copy in the sealed session, which EL1 cannot write. EL0 never calls this: it cannot read CurrentEL. */
#include "os.h"
#include "../board/session.h"
extern unsigned os_admitting_lifetime;
/* Not inlined: EL1 has no mapping of monitor memory, so its loads must stay behind the exception-level test. */
__attribute__((noinline)) static unsigned el3_lifetime(void)
{
    unsigned admitting=*(volatile unsigned *)&os_admitting_lifetime;
    return admitting?admitting:*(volatile unsigned *)&a733_smp_lifetime;
}
unsigned os_lifetime(void)
{
#ifdef __aarch64__
    uint64_t el;__asm__ volatile("mrs %0, CurrentEL":"=r"(el));
    return el==12?el3_lifetime():board_session_lifetime();
#else
    /* The host pre-flight (tools/host_admission.py) runs the EL3 admission alone. */
    return el3_lifetime();
#endif
}
