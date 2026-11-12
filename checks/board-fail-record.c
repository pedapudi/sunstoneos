/* Which failure notes reach the vector's first-failure call (apps/board/fail_record.h): the first, and one device failure after an
 * application's. Groups: 4 application, 8 NPU, 7 board. */
#include <assert.h>
#include <stdio.h>
#include "../apps/board/fail_record.h"

static atomic_uint recorded, first, second;
static unsigned note(unsigned group) { return (unsigned)board_fail_record_wanted(&recorded, &first, &second, (uint32_t)group << 12 | 0x1e5u | 0x80000000u); }
static void reset(void) { recorded = first = second = 0; }

int main(void)
{
    /* the failure return: the application's failure, then the NPU's, then more */
    reset(); assert(note(4) == 1 && note(8) == 1 && note(8) == 0 && note(7) == 0 && note(4) == 0);
    /* Registered owners retain one secondary RTC record. Their separate
     * descriptor records preserve the remaining faults. */
    reset(); assert(note(4) == 1 && note(11) == 1 && note(11) == 0);
    /* a second application failure (the producer after the main task) is not a device failure */
    reset(); assert(note(4) == 1 && note(4) == 0 && note(8) == 1 && note(8) == 0);
    /* a device failure first keeps its record against every later one */
    reset(); assert(note(8) == 1 && note(7) == 0 && note(4) == 0 && note(8) == 0);
    reset(); assert(note(7) == 1 && note(8) == 0);
    puts("board-fail-record host check passed");
    return 0;
}
