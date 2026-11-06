/* The admitted lifetime on the virtual board, in place of apps/os/lifetime.c. The virtual board runs the EL3
 * admission at EL1 with every page mapped, so the exception level cannot tell admission from the OS: before the session
 * is sealed this is the lifetime under admission, or the admitted one, and afterwards the sealed copy. */
#include "os.h"
#include "../board/session.h"
extern unsigned os_admitting_lifetime;
unsigned os_lifetime(void)
{
    unsigned sealed=board_session_lifetime();
    if(sealed)return sealed;
    unsigned admitting=*(volatile unsigned *)&os_admitting_lifetime;
    return admitting?admitting:*(volatile unsigned *)&a733_smp_lifetime;
}
